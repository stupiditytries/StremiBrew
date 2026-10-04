//! A file-like reader over a web address, for the video player: it reads forward through
//! one connection and, when asked for another part of the file, opens a new connection
//! that starts there (an HTTP range request).
//!
//! What has been read is also kept in a file on the console's disk (the newest couple of
//! gigabytes of it), and a read that falls inside it is served from there with no request
//! at all. That is what makes stepping back in a video, or forward into what has already
//! been read ahead, immediate.

use std::collections::{HashMap, VecDeque};
use std::ffi::{c_char, CStr};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::sync::Mutex;
use std::time::Duration;

use once_cell::sync::Lazy;

/// Unlike the agent for the core's own requests, this one puts no limit on how long a
/// response may take as a whole: a film is read for hours. A read that gets nothing for
/// this long is treated as a dropped connection.
static HTTP: Lazy<ureq::Agent> = Lazy::new(|| {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(15))
        .timeout_read(Duration::from_secs(20))
        // Several readers work on one video at once (playback, and the workers that
        // make the scrubbing pictures); each keeps its connection for its next request.
        .max_idle_connections(32)
        .max_idle_connections_per_host(24)
        .user_agent("stremio-ps5")
        .build()
});

/// Where each address really leads. An add-on's stream address usually only redirects to
/// the server that has the file (after looking the file up, which can take a second or
/// more); asking that server directly from the second request on saves the detour on
/// every seek.
static RESOLVED: Lazy<Mutex<HashMap<String, String>>> = Lazy::new(Default::default);

/// Moving forward by no more than this is done by reading and discarding, which is
/// quicker than a new connection.
const SKIP_LIMIT: u64 = 1 << 20;
/// How many times in a row a failed read is retried on a new connection.
const RETRIES: u32 = 3;

/// The cache keeps the file in blocks of this size.
const BLOCK: u64 = 1 << 20;

/// What has been read of one address, in a file used as a ring of blocks.
struct Cache {
    file: File,
    slots: usize,
    /// The address the blocks belong to; another address starts the cache afresh.
    url: String,
    /// For each block of the video that is held: its slot in the file and which parts of
    /// it have been filled in.
    blocks: HashMap<u64, Block>,
    /// The held blocks, oldest first.
    order: VecDeque<u64>,
}

/// A held block. Reading runs forward, so a block fills as one stretch, `begin..end`.
/// That stretch starts at the block's beginning unless reading began part-way through it
/// (the first block after a seek); if reading later arrives from the block before, the
/// missing start is filled in as `0..head` until it meets `begin`.
#[derive(Clone, Copy)]
struct Block {
    slot: usize,
    head: u64,
    begin: u64,
    end: u64,
}

static CACHE: Mutex<Option<Cache>> = Mutex::new(None);

/// What playback's reading has cost, for the app's log: bytes that came from the cache,
/// bytes that came from the network, connections opened, and the milliseconds those took
/// to answer.
static STATS: [std::sync::atomic::AtomicU64; 4] = [
    std::sync::atomic::AtomicU64::new(0),
    std::sync::atomic::AtomicU64::new(0),
    std::sync::atomic::AtomicU64::new(0),
    std::sync::atomic::AtomicU64::new(0),
];

fn count(which: usize, amount: u64) {
    STATS[which].fetch_add(amount, std::sync::atomic::Ordering::Relaxed);
}

/// Copies the four counters (see `STATS`) to `out`.
#[no_mangle]
pub extern "C" fn stremio_http_stats(out: *mut u64) {
    if out.is_null() {
        return;
    }
    for (index, counter) in STATS.iter().enumerate() {
        unsafe { out.add(index).write(counter.load(std::sync::atomic::Ordering::Relaxed)) };
    }
}

impl Cache {
    fn claim(&mut self, url: &str) {
        if self.url != url {
            self.url = url.to_owned();
            self.blocks.clear();
            self.order.clear();
        }
    }

