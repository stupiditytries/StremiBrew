//! Runs the bridge on the PC through its C interface: starts the core, loads the board and
//! prints what arrived.
//!
//!   cargo run --release --example board -- <storage folder> [board JSON output file]

use std::ffi::CString;
use std::time::{Duration, Instant};

use stremio_core_ps5::{
    stremio_core_account, stremio_core_account_advance, stremio_core_board_rows,
    stremio_core_board_summary, stremio_core_details, stremio_core_init,
    stremio_core_load_details, stremio_core_sign_in_start,
    stremio_core_last_error, stremio_core_load_board, stremio_core_poll_event,
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
            // The board's rows exactly as the app's UI receives them, for its PC preview.
            if let Some(output) = std::env::args().nth(2) {
                let mut buffer = vec![0u8; 4 << 20];
                let length =
                    stremio_core_board_rows(20, buffer.as_mut_ptr().cast(), buffer.len());
                std::fs::write(&output, &buffer[..length.min(buffer.len())]).unwrap();
                println!("wrote {length} bytes to {output}");
            }
            break;
        }
        std::thread::sleep(Duration::from_millis(100));
    }

    // "link" as the third argument instead: request a sign-in code and show the account's
    // state for a few seconds (without anyone entering the code, it stays "waiting").
    if std::env::args().nth(3).as_deref() == Some("link") {
        stremio_core_sign_in_start();
        for _ in 0..3 {
            std::thread::sleep(Duration::from_secs(2));
            stremio_core_account_advance();
            let account = text(|out, cap| stremio_core_account(out, cap));
            // The code is shown to whoever runs this; nothing else in the state is secret.
            println!("{account}");
        }
        return;
    }

    // Optionally a title's details too: <type> <id> [video id] [output file].
    let (Some(kind), Some(id)) = (std::env::args().nth(3), std::env::args().nth(4)) else {
        return;
    };
    let video = std::env::args().nth(5).filter(|video| !video.is_empty());
    let (kind, id) = (CString::new(kind).unwrap(), CString::new(id).unwrap());
    let video = video.map(|video| CString::new(video).unwrap());
    stremio_core_load_details(
        kind.as_ptr(),
        id.as_ptr(),
        video.as_ref().map_or(std::ptr::null(), |video| video.as_ptr()),
    );
    // Add-ons answer one by one; give them a few seconds.
    std::thread::sleep(Duration::from_secs(6));
    let mut buffer = vec![0u8; 8 << 20];
    let length = stremio_core_details(buffer.as_mut_ptr().cast(), buffer.len());
    let json = String::from_utf8_lossy(&buffer[..length.min(buffer.len())]).into_owned();
    match std::env::args().nth(6) {
        Some(output) => {
            std::fs::write(&output, &json).unwrap();
            println!("wrote {length} bytes of details to {output}");
        }
        None => println!("{json}"),
    }
}
