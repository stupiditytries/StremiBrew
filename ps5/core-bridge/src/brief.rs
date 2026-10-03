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

/// What is known about a title. The first call for a title starts fetching it and returns
/// nothing; the board is announced as changed when the answer is in.
pub fn get(kind: &str, id: &str) -> Option<Brief> {
    let mut briefs = BRIEFS.lock().ok()?;
    if let Some(known) = briefs.get(id) {
        return known.clone();
    }
    briefs.insert(id.to_owned(), None);
    drop(briefs);
    if id.starts_with("tt") {
        let (kind, id) = (kind.to_owned(), id.to_owned());
        std::thread::spawn(move || {
            if let Some(brief) = fetch(&kind, &id) {
                if let Ok(mut briefs) = BRIEFS.lock() {
                    briefs.insert(id, Some(brief));
                }
                crate::announce("{\"event\":\"brief\",\"fields\":[\"board\"]}");
            }
        });
    }
    None
}