    fn read(&mut self, position: u64, buffer: &mut [u8]) -> Option<usize> {
        let block = *self.blocks.get(&(position / BLOCK))?;
        let offset = position % BLOCK;
        let until = if offset >= block.begin && offset < block.end {
            block.end
        } else if block.begin > 0 && offset < block.head {
            block.head
        } else {
            return None;
        };
        let count = buffer.len().min((until - offset) as usize);
        self.file
            .seek(SeekFrom::Start(block.slot as u64 * BLOCK + offset))
            .ok()?;
        self.file.read_exact(&mut buffer[..count]).ok()?;
        Some(count)
    }

    fn store(&mut self, slot: usize, offset: u64, data: &[u8]) -> bool {
        self.file
            .seek(SeekFrom::Start(slot as u64 * BLOCK + offset))
            .and_then(|_| self.file.write_all(data))
            .is_ok()
    }

    fn write(&mut self, mut position: u64, mut data: &[u8]) {
        while !data.is_empty() {
            let index = position / BLOCK;
            let offset = position % BLOCK;
            let count = data.len().min((BLOCK - offset) as usize);
            match self.blocks.get(&index).copied() {
                // The next part of the block's stretch.
                Some(mut block) if offset == block.end => {
                    if self.store(block.slot, offset, &data[..count]) {
                        block.end += count as u64;
                        self.blocks.insert(index, block);
                    }
                }
                // The next part of the block's missing start, up to where the stretch
                // begins; when the two meet the block is whole from its beginning.
                Some(mut block) if block.begin > 0 && offset == block.head => {
                    let fill = count.min((block.begin - block.head) as usize);
                    if self.store(block.slot, offset, &data[..fill]) {
                        block.head += fill as u64;
                        if block.head == block.begin {
                            block.begin = 0;
                            block.head = 0;
                        }
                        self.blocks.insert(index, block);
                    }
                }
                Some(_) => {} // already held, or not next to what is held
                None => {
                    // A new block takes a free slot, or the oldest block's.
                    let slot = if self.order.len() < self.slots {
                        self.order.len()
                    } else {
                        let oldest = self.order.pop_front().unwrap_or(index);
                        self.blocks.remove(&oldest).map_or(0, |block| block.slot)
                    };
                    if self.store(slot, offset, &data[..count]) {
                        self.order.push_back(index);
                        self.blocks.insert(
                            index,
                            Block {
                                slot,
                                head: 0,
                                begin: offset,
                                end: offset + count as u64,
                            },
                        );
                    }
                }
            }
            position += count as u64;
            data = &data[count..];
        }
    }
}

/// Names the file the cache uses and its size. Call once, before any stream is opened.
#[no_mangle]
pub extern "C" fn stremio_http_set_cache(path: *const c_char, megabytes: u32) {
    if path.is_null() {
        return;
    }
    let Ok(path) = unsafe { CStr::from_ptr(path) }.to_str() else {
        return;
    };
    let file = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(true)
        .open(path);
    match file {
        Ok(file) => {
            *CACHE.lock().unwrap() = Some(Cache {
                file,
                slots: megabytes.max(16) as usize,
                url: String::new(),
                blocks: HashMap::new(),
                order: VecDeque::new(),
            });
        }
        Err(error) => crate::kernel_log(&format!("stream cache not available: {error}")),
    }
}

pub struct HttpStream {
    url: String,
    /// Where the next read is wanted.
    position: u64,
    size: Option<u64>,
    /// The open response, and where in the file its next byte is.
    body: Option<Box<dyn Read + Send + Sync>>,
    body_position: u64,
    /// When set, the file is asked for this many bytes at a time instead of "from here
    /// to the end". A response read to its end leaves its connection usable for the next
    /// request, which a reader that jumps about the file (the scrubbing pictures) gains
    /// from: no new connection for each jump.
    piece: Option<u64>,
    /// Where the open response ends, when the file is read in pieces.
    body_end: u64,
}

