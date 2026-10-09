//! The few facts the board's featured area shows about a title (description, year,
//! length, rating, genres), for titles that come from the library, which keeps none of
//! them. They are fetched once from Cinemeta, Stremio's own catalogue, and remembered.

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Duration;

use once_cell::sync::Lazy;

#[derive(Clone, Default)]
pub struct Brief {
    pub description: Option<String>,
    pub release_info: Option<String>,
    pub runtime: Option<String>,
    pub imdb_rating: Option<String>,
    pub genres: Vec<String>,
}

/// By title id. An empty entry is one that is being fetched, or could not be.
static BRIEFS: Lazy<Mutex<HashMap<String, Option<Brief>>>> = Lazy::new(Default::default);

fn fetch(kind: &str, id: &str) -> Option<Brief> {
    let address = format!("https://v3-cinemeta.strem.io/meta/{kind}/{id}.json");
    let response = ureq::get(&address)
        .timeout(Duration::from_secs(20))
        .set("User-Agent", "stremio-ps5")
        .call()
        .ok()?;
    let answer: serde_json::Value = serde_json::from_reader(response.into_reader()).ok()?;
    let meta = answer.get("meta")?;
    let text = |key: &str| {
        meta.get(key)
            .and_then(|value| value.as_str())
            .filter(|value| !value.is_empty())
            .map(str::to_owned)
    };
    Some(Brief {
        description: text("description"),
        release_info: text("releaseInfo").or_else(|| text("year")),
        runtime: text("runtime"),
        imdb_rating: text("imdbRating"),
        genres: meta
            .get("genres")
            .and_then(|genres| genres.as_array())
            .map(|genres| {
                genres
                    .iter()
                    .filter_map(|genre| genre.as_str().map(str::to_owned))
                    .take(3)
                    .collect()
            })
            .unwrap_or_default(),
    })
}

/// Titles waiting to be looked up, and whether the thread that does it has been started.
static WAITING: Lazy<Mutex<(Vec<(String, String)>, bool)>> = Lazy::new(Default::default);

/// Looks the waiting titles up one after another. One thread does this for as long as
/// the app runs: a library can hold hundreds of titles, and a thread for each (as there
/// once was) runs the console out of threads.
fn work() {
    loop {
        let next = WAITING.lock().ok().and_then(|mut waiting| waiting.0.pop());
        let Some((kind, id)) = next else {
            std::thread::sleep(Duration::from_millis(200));
            continue;
        };
        #[allow(unused_mut)]
        let mut found = fetch(&kind, &id);
        // (The Switch's network fails a request now and then that works a moment later.)
        #[cfg(target_os = "horizon")]
        for _ in 0..3 {
            if found.is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(1500));
            found = fetch(&kind, &id);
        }
        #[cfg(target_os = "horizon")]
        if found.is_none() {
            crate::horizon::note_failure(&format!("the facts of {id}"), "not to be had from Cinemeta");
        }
        if let Some(brief) = found {
            if let Ok(mut briefs) = BRIEFS.lock() {
                briefs.insert(id, Some(brief));
            }
            crate::announce("{\"event\":\"brief\",\"fields\":[\"board\"]}");
        }
    }
}

/// What is known about a title. The first call for a title queues it to be fetched and
/// returns nothing; the board is announced as changed when the answer is in.
pub fn get(kind: &str, id: &str) -> Option<Brief> {
    let mut briefs = BRIEFS.lock().ok()?;
    if let Some(known) = briefs.get(id) {
        return known.clone();
    }
    briefs.insert(id.to_owned(), None);
    drop(briefs);
    if id.starts_with("tt") {
        if let Ok(mut waiting) = WAITING.lock() {
            waiting.0.push((kind.to_owned(), id.to_owned()));
            if !waiting.1 {
                // Should the thread not start, the titles simply go without these facts.
                waiting.1 = std::thread::Builder::new()
                    .name("briefs".to_owned())
                    .spawn(work)
                    .is_ok();
            }
        }
    }
    None
}
