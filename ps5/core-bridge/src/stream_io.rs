//! A file-like reader over a web address, for the video player: it reads forward through
//! one connection and, when asked for another part of the file, opens a new connection
//! that starts there (an HTTP range request).

use std::ffi::{c_char, CStr};
use std::io::Read;
use std::time::Duration;

use once_cell::sync::Lazy;

/// Unlike the agent for the core's own requests, this one puts no limit on how long a
/// response may take as a whole: a film is read for hours. A read that gets nothing for
/// this long is treated as a dropped connection.
static HTTP: Lazy<ureq::Agent> = Lazy::new(|| {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(15))
        .timeout_read(Duration::from_secs(20))
        .user_agent("stremio-ps5")
        .build()
});

/// Moving forward by no more than this is done by reading and discarding, which is
/// quicker than a new connection.
const SKIP_LIMIT: u64 = 1 << 20;
/// How many times in a row a failed read is retried on a new connection.
const RETRIES: u32 = 3;

pub struct HttpStream {
    url: String,
    /// Where the next read is wanted.
    position: u64,
    size: Option<u64>,
    /// The open response, and where in the file its next byte is.
    body: Option<Box<dyn Read + Send + Sync>>,
    body_position: u64,
}

impl HttpStream {
    fn open(url: &str) -> Option<Self> {
        let mut stream = HttpStream {
            url: url.to_owned(),
            position: 0,
            size: None,
            body: None,
            body_position: 0,
        };
        stream.connect().ok()?;
        Some(stream)
    }

    /// Opens a response that starts at `position`. `Ok(false)` means the position is at
    /// or past the end of the file.
    fn connect(&mut self) -> Result<bool, String> {
        self.body = None;
        let response = match HTTP
            .get(&self.url)
            .set("Range", &format!("bytes={}-", self.position))
            .call()
        {
            Ok(response) => response,
            Err(ureq::Error::Status(416, _)) => return Ok(false),
            Err(error) => return Err(error.to_string()),
        };
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
            self.body = Some(response.into_reader());
        } else {
            // The server ignored the range and is sending the file from its start.
            self.size = length.or(self.size);
            self.body_position = 0;
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
        let mut failures = 0;
        loop {
            let near = self.body.is_some()
                && self.position >= self.body_position
                && self.position - self.body_position <= SKIP_LIMIT;
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
                Some(Ok(count)) if count > 0 => {
                    self.position += count as u64;
                    self.body_position = self.position;
                    return Ok(count);
                }
                // An early end is a dropped connection when more of the file is known to
                // follow; otherwise it is the end of the file.
                Some(Ok(_)) if self.size.map_or(true, |size| self.position >= size) => {
                    return Ok(0)
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
    match HttpStream::open(url) {
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
