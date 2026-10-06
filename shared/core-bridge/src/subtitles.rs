//! Subtitles from the account's add-ons (OpenSubtitles is one of Stremio's defaults).

use std::time::Duration;

use serde::Serialize;
use stremio_core::types::addon::ResourcePath;

use crate::model::Ps5Model;

#[derive(Serialize)]
pub struct Subtitle {
    addon: String,
    id: String,
    /// The language, as the add-on names it (usually a three-letter code).
    lang: String,
    url: String,
}

/// The add-ons to ask for a video's subtitles: each one's name and the address to ask.
pub fn sources(model: &Ps5Model, kind: &str, id: &str) -> Vec<(String, url::Url)> {
    let path = ResourcePath::without_extra("subtitles", kind, id);
    model
        .ctx
        .profile
        .addons
        .iter()
        .filter(|addon| addon.manifest.is_resource_supported(&path))
        .filter_map(|addon| {
            // An add-on's resources live beside its manifest.
            let mut address = addon.transport_url.clone();
            {
                let mut segments = address.path_segments_mut().ok()?;
                segments.pop();
                segments.push("subtitles").push(kind).push(&format!("{id}.json"));
            }
            Some((addon.manifest.name.clone(), address))
        })
        .collect()
}

/// Asks each add-on in turn. One that fails or is slow is left out.
pub fn fetch(sources: Vec<(String, url::Url)>) -> Vec<Subtitle> {
    let mut found = Vec::new();
    for (addon, address) in sources {
        let Ok(response) = ureq::get(address.as_str())
            .timeout(Duration::from_secs(20))
            .set("User-Agent", "stremio-ps5")
            .call()
        else {
            continue;
        };
        let Ok(answer) = serde_json::from_reader::<_, serde_json::Value>(response.into_reader()) else {
            continue;
        };
        let Some(list) = answer.get("subtitles").and_then(|list| list.as_array()) else {
            continue;
        };
        for entry in list {
            let text = |key: &str| entry.get(key).and_then(|value| value.as_str());
            if let (Some(url), Some(lang)) = (text("url"), text("lang")) {
                found.push(Subtitle {
                    addon: addon.clone(),
                    id: text("id").unwrap_or_default().to_owned(),
                    lang: lang.to_owned(),
                    url: url.to_owned(),
                });
            }
        }
    }
    found
}
