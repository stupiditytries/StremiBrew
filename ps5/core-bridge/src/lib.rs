//! C interface to stremio-core for the PS5 app.
//!
//! The app starts the core once, sends it actions as JSON, drains the events it emits, and
//! reads model state as JSON. All functions may be called from any thread.

mod env;
mod model;

use std::collections::VecDeque;
use std::ffi::{c_char, CStr};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

use futures::executor::block_on;
use futures::{future, StreamExt};

use stremio_core::constants::{
    DISMISSED_EVENTS_STORAGE_KEY, LIBRARY_RECENT_STORAGE_KEY, LIBRARY_STORAGE_KEY,
    NOTIFICATIONS_STORAGE_KEY, PROFILE_STORAGE_KEY, SEARCH_HISTORY_STORAGE_KEY,
    STREAMING_SERVER_URLS_STORAGE_KEY, STREAMS_STORAGE_KEY,
};
use stremio_core::models::catalogs_with_extra::Selected;
use stremio_core::models::common::Loadable;
use stremio_core::runtime::msg::{Action, ActionCatalogsWithExtra, ActionLoad};
use stremio_core::runtime::{Env, EnvError, Runtime, RuntimeAction};
use stremio_core::types::events::DismissedEventsBucket;
use stremio_core::types::library::LibraryBucket;
use stremio_core::types::notifications::NotificationsBucket;
use stremio_core::types::profile::Profile;
use stremio_core::types::search_history::SearchHistoryBucket;
use stremio_core::types::server_urls::ServerUrlsBucket;
use stremio_core::types::streams::StreamsBucket;

pub use env::Ps5Env;
pub use model::{Ps5Model, Ps5ModelField};

static RUNTIME: OnceLock<Runtime<Ps5Env, Ps5Model>> = OnceLock::new();
/// Events the core has emitted and the app has not collected yet, as JSON.
static EVENTS: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static LAST_ERROR: Mutex<String> = Mutex::new(String::new());

fn set_error(text: impl Into<String>) {
    if let Ok(mut error) = LAST_ERROR.lock() {
        *error = text.into();
    }
}

fn c_str<'a>(value: *const c_char) -> Option<&'a str> {
    if value.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(value) }.to_str().ok()
}

/// Copies `text` into `out` (NUL-terminated) when it fits. Returns the length of `text`,
/// so a result of `capacity` or more means the buffer was too small and nothing was copied.
fn copy_out(text: &str, out: *mut c_char, capacity: usize) -> usize {
    if !out.is_null() && text.len() < capacity {
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), out as *mut u8, text.len());
            *out.add(text.len()) = 0;
        }
    }
    text.len()
}

fn start(storage_dir: PathBuf) -> Result<(), EnvError> {
    Ps5Env::init(storage_dir)?;
    block_on(Ps5Env::migrate_storage_schema())?;
    let (profile, recent, other, streams, server_urls, notifications, search_history, dismissed) =
        block_on(async {
            futures::try_join!(
                Ps5Env::get_storage::<Profile>(PROFILE_STORAGE_KEY),
                Ps5Env::get_storage::<LibraryBucket>(LIBRARY_RECENT_STORAGE_KEY),
                Ps5Env::get_storage::<LibraryBucket>(LIBRARY_STORAGE_KEY),
                Ps5Env::get_storage::<StreamsBucket>(STREAMS_STORAGE_KEY),
                Ps5Env::get_storage::<ServerUrlsBucket>(STREAMING_SERVER_URLS_STORAGE_KEY),
                Ps5Env::get_storage::<NotificationsBucket>(NOTIFICATIONS_STORAGE_KEY),
                Ps5Env::get_storage::<SearchHistoryBucket>(SEARCH_HISTORY_STORAGE_KEY),
                Ps5Env::get_storage::<DismissedEventsBucket>(DISMISSED_EVENTS_STORAGE_KEY),
            )
        })?;
    let profile = profile.unwrap_or_default();
    let mut library = LibraryBucket::new(profile.uid(), vec![]);
    if let Some(recent) = recent {
        library.merge_bucket(recent);
    }
    if let Some(other) = other {
        library.merge_bucket(other);
    }
    let streams = streams.unwrap_or_else(|| StreamsBucket::new(profile.uid()));
    let server_urls =
        server_urls.unwrap_or_else(|| ServerUrlsBucket::new::<Ps5Env>(profile.uid()));
    let notifications = notifications
        .unwrap_or_else(|| NotificationsBucket::new::<Ps5Env>(profile.uid(), vec![]));
    let search_history =
        search_history.unwrap_or_else(|| SearchHistoryBucket::new(profile.uid()));
    let dismissed = dismissed.unwrap_or_else(|| DismissedEventsBucket::new(profile.uid()));

    let (model, effects) = Ps5Model::new(
        profile,
        library,
        streams,
        server_urls,
        notifications,
        search_history,
        dismissed,
    );
    let (runtime, events) = Runtime::<Ps5Env, _>::new(model, effects.into_iter().collect(), 1000);
    Ps5Env::exec_concurrent(events.for_each(|event| {
        if let (Ok(json), Ok(mut queue)) = (serde_json::to_string(&event), EVENTS.lock()) {
            queue.push_back(json);
        }
        future::ready(())
    }));
    RUNTIME
        .set(runtime)
        .map_err(|_| EnvError::Other("the core is already running".to_owned()))
}

