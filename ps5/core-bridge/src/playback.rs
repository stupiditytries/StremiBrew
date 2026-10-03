//! Telling the core what is being played and how far it has got. The core keeps the
//! library's watch progress from this (and syncs it with the account), which is where the
//! board's "Continue watching" row and the ticks on watched episodes come from.

use stremio_core::models::common::Loadable;
use stremio_core::models::player::Selected;
use stremio_core::runtime::msg::{Action, ActionLoad, ActionPlayer};
use stremio_core::runtime::{Runtime, RuntimeAction};
use stremio_core::types::addon::ResourcePath;

use crate::model::{Ps5Model, Ps5ModelField};
use crate::Ps5Env;

type Core = Runtime<Ps5Env, Ps5Model>;

const DEVICE: &str = "ps5";

fn dispatch(core: &Core, action: Action) {
    core.dispatch(RuntimeAction {
        field: Some(Ps5ModelField::Player),
        action,
    });
}

/// Starts a playback session for one of the open title's streams: the `index`th of the
/// streams the details list (see details.rs, which lists them in the same order).
pub fn load(core: &Core, index: usize) -> bool {
    let selected = {
        let Ok(model) = core.model() else {
            return false;
        };
        let details = &model.meta_details;
        let found = details
            .streams
            .iter()
            .filter(|group| {
                model
                    .ctx
                    .profile
                    .addons
                    .iter()
                    .any(|addon| addon.transport_url == group.request.base)
            })
            .filter_map(|group| match &group.content {
                Some(Loadable::Ready(streams)) => Some((group, streams)),
                _ => None,
            })
            .flat_map(|(group, streams)| streams.iter().map(move |stream| (group, stream)))
            .nth(index);
        let Some((group, stream)) = found else {
            return false;
        };
        let meta_request = details
            .meta_items
            .iter()
            .find(|item| matches!(item.content, Some(Loadable::Ready(_))))
            .or(details.meta_items.first())
            .map(|item| item.request.clone());
        Selected {
            stream: stream.clone(),
            stream_request: Some(group.request.clone()),
            meta_request,
            subtitles_path: Some(ResourcePath::without_extra(
                "subtitles",
                &group.request.path.r#type,
                &group.request.path.id,
            )),
        }
    };
    dispatch(core, Action::Load(ActionLoad::Player(Box::new(selected))));
    true
}

/// Where playback has got to, in milliseconds. `seek` says the user jumped there.
pub fn time(core: &Core, time: u64, duration: u64, seek: bool) {
    let device = DEVICE.to_owned();
    dispatch(
        core,
        Action::Player(if seek {
            ActionPlayer::Seek { time, duration, device }
        } else {
            ActionPlayer::TimeChanged { time, duration, device }
        }),
    );
}

pub fn paused(core: &Core, paused: bool) {
    dispatch(core, Action::Player(ActionPlayer::PausedChanged { paused }));
}

pub fn ended(core: &Core) {
    dispatch(core, Action::Player(ActionPlayer::Ended));
}

pub fn unload(core: &Core) {
    dispatch(core, Action::Unload);
}

/// Where a video was left, in milliseconds: its title's saved position when that position
/// is in this video, otherwise 0.
pub fn resume_offset(model: &Ps5Model, title: &str, video: &str) -> u64 {
    model
        .ctx
        .library
        .items
        .get(title)
        .filter(|item| item.state.video_id.as_deref() == Some(video))
        .map_or(0, |item| item.state.time_offset)
}
