//! A title's trailer as a video file the player can play.
//!
//! Stremio's catalogue names trailers by their YouTube id, which is no use here. IMDb's
//! own site plays its trailers from plain MP4 files and gets their addresses from the
//! service asked below. That service is not a published one: it wants the headers a
//! browser on imdb.com would send, and it may change. When it does not answer as
//! expected there is simply no trailer.

use std::time::Duration;

const SERVICE: &str = "https://caching.graphql.imdb.com/";
/// Trailers longer than this are passed over when there is a shorter one.
const LONGEST: u64 = 200;

/// The address of the best trailer for `id` (an IMDb id), or nothing.
pub fn find(id: &str) -> Option<String> {
    if !id.starts_with("tt") || !id[2..].bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    let query = format!(
        "query {{ title(id: \"{id}\") {{ primaryVideos(first: 12) {{ edges {{ node {{ \
         runtime {{ value }} contentType {{ id }} \
         playbackURLs {{ videoMimeType videoDefinition url }} }} }} }} }} }}"
    );
    let body = serde_json::json!({ "query": query }).to_string();
    let response = ureq::post(SERVICE)
        .timeout(Duration::from_secs(15))
        .set("Content-Type", "application/json")
        .set(
            "User-Agent",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) \
             Chrome/126.0 Safari/537.36",
        )
        .set("Origin", "https://www.imdb.com")
        .set("Referer", "https://www.imdb.com/")
        .send_string(&body)
        .ok()?;
    let answer: serde_json::Value = serde_json::from_reader(response.into_reader()).ok()?;
    let videos = answer
        .pointer("/data/title/primaryVideos/edges")?
        .as_array()?;

    // Each video's length, whether IMDb calls it a trailer (it also lists clips and
    // featurettes), and its best MP4: 1080p, else 720p, else 480p, else whatever MP4
    // there is; and whether that is a sharp one.
    let mut found: Vec<(bool, u64, bool, String)> = videos
        .iter()
        .filter_map(|edge| {
            let node = edge.get("node")?;
            let seconds = node.pointer("/runtime/value")?.as_u64()?;
            let trailer = node
                .pointer("/contentType/id")?
                .as_str()?
                .ends_with(".trailer");
            let files = node.get("playbackURLs")?.as_array()?;
            let mp4 = |definition: Option<&str>| {
                files.iter().find_map(|file| {
                    let kind = file.get("videoMimeType")?.as_str()?;
                    let size = file.get("videoDefinition")?.as_str()?;
                    (kind == "MP4" && definition.map_or(true, |wanted| size == wanted))
                        .then(|| file.get("url")?.as_str().map(str::to_owned))
                        .flatten()
                })
            };
            let full = mp4(Some("DEF_1080p"));
            let sharp = full.is_some();
            let url = full
                .or_else(|| mp4(Some("DEF_720p")))
                .or_else(|| mp4(Some("DEF_480p")))
                .or_else(|| mp4(None))?;
            Some((trailer, seconds, sharp, url))
        })
        .collect();
    // Trailers before anything else; among them the ones not overlong, then the ones to
    // be had in 1080p, then the longest (the short ones are usually teasers or scenes).
    found.sort_by_key(|(trailer, seconds, sharp, _)| {
        (!*trailer, *seconds > LONGEST, !*sharp, std::cmp::Reverse(*seconds))
    });
    found.into_iter().next().map(|(_, _, _, url)| url)
}