/// Starts the core with its storage in `storage_dir`. Returns 0 on success; on failure the
/// reason is available from `stremio_core_last_error`.
#[no_mangle]
pub extern "C" fn stremio_core_init(storage_dir: *const c_char) -> i32 {
    let Some(dir) = c_str(storage_dir) else {
        set_error("storage folder is not valid text");
        return -1;
    };
    match start(PathBuf::from(dir)) {
        Ok(()) => 0,
        Err(error) => {
            set_error(error.message());
            -1
        }
    }
}

/// Sends an action to the core. `action_json` is an action in the core's own JSON form;
/// `field_json` names one model field (for example `"board"`) or is null to offer the
/// action to every model. Returns 0 on success.
#[no_mangle]
pub extern "C" fn stremio_core_dispatch(
    action_json: *const c_char,
    field_json: *const c_char,
) -> i32 {
    let Some(runtime) = RUNTIME.get() else {
        set_error("the core is not running");
        return -1;
    };
    let action: Action = match c_str(action_json).map(serde_json::from_str) {
        Some(Ok(action)) => action,
        Some(Err(error)) => {
            set_error(format!("action: {error}"));
            return -2;
        }
        None => {
            set_error("action is not valid text");
            return -2;
        }
    };
    let field: Option<Ps5ModelField> = match c_str(field_json).map(serde_json::from_str) {
        Some(Ok(field)) => Some(field),
        Some(Err(error)) => {
            set_error(format!("field: {error}"));
            return -3;
        }
        None => None,
    };
    runtime.dispatch(RuntimeAction { field, action });
    0
}

/// Loads the board (the home screen's catalog rows) and requests its first `rows` rows.
#[no_mangle]
pub extern "C" fn stremio_core_load_board(rows: u32) -> i32 {
    let Some(runtime) = RUNTIME.get() else {
        set_error("the core is not running");
        return -1;
    };
    runtime.dispatch(RuntimeAction {
        field: Some(Ps5ModelField::Board),
        action: Action::Load(ActionLoad::CatalogsWithExtra(Selected {
            r#type: None,
            extra: vec![],
        })),
    });
    runtime.dispatch(RuntimeAction {
        field: Some(Ps5ModelField::Board),
        action: Action::CatalogsWithExtra(ActionCatalogsWithExtra::LoadRange(
            0..rows as usize,
        )),
    });
    0
}

/// Takes the oldest uncollected event as JSON. Returns its length, or 0 when there is none.
/// If the buffer is too small the event stays queued and its length is returned.
#[no_mangle]
pub extern "C" fn stremio_core_poll_event(out: *mut c_char, capacity: usize) -> usize {
    let Ok(mut queue) = EVENTS.lock() else {
        return 0;
    };
    let Some(event) = queue.front() else {
        return 0;
    };
    let length = copy_out(event, out, capacity);
    if length < capacity {
        queue.pop_front();
    }
    length
}

/// Serialises one model field (`"ctx"` or `"board"`, without quotes) as JSON. Returns the
/// JSON's length (see `copy_out` for the too-small case), or 0 on failure.
#[no_mangle]
pub extern "C" fn stremio_core_get_state(
    field: *const c_char,
    out: *mut c_char,
    capacity: usize,
) -> usize {
    let (Some(runtime), Some(field)) = (RUNTIME.get(), c_str(field)) else {
        set_error("the core is not running or the field is not valid text");
        return 0;
    };
    let Ok(model) = runtime.model() else {
        set_error("model read failed");
        return 0;
    };
    let json = match field {
        "ctx" => serde_json::to_string(&model.ctx),
        "board" => serde_json::to_string(&model.board),
        other => {
            set_error(format!("unknown field {other}"));
            return 0;
        }
    };
    match json {
        Ok(json) => copy_out(&json, out, capacity),
        Err(error) => {
            set_error(error.to_string());
            0
        }
    }
}

