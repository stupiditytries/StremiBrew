//! What the Switch (Horizon) needs that the crates the bridge uses do not provide.

/// Random bytes, for the crates that ask the `getrandom` crate for them: it does not
/// know the Switch, so it is given the console's own generator.
fn console_random(out: &mut [u8]) -> Result<(), getrandom::Error> {
    extern "C" {
        /// libnx: fills a buffer from the console's random number generator.
        fn randomGet(buffer: *mut core::ffi::c_void, length: usize);
    }
    unsafe { randomGet(out.as_mut_ptr().cast(), out.len()) };
    Ok(())
}

getrandom::register_custom_getrandom!(console_random);

/// A line for the app's log: the Switch has no log of its own an app can write to, so
/// the app keeps one in a file and takes the bridge's lines for it.
pub(crate) fn log(text: &str) {
    extern "C" {
        fn stremio_host_log(text: *const core::ffi::c_char);
    }
    if let Ok(line) = std::ffi::CString::new(text) {
        unsafe { stremio_host_log(line.as_ptr()) };
    }
}

/// Notes a request that failed in the app's log: the first few only, which is enough to
/// tell a console that cannot reach the network (or cannot make secure connections) from
/// one address being down.
pub(crate) fn note_failure(address: &str, error: &str) {
    use std::sync::atomic::{AtomicU32, Ordering};
    static NOTED: AtomicU32 = AtomicU32::new(0);
    if NOTED.fetch_add(1, Ordering::Relaxed) < 12 {
        // Without what follows a '?': addresses can carry an account's key there.
        let shown = address.split('?').next().unwrap_or(address);
        log(&format!("request failed: {shown}: {error}"));
    }
}
