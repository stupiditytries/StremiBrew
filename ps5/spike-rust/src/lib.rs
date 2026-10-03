//! Phase 0.1 spike: exercises the parts of `std` that stremio-core's environment needs
//! (threads, files, sockets, DNS, clocks) behind a C ABI, so a C++ `main` can run each
//! check on the console and log the result.

use std::ffi::{c_char, CStr};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::mpsc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn c_str<'a>(value: *const c_char) -> &'a str {
    if value.is_null() {
        return "";
    }
    unsafe { CStr::from_ptr(value) }.to_str().unwrap_or("")
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

/// Writes, renames, reads back and removes a file under `dir`. Returns 0 on success,
/// otherwise the number of the step that failed.
#[no_mangle]
pub extern "C" fn spike_files(dir: *const c_char) -> i32 {
    let dir = std::path::Path::new(c_str(dir));
    let temporary = dir.join("spike.tmp");
    let target = dir.join("spike.json");
    let payload = br#"{"spike":true}"#;
    if std::fs::create_dir_all(dir).is_err() {
        return 1;
    }
    if std::fs::write(&temporary, payload).is_err() {
        return 2;
    }
    if std::fs::rename(&temporary, &target).is_err() {
        return 3;
    }
    match std::fs::read(&target) {
        Ok(content) if content == payload => {}
        _ => return 4,
    }
    if std::fs::read_dir(dir).map(|entries| entries.count()).unwrap_or(0) == 0 {
        return 5;
    }
    if std::fs::remove_file(&target).is_err() {
        return 6;
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
/// -3 write, -4 read, -5 unparsable reply.
#[no_mangle]
pub extern "C" fn spike_http_status(host: *const c_char) -> i32 {
    let host = c_str(host);
    let address = match (host, 80).to_socket_addrs().ok().and_then(|mut list| list.next()) {
        Some(address) => address,
        None => return -1,
    };
    let mut stream = match TcpStream::connect_timeout(&address, Duration::from_secs(10)) {
        Ok(stream) => stream,
        Err(_) => return -2,
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(10)));
    let request = format!("GET / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n");
    if stream.write_all(request.as_bytes()).is_err() {
        return -3;
    }
    let mut reply = [0u8; 64];
    let length = match stream.read(&mut reply) {
        Ok(length) if length > 12 => length,
        _ => return -4,
    };
    std::str::from_utf8(&reply[..length])
        .ok()
        .and_then(|text| text.split(' ').nth(1))
        .and_then(|code| code.parse().ok())
        .unwrap_or(-5)
}
