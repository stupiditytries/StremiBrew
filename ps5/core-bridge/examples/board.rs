//! Runs the bridge on the PC through its C interface: starts the core, loads the board and
//! prints what arrived.
//!
//!   cargo run --release --example board -- <storage folder>

use std::ffi::CString;
use std::time::{Duration, Instant};

use stremio_core_ps5::{
    stremio_core_board_summary, stremio_core_init, stremio_core_last_error,
    stremio_core_load_board, stremio_core_poll_event,
};

fn text(fill: impl Fn(*mut std::ffi::c_char, usize) -> usize) -> String {
    let mut buffer = vec![0u8; 64 * 1024];
    let length = fill(buffer.as_mut_ptr().cast(), buffer.len()).min(buffer.len() - 1);
    String::from_utf8_lossy(&buffer[..length]).into_owned()
}

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| "/tmp/stremio-ps5-storage".to_owned());
    let dir = CString::new(dir).unwrap();
    if stremio_core_init(dir.as_ptr()) != 0 {
        eprintln!("init failed: {}", text(|out, cap| stremio_core_last_error(out, cap)));
        std::process::exit(1);
    }
    stremio_core_load_board(6);

    let start = Instant::now();
    let mut events = 0;
    loop {
        while !text(|out, cap| stremio_core_poll_event(out, cap)).is_empty() {
            events += 1;
        }
        let summary = text(|out, cap| stremio_core_board_summary(out, cap));
        let settled = summary.contains(" loading 0 ") && !summary.starts_with("rows 0 ");
        if (settled && start.elapsed() > Duration::from_secs(1))
            || start.elapsed() > Duration::from_secs(30)
        {
            println!("{events} events in {:.1} s\n{summary}", start.elapsed().as_secs_f32());
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}
