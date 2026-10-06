//! The calendar and the list of installed add-ons, as the UI draws them.

use serde::Serialize;

use stremio_core::models::calendar::Selected;
use stremio_core::models::common::Loadable;
use stremio_core::runtime::msg::{Action, ActionLoad};
use stremio_core::runtime::{Runtime, RuntimeAction};

use crate::model::{Ps5Model, Ps5ModelField};
use crate::Ps5Env;

type Core = Runtime<Ps5Env, Ps5Model>;

/// One month of the calendar: the episodes of the library's series released on each of
/// its days.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Calendar<'a> {
    year: i32,
    month: u32,
    /// Today's day of the month, or 0 when the month shown is not this one.
    today: u32,
    days: u32,
    /// The weekday the month starts on: 0 is Monday.
    first_weekday: u32,
    /// Add-ons are still being asked what is released when.
    loading: bool,
    /// Only the days that have something.
    items: Vec<Day<'a>>,
}

#[derive(Serialize)]
struct Day<'a> {
    day: u32,
    items: Vec<Entry<'a>>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Entry<'a> {
    /// The series.
    id: &'a str,
    r#type: &'a str,
    name: &'a str,
    poster: Option<&'a str>,
    /// The episode.
    video: &'a str,
    title: &'a str,
    season: u32,
    episode: u32,
}

/// Shows a month in the calendar: `year` 0 asks for the present month, and for what the
/// library's series are releasing to be looked up afresh.
pub fn load_calendar(core: &Core, year: i32, month: u32) {
    if year == 0 {
        core.dispatch(RuntimeAction {
            field: Some(Ps5ModelField::Calendar),
            action: Action::Unload,
        });
    }
    let selected = (year != 0).then_some(Selected { month, year });
    core.dispatch(RuntimeAction {
        field: Some(Ps5ModelField::Calendar),
        action: Action::Load(ActionLoad::Calendar(selected)),
    });
}

pub fn calendar(model: &Ps5Model) -> Option<Calendar<'_>> {
    let calendar = &model.calendar;
    let selected = calendar.selected.as_ref()?;
    Some(Calendar {
        year: selected.year,
        month: selected.month,
        today: calendar.month_info.today.unwrap_or(0),
        days: calendar.month_info.days,
        first_weekday: calendar.month_info.first_weekday,
        loading: calendar
            .meta_items
            .iter()
            .any(|request| matches!(request.content, Some(Loadable::Loading))),
        items: calendar
            .items
            .iter()
            .filter(|day| !day.items.is_empty())
            .map(|day| Day {
                day: day.date.day,
                items: day
                    .items
                    .iter()
                    .map(|entry| Entry {
                        id: &entry.meta_item.preview.id,
                        r#type: &entry.meta_item.preview.r#type,
                        name: &entry.meta_item.preview.name,
                        poster: entry.meta_item.preview.poster.as_ref().map(|url| url.as_str()),
                        video: &entry.video.id,
                        title: &entry.video.title,
                        season: entry.video.series_info.as_ref().map_or(0, |info| info.season),
                        episode: entry.video.series_info.as_ref().map_or(0, |info| info.episode),
                    })
                    .collect(),
            })
            .collect(),
    })
}

/// An installed add-on.
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Addon<'a> {
    name: &'a str,
    version: String,
    description: Option<&'a str>,
    logo: Option<&'a str>,
    /// The kinds of thing it has ("movie", "series").
    types: &'a [String],
    /// What it provides ("catalog", "stream", "subtitles").
    resources: Vec<&'a str>,
    official: bool,
    /// Where it is served from, without the scheme or the path.
    host: String,
}

pub fn addons(model: &Ps5Model) -> Vec<Addon<'_>> {
    use stremio_core::types::addon::ManifestResource;
    model
        .ctx
        .profile
        .addons
        .iter()
        .map(|addon| {
            let manifest = &addon.manifest;
            let mut resources: Vec<&str> = manifest
                .resources
                .iter()
                .map(|resource| match resource {
                    ManifestResource::Short(name) => name.as_str(),
                    ManifestResource::Full { name, .. } => name.as_str(),
                })
                .collect();
            if !manifest.catalogs.is_empty() && !resources.contains(&"catalog") {
                resources.insert(0, "catalog");
            }
            resources.dedup();
            Addon {
                name: &manifest.name,
                version: manifest.version.to_string(),
                description: manifest.description.as_deref(),
                logo: manifest.logo.as_ref().map(|url| url.as_str()),
                types: &manifest.types,
                resources,
                official: addon.flags.official,
                host: addon.transport_url.host_str().unwrap_or_default().to_owned(),
            }
        })
        .collect()
}
