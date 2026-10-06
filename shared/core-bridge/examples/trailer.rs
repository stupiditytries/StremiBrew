//! Looks up the trailers of the IMDb ids given as arguments (a check of trailer.rs).

fn main() {
    for id in std::env::args().skip(1) {
        match stremio_core_ps5::trailer::find(&id) {
            Some(address) => println!("{id} {address}"),
            None => println!("{id}: no trailer"),
        }
    }
}