impl HttpStream {
    fn open(url: &str, piece: Option<u64>) -> Option<Self> {
        let mut stream = HttpStream {
            url: url.to_owned(),
            position: 0,
            size: None,
            body: None,
            body_position: 0,
            piece,
            body_end: 0,
        };
        stream.connect().ok()?;
        if let Some(cache) = CACHE.lock().unwrap().as_mut() {
            cache.claim(url);
        }
        Some(stream)
    }

    /// Opens a response that starts at `position`. `Ok(false)` means the position is at
    /// or past the end of the file.
    fn connect(&mut self) -> Result<bool, String> {
        // What is left of a piece is read out, so its connection can be used again.
        if let (Some(_), Some(mut body)) = (self.piece, self.body.take()) {
            let _ = std::io::copy(&mut body, &mut std::io::sink());
        }
        self.body = None;
        let range = match self.piece {
            Some(piece) => format!("bytes={}-{}", self.position, self.position + piece - 1),
            None => format!("bytes={}-", self.position),
        };
        let asked = std::time::Instant::now();
        let direct = RESOLVED.lock().ok().and_then(|known| known.get(&self.url).cloned());
        let mut answer = HTTP
            .get(direct.as_deref().unwrap_or(&self.url))
            .set("Range", &range)
            .call();
        if direct.is_some() && matches!(answer, Err(ureq::Error::Status(code, _)) if code != 416) {
            // The direct address has stopped working (such links expire): back to the
            // add-on's own, which finds the file again.
            if let Ok(mut known) = RESOLVED.lock() {
                known.remove(&self.url);
            }
            answer = HTTP.get(&self.url).set("Range", &range).call();
        }
        let response = match answer {
            Ok(response) => response,
            Err(ureq::Error::Status(416, _)) => return Ok(false),
            Err(error) => return Err(error.to_string()),
        };
        if self.piece.is_none() {
            count(2, 1);
            count(3, asked.elapsed().as_millis() as u64);
        }
        if response.get_url() != self.url {
            if let Ok(mut known) = RESOLVED.lock() {
                known.insert(self.url.clone(), response.get_url().to_owned());
            }
        }
        let total = response
            .header("Content-Range")
            .and_then(|range| range.rsplit('/').next())
            .and_then(|total| total.trim().parse::<u64>().ok());
        let length = response
            .header("Content-Length")
            .and_then(|length| length.trim().parse::<u64>().ok());
        if response.status() == 206 {
            self.size = total.or(length.map(|length| self.position + length)).or(self.size);
            self.body_position = self.position;
            self.body_end = self.position + length.unwrap_or(u64::MAX / 2);
            self.body = Some(response.into_reader());
        } else {
            // The server ignored the range and is sending the file from its start.
            self.size = length.or(self.size);
            self.body_position = 0;
            self.body_end = u64::MAX / 2;
            self.piece = None;
            self.body = Some(response.into_reader());
            if self.position > 64 * SKIP_LIMIT {
                self.body = None;
                return Err("the server cannot start part-way through the file".to_owned());
            }
        }
        Ok(true)
    }

    /// Reads and discards up to the wanted position.
    fn skip(&mut self) -> bool {
        let mut scratch = [0u8; 16 * 1024];
        while self.body_position < self.position {
            let want = (self.position - self.body_position).min(scratch.len() as u64) as usize;
            match self.body.as_mut().map(|body| body.read(&mut scratch[..want])) {
                Some(Ok(count)) if count > 0 => self.body_position += count as u64,
                _ => return false,
            }
        }
        true
    }

