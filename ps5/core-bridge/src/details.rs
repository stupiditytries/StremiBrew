//! A title's details for the UI: what the details screen shows about a film or series,
//! its episodes, and the streams add-ons offer for it.

use serde::Serialize;

use stremio_core::constants::{GENRES_LINK_CATEGORY, IMDB_LINK_CATEGORY};
use stremio_core::models::common::Loadable;
use stremio_core::models::meta_details::Selected;
use stremio_core::runtime::msg::{Action, ActionLoad};
use stremio_core::runtime::RuntimeAction;
use stremio_core::types::addon::ResourcePath;
use stremio_core::types::resource::{MetaItem, StreamSource};

use crate::model::{Ps5Model, Ps5ModelField};

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Details<'a> {
    /// "loading", "ready" or "error" (no add-on could describe the title).
    state: &'static str,
    id: &'a str,
    r#type: &'a str,
    name: &'a str,
    description: Option<&'a str>,
    background: Option<&'a str>,
    logo: Option<&'a str>,
    poster: Option<&'a str>,
    release_info: Option<&'a str>,
    runtime: Option<&'a str>,
    imdb_rating: Option<&'a str>,
    genres: Vec<&'a str>,
    cast: Vec<&'a str>,
    directors: Vec<&'a str>,
    videos: Vec<Video<'a>>,
    /// The video whose streams are loaded: an episode's id, or the title's own for a film.
    stream_video: Option<&'a str>,
    stream_groups: Vec<StreamGroup<'a>>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Video<'a> {
    id: &'a str,
    title: &'a str,
    season: Option<u32>,
    episode: Option<u32>,
    released: Option<String>,
    thumbnail: Option<&'a str>,
    overview: Option<&'a str>,
}

/// One add-on's answer for the selected video.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct StreamGroup<'a> {
    addon: &'a str,
    state: &'static str,
    streams: Vec<Stream<'a>>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Stream<'a> {
    name: Option<&'a str>,
    description: Option<&'a str>,
    /// The address to play, for streams this app can play: direct web links. Torrents,
    /// YouTube ids and external links have none.
    url: Option<&'a str>,
    /// Why a stream without an address cannot be played here.
    unsupported: Option<&'static str>,
    /// The file's size in bytes, when the add-on gives it.
    size: Option<u64>,
}

/// Loads a title's details. `video` selects the video to load streams for: an episode's
/// id for a series, or null to use the title's own id for a film and none for a series.
pub fn load(
    runtime: &stremio_core::runtime::Runtime<crate::Ps5Env, Ps5Model>,
    r#type: &str,
    id: &str,
    video: Option<&str>,
) {
    let stream_id = video.or(if r#type == "series" { None } else { Some(id) });
    runtime.dispatch(RuntimeAction {
        field: Some(Ps5ModelField::MetaDetails),
        action: Action::Load(ActionLoad::MetaDetails(Selected {
            meta_path: ResourcePath::without_extra("meta", r#type, id),
            stream_path: stream_id.map(|video| ResourcePath::without_extra("stream", r#type, video)),
            guess_stream: false,
        })),
    });
}

fn links<'a>(item: &'a MetaItem, category: &str) -> Vec<&'a str> {
    item.preview
        .links
        .iter()
        .filter(|link| link.category == category)
        .map(|link| link.name.as_str())
        .collect()
}

/// The details as the UI draws them, or `None` when no title is selected.
pub fn details(model: &Ps5Model) -> Option<Details<'_>> {
    let meta = &model.meta_details;
    let selected = meta.selected.as_ref()?;
    // The first add-on that could describe the title wins, as in Stremio's other apps.
    let ready = meta.meta_items.iter().find_map(|item| match &item.content {
        Some(Loadable::Ready(item)) => Some(item),
        _ => None,
    });
    let failed = !meta.meta_items.is_empty()
        && meta
            .meta_items
            .iter()
            .all(|item| matches!(item.content, Some(Loadable::Err(_))));

    let stream_groups = meta
        .streams
        .iter()
        .filter_map(|group| {
            let addon = model
                .ctx
                .profile
                .addons
                .iter()
                .find(|addon| addon.transport_url == group.request.base)?;
            let (state, streams) = match &group.content {
                Some(Loadable::Ready(streams)) => (
                    "ready",
                    streams
                        .iter()
                        .map(|stream| {
                            let (url, unsupported) = match &stream.source {
                                StreamSource::Url { url }
                                    if matches!(url.scheme(), "http" | "https") =>
                                {
                                    (Some(url.as_str()), None)
                                }
                                StreamSource::Url { .. } => (None, Some("Unsupported link")),
                                StreamSource::YouTube { .. } => (None, Some("YouTube")),
                                StreamSource::Torrent { .. } => (None, Some("Torrent")),
                                StreamSource::External { .. } => (None, Some("Opens elsewhere")),
                                _ => (None, Some("Unsupported")),
                            };
                            Stream {
                                name: stream.name.as_deref(),
                                description: stream.description.as_deref(),
                                url,
                                unsupported,
                                size: stream.behavior_hints.video_size,
                            }
                        })
                        .collect(),
                ),
                Some(Loadable::Err(_)) => ("error", vec![]),
                Some(Loadable::Loading) | None => ("loading", vec![]),
            };
            Some(StreamGroup {
                addon: &addon.manifest.name,
                state,
                streams,
            })
        })
        .collect();

    let path = &selected.meta_path;
    let stream_video = selected.stream_path.as_ref().map(|path| path.id.as_str());
    Some(match ready {
        Some(item) => Details {
            state: "ready",
            id: &item.preview.id,
            r#type: &item.preview.r#type,
            name: &item.preview.name,
            description: item.preview.description.as_deref(),
            background: item.preview.background.as_ref().map(|url| url.as_str()),
            logo: item.preview.logo.as_ref().map(|url| url.as_str()),
            poster: item.preview.poster.as_ref().map(|url| url.as_str()),
            release_info: item.preview.release_info.as_deref(),
            runtime: item.preview.runtime.as_deref(),
            imdb_rating: links(item, IMDB_LINK_CATEGORY).first().copied(),
            genres: links(item, GENRES_LINK_CATEGORY),
            cast: links(item, "Cast"),
            directors: links(item, "Directors"),
            videos: item
                .videos
                .iter()
                .map(|video| Video {
                    id: &video.id,
                    title: &video.title,
                    season: video.series_info.as_ref().map(|info| info.season),
                    episode: video.series_info.as_ref().map(|info| info.episode),
                    released: video.released.map(|date| date.format("%Y-%m-%d").to_string()),
                    thumbnail: video.thumbnail.as_deref(),
                    overview: video.overview.as_deref(),
                })
                .collect(),
            stream_video,
            stream_groups,
        },
        None => Details {
            state: if failed { "error" } else { "loading" },
            id: &path.id,
            r#type: &path.r#type,
            name: "",
            description: None,
            background: None,
            logo: None,
            poster: None,
            release_info: None,
            runtime: None,
            imdb_rating: None,
            genres: vec![],
            cast: vec![],
            directors: vec![],
            videos: vec![],
            stream_video,
            stream_groups,
        },
    })
}
