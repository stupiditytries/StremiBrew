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