    /// Reads into `buffer`. `Ok(0)` is the end of the file.
    fn read(&mut self, buffer: &mut [u8]) -> Result<usize, String> {
        if self.size.is_some_and(|size| self.position >= size) {
            return Ok(0);
        }
        if let Some(cache) = CACHE.lock().unwrap().as_mut() {
            if cache.url == self.url {
                if let Some(read) = cache.read(self.position, buffer) {
                    if self.piece.is_none() {
                        count(0, read as u64);
                    }
                    self.position += read as u64;
                    return Ok(read);
                }
            }
        }
        let mut failures = 0;
        loop {
            let near = self.body.is_some()
                && self.position >= self.body_position
                && self.position - self.body_position <= SKIP_LIMIT
                && (self.piece.is_none() || self.position < self.body_end);
            if !near {
                match self.connect() {
                    Ok(true) => {}
                    Ok(false) => return Ok(0),
                    Err(error) => {
                        failures += 1;
                        if failures > RETRIES {
                            return Err(error);
                        }
                        std::thread::sleep(Duration::from_millis(500));
                        continue;
                    }
                }
            }
            if self.body_position < self.position && !self.skip() {
                self.body = None;
                failures += 1;
                if failures > RETRIES {
                    return Err("the connection keeps dropping".to_owned());
                }
                continue;
            }
            match self.body.as_mut().map(|body| body.read(buffer)) {
                Some(Ok(read)) if read > 0 => {
                    // Only playback's own reading is kept: the scattered pieces the
                    // scrubbing pictures read would leave blocks that playback's reading
                    // could then not fill.
                    if self.piece.is_none() {
                        count(1, read as u64);
                        if let Some(cache) = CACHE.lock().unwrap().as_mut() {
                            if cache.url == self.url {
                                cache.write(self.position, &buffer[..read]);
                            }
                        }
                    }
                    self.position += read as u64;
                    self.body_position = self.position;
                    return Ok(read);
                }
                // An early end is a dropped connection when more of the file is known to
                // follow; otherwise it is the end of the file.
                Some(Ok(_)) if self.size.map_or(true, |size| self.position >= size) => {
                    return Ok(0)
                }
                // The end of a piece: the next one is asked for.
                Some(Ok(_)) if self.piece.is_some() && self.position >= self.body_end => {
                    self.body_end = 0;
                }
                Some(Ok(_)) | Some(Err(_)) | None => {
                    self.body = None;
                    failures += 1;
                    if failures > RETRIES {
                        return Err("the connection keeps dropping".to_owned());
                    }
                }
            }
        }
    }
}

/// Opens `url` for reading. Null when the address cannot be opened.
#[no_mangle]
pub extern "C" fn stremio_http_open(url: *const c_char) -> *mut HttpStream {
    if url.is_null() {
        return std::ptr::null_mut();
    }
    let Ok(url) = unsafe { CStr::from_ptr(url) }.to_str() else {
        return std::ptr::null_mut();
    };
    match HttpStream::open(url, None) {
        Some(stream) => Box::into_raw(Box::new(stream)),
        None => std::ptr::null_mut(),
    }
}

/// Opens `url` for a reader that jumps about the file: it is asked for `piece` bytes at a
/// time (see `HttpStream::piece`). Null when the address cannot be opened.
#[no_mangle]
pub extern "C" fn stremio_http_open_pieces(url: *const c_char, piece: u64) -> *mut HttpStream {
    if url.is_null() {
        return std::ptr::null_mut();
    }
    let Ok(url) = unsafe { CStr::from_ptr(url) }.to_str() else {
        return std::ptr::null_mut();
    };
    match HttpStream::open(url, Some(piece.max(64 * 1024))) {
        Some(stream) => Box::into_raw(Box::new(stream)),
        None => std::ptr::null_mut(),
    }
}

/// Reads up to `length` bytes. Returns how many were read, 0 at the end of the file, or
/// -1 when the connection failed and could not be re-established.
#[no_mangle]
pub extern "C" fn stremio_http_read(stream: *mut HttpStream, buffer: *mut u8, length: usize) -> i64 {
    if stream.is_null() || buffer.is_null() {
        return -1;
    }
    let (stream, buffer) =
        unsafe { (&mut *stream, std::slice::from_raw_parts_mut(buffer, length)) };
    match stream.read(buffer) {
        Ok(count) => count as i64,
        Err(error) => {
            crate::kernel_log(&format!("stream read failed: {error}"));
            -1
        }
    }
}

