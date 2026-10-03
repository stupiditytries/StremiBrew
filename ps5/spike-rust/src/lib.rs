//! Phase 0.1 spike: exercises the parts of `std` that stremio-core's environment needs
//! (threads, files, sockets, DNS, clocks) behind a C ABI, so a C++ `main` can run each
//! check on the console and log the result.

use std::ffi::{c_char, CStr};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::{mpsc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Text describing why the most recent check failed (or what it found).
static DETAIL: Mutex<String> = Mutex::new(String::new());

fn set_detail(text: String) {
    if let Ok(mut detail) = DETAIL.lock() {
        *detail = text;
    }
}

fn c_str<'a>(value: *const c_char) -> &'a str {
    if value.is_null() {
        return "";
    }
    unsafe { CStr::from_ptr(value) }.to_str().unwrap_or("")
}

/// Copies the detail text of the most recent check into `out` (NUL-terminated, truncated
/// to `capacity`) and clears it.
#[no_mangle]
pub extern "C" fn spike_detail(out: *mut c_char, capacity: usize) {
    if out.is_null() || capacity == 0 {
        return;
    }
    let text = DETAIL.lock().map(|mut detail| std::mem::take(&mut *detail)).unwrap_or_default();
    let length = text.len().min(capacity - 1);
    unsafe {
        std::ptr::copy_nonoverlapping(text.as_ptr(), out as *mut u8, length);
        *out.add(length) = 0;
    }
}

/// Spawns `count` threads that each send their index back; returns how many answered.
#[no_mangle]
pub extern "C" fn spike_threads(count: u32) -> u32 {
    let (sender, receiver) = mpsc::channel();
    let handles: Vec<_> = (0..count)
        .map(|index| {
            let sender = sender.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_millis(10));
                let _ = sender.send(index);
            })
        })
        .collect();
    drop(sender);
    let answered = receiver.iter().count() as u32;
    for handle in handles {
        let _ = handle.join();
    }
    answered
}

/// Creates `dir`, then writes, renames, inspects, lists, reads back and removes a file in
/// it. Returns 0 on success, otherwise the number of the step that failed.
#[no_mangle]
pub extern "C" fn spike_files(dir: *const c_char) -> i32 {
    let dir = std::path::Path::new(c_str(dir));
    let temporary = dir.join("spike.tmp");
    let target = dir.join("spike.json");
    let payload = br#"{"spike":true}"#;
    let fail = |step: i32, error: &dyn std::fmt::Display| {
        set_detail(format!("step {step}: {error}"));
        step
    };
    if let Err(error) = std::fs::create_dir_all(dir) {
        return fail(1, &error);
    }
    if let Err(error) = std::fs::write(&temporary, payload) {
        return fail(2, &error);
    }
    if let Err(error) = std::fs::rename(&temporary, &target) {
        return fail(3, &error);
    }
    match std::fs::metadata(&target) {
        Ok(status) if status.is_file() && status.len() == payload.len() as u64 => {}
        Ok(status) => {
            return fail(4, &format!("is_file={} len={}", status.is_file(), status.len()))
        }
        Err(error) => return fail(4, &error),
    }
    match std::fs::metadata(dir) {
        Ok(status) if status.is_dir() => {}
        Ok(_) => return fail(5, &"directory is not reported as one"),
        Err(error) => return fail(5, &error),
    }
    let names: Vec<String> = match std::fs::read_dir(dir) {
        Ok(entries) => entries
            .filter_map(|entry| entry.ok())
            .map(|entry| entry.file_name().to_string_lossy().into_owned())
            .collect(),
        Err(error) => return fail(6, &error),
    };
    if !names.iter().any(|name| name == "spike.json") {
        return fail(6, &format!("listing was {names:?}"));
    }
    match std::fs::read(&target) {
        Ok(content) if content == payload => {}
        Ok(content) => return fail(7, &format!("read {} bytes", content.len())),
        Err(error) => return fail(7, &error),
    }
    if let Err(error) = std::fs::remove_file(&target) {
        return fail(8, &error);
    }
    0
}

/// Milliseconds since the Unix epoch, or 0 when the clock is unavailable.
#[no_mangle]
pub extern "C" fn spike_wall_clock_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|elapsed| elapsed.as_millis() as u64)
        .unwrap_or(0)
}

/// Sleeps `ms` milliseconds and returns how long the monotonic clock says it took.
#[no_mangle]
pub extern "C" fn spike_monotonic_ms(ms: u32) -> u64 {
    let start = Instant::now();
    std::thread::sleep(Duration::from_millis(ms as u64));
    start.elapsed().as_millis() as u64
}

/// Resolves `host`, connects to port 80, sends a plain HTTP request and returns the status
/// code of the reply. Negative values name the failed step: -1 resolve, -2 connect,
/// -3 connect with a timeout, -4 write, -5 read, -6 unparsable reply.
#[no_mangle]
pub extern "C" fn spike_http_status(host: *const c_char) -> i32 {
    let host = c_str(host);
    let address = match (host, 80).to_socket_addrs() {
        Ok(mut list) => match list.next() {
            Some(address) => address,
            None => {
                set_detail("resolver returned no address".into());
                return -1;
            }
        },
        Err(error) => {
            set_detail(format!("resolve: {error}"));
            return -1;
        }
    };
    if let Err(error) = TcpStream::connect(address) {
        set_detail(format!("connect {address}: {error}"));
        return -2;
    }
    let mut stream = match TcpStream::connect_timeout(&address, Duration::from_secs(10)) {
        Ok(stream) => stream,
        Err(error) => {
            set_detail(format!("connect_timeout {address}: {error}"));
            return -3;
        }
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(10)));
    let request = format!("GET / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n");
    if let Err(error) = stream.write_all(request.as_bytes()) {
        set_detail(format!("write: {error}"));
        return -4;
    }
    let mut reply = [0u8; 64];
    let length = match stream.read(&mut reply) {
        Ok(length) if length > 12 => length,
        Ok(length) => {
            set_detail(format!("read only {length} bytes"));
            return -5;
        }
        Err(error) => {
            set_detail(format!("read: {error}"));
            return -5;
        }
    };
    set_detail(format!("address {address}"));
    std::str::from_utf8(&reply[..length])
        .ok()
        .and_then(|text| text.split(' ').nth(1))
        .and_then(|code| code.parse().ok())
        .unwrap_or(-6)
}