/// A short plain-text description of the board: one line of totals, then one line per row
/// with its add-on catalog, state and first title. Returns the text's length.
#[no_mangle]
pub extern "C" fn stremio_core_board_summary(out: *mut c_char, capacity: usize) -> usize {
    let Some(runtime) = RUNTIME.get() else {
        return 0;
    };
    let Ok(model) = runtime.model() else {
        return 0;
    };
    let (mut ready, mut loading, mut failed, mut idle) = (0, 0, 0, 0);
    let mut rows = String::new();
    for catalog in &model.board.catalogs {
        let Some(page) = catalog.first() else {
            continue;
        };
        let name = format!("{}/{}", page.request.path.r#type, page.request.path.id);
        let state = match &page.content {
            Some(Loadable::Ready(items)) => {
                ready += 1;
                let first = items.first().map(|item| item.name.as_str()).unwrap_or("");
                format!("{} items, first: {first}", items.len())
            }
            Some(Loadable::Loading) => {
                loading += 1;
                "loading".to_owned()
            }
            Some(Loadable::Err(error)) => {
                failed += 1;
                format!("error: {error:?}")
            }
            None => {
                idle += 1;
                continue;
            }
        };
        rows.push_str(&format!("{name}: {state}\n"));
    }
    let text = format!(
        "rows {} ready {ready} loading {loading} failed {failed} not requested {idle}\n{rows}",
        model.board.catalogs.len()
    );
    copy_out(&text, out, capacity)
}

/// Copies the text of the most recent failure into `out`. Returns its length.
#[no_mangle]
pub extern "C" fn stremio_core_last_error(out: *mut c_char, capacity: usize) -> usize {
    let text = LAST_ERROR.lock().map(|error| error.clone()).unwrap_or_default();
    copy_out(&text, out, capacity)
}

/// The board as the UI draws it: one entry per row, with the row's title parts from the
/// add-on's manifest and the fields of each item a poster card needs.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct BoardRow<'a> {
    id: &'a str,
    name: &'a str,
    r#type: &'a str,
    addon: &'a str,
    /// "ready", "loading" or "error".
    state: &'static str,
    error: Option<String>,
    items: Vec<BoardItem<'a>>,
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct BoardItem<'a> {
    id: &'a str,
    r#type: &'a str,
    name: &'a str,
    poster: Option<std::borrow::Cow<'a, str>>,
    poster_shape: &'a stremio_core::types::resource::PosterShape,
    release_info: Option<&'a str>,
}

/// Stremio's poster service offers each poster in three sizes and catalogs link the
/// smallest, which is soft on a 4K screen; the medium one is asked for instead.
fn sharper_poster(url: &str) -> std::borrow::Cow<'_, str> {
    const SMALL: &str = "images.metahub.space/poster/small/";
    if url.contains(SMALL) {
        url.replace(SMALL, "images.metahub.space/poster/medium/").into()
    } else {
        url.into()
    }
}

/// Serialises the board's rows for the UI as JSON (see `BoardRow`), with at most
/// `items_per_row` items in each. Returns the JSON's length, or 0 on failure.
#[no_mangle]
pub extern "C" fn stremio_core_board_rows(
    items_per_row: u32,
    out: *mut c_char,
    capacity: usize,
) -> usize {
    let Some(runtime) = RUNTIME.get() else {
        set_error("the core is not running");
        return 0;
    };
    let Ok(model) = runtime.model() else {
        set_error("model read failed");
        return 0;
    };
    let rows: Vec<BoardRow> = model
        .board
        .catalogs
        .iter()
        .filter_map(|catalog| catalog.first())
        .filter_map(|page| {
            // A row is titled by the catalog's entry in its add-on's manifest.
            let addon = model
                .ctx
                .profile
                .addons
                .iter()
                .find(|addon| addon.transport_url == page.request.base)?;
            let catalog = addon.manifest.catalogs.iter().find(|catalog| {
                catalog.id == page.request.path.id && catalog.r#type == page.request.path.r#type
            })?;
            let (state, error, items) = match &page.content {
                Some(Loadable::Ready(items)) => {
                    // A row is drawn in one shape: its first item's.
                    let shape = items.first().map(|item| &item.poster_shape);
                    let mut seen = std::collections::HashSet::new();
                    let items = items
                        .iter()
                        .filter(|item| seen.insert(item.id.as_str()))
                        .take(items_per_row as usize)
                        .map(|item| BoardItem {
                            id: &item.id,
                            r#type: &item.r#type,
                            name: &item.name,
                            poster: item.poster.as_ref().map(|url| sharper_poster(url.as_str())),
                            poster_shape: shape.unwrap_or(&item.poster_shape),
                            release_info: item.release_info.as_deref(),
                        })
                        .collect();
                    ("ready", None, items)
                }
                Some(Loadable::Err(error)) => ("error", Some(format!("{error:?}")), vec![]),
                Some(Loadable::Loading) | None => ("loading", None, vec![]),
            };
            Some(BoardRow {
                id: &catalog.id,
                name: catalog.name.as_deref().unwrap_or(&addon.manifest.name),
                r#type: &catalog.r#type,
                addon: &addon.manifest.name,
                state,
                error,
                items,
            })
        })
        .collect();
    match serde_json::to_string(&rows) {
        Ok(json) => copy_out(&json, out, capacity),
        Err(error) => {
            set_error(error.to_string());
            0
        }
    }
}

/// Starts downloading `url` into the file `path` (for example a poster into the image
/// cache) and returns at once; the file appears when the download has finished. Returns 0
/// when the download was started.
#[no_mangle]
pub extern "C" fn stremio_core_fetch_file(url: *const c_char, path: *const c_char) -> i32 {
    let (Some(url), Some(path)) = (c_str(url), c_str(path)) else {
        set_error("address or path is not valid text");
        return -1;
    };
    if !(url.starts_with("https://") || url.starts_with("http://")) {
        set_error("only web addresses can be downloaded");
        return -2;
    }
    env::fetch_to_file(url.to_owned(), PathBuf::from(path));
    0
}