/// Sets where the next read starts. The new connection, if one is needed, is opened by
/// that read.
#[no_mangle]
pub extern "C" fn stremio_http_seek(stream: *mut HttpStream, position: u64) {
    if let Some(stream) = unsafe { stream.as_mut() } {
        stream.position = position;
    }
}

/// The file's size in bytes, or -1 when the server did not say.
#[no_mangle]
pub extern "C" fn stremio_http_size(stream: *const HttpStream) -> i64 {
    unsafe { stream.as_ref() }
        .and_then(|stream| stream.size)
        .map_or(-1, |size| size as i64)
}

#[no_mangle]
pub extern "C" fn stremio_http_close(stream: *mut HttpStream) {
    if !stream.is_null() {
        drop(unsafe { Box::from_raw(stream) });
    }
}

/// Fetches a small file (a subtitle) into `out`. Returns its length, or -1 when it could
/// not be fetched or does not fit.
#[no_mangle]
pub extern "C" fn stremio_http_get(url: *const c_char, out: *mut u8, capacity: usize) -> i64 {
    if url.is_null() || out.is_null() {
        return -1;
    }
    let Ok(url) = unsafe { CStr::from_ptr(url) }.to_str() else {
        return -1;
    };
    let Ok(response) = HTTP.get(url).timeout(Duration::from_secs(30)).call() else {
        return -1;
    };
    let mut bytes = Vec::new();
    if response
        .into_reader()
        .take(capacity as u64 + 1)
        .read_to_end(&mut bytes)
        .is_err()
        || bytes.len() > capacity
    {
        return -1;
    }
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len()) };
    bytes.len() as i64
}

/// Downloads a large file (a speech model) to `path`, by way of `path` + ".part" so a
/// download cut short never looks like the whole file. `done` and `total` are counters
/// the caller watches for progress: bytes so far, and the file's size once it is known.
/// Returns 0 when the file is in place.
#[no_mangle]
pub extern "C" fn stremio_http_download(
    url: *const c_char,
    path: *const c_char,
    done: *mut u64,
    total: *mut u64,
) -> i32 {
    if url.is_null() || path.is_null() || done.is_null() || total.is_null() {
        return -1;
    }
    let (Ok(url), Ok(path)) = (
        unsafe { CStr::from_ptr(url) }.to_str(),
        unsafe { CStr::from_ptr(path) }.to_str(),
    ) else {
        return -1;
    };
    let fetched = (|| -> Result<(), String> {
        let response = HTTP.get(url).call().map_err(|error| error.to_string())?;
        let size = response
            .header("Content-Length")
            .and_then(|length| length.trim().parse::<u64>().ok())
            .unwrap_or(0);
        unsafe { total.write_volatile(size) };
        let part = format!("{path}.part");
        let mut file = File::create(&part).map_err(|error| error.to_string())?;
        let mut body = response.into_reader();
        let mut buffer = vec![0u8; 256 * 1024];
        let mut written = 0u64;
        loop {
            let count = body.read(&mut buffer).map_err(|error| error.to_string())?;
            if count == 0 {
                break;
            }
            file.write_all(&buffer[..count]).map_err(|error| error.to_string())?;
            written += count as u64;
            unsafe { done.write_volatile(written) };
        }
        drop(file);
        if size != 0 && written != size {
            let _ = std::fs::remove_file(&part);
            return Err(format!("the download stopped at {written} of {size} bytes"));
        }
        std::fs::rename(&part, path).map_err(|error| error.to_string())
    })();
    match fetched {
        Ok(()) => 0,
        Err(error) => {
            crate::kernel_log(&format!("download failed: {error}"));
            -1
        }
    }
}
