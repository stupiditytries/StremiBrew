//! C interface to stremio-core for the PS5 app.
//!
//! The app starts the core once, sends it actions as JSON, drains the events it emits, and
//! reads model state as JSON. All functions may be called from any thread.

mod account;
mod details;
mod env;
mod model;

use std::collections::VecDeque;
use std::ffi::{c_char, CStr};
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};

use futures::executor::block_on;
use futures::{future, StreamExt};

use stremio_core::constants::{
    GENRES_LINK_CATEGORY, IMDB_LINK_CATEGORY, DISMISSED_EVENTS_STORAGE_KEY, LIBRARY_RECENT_STORAGE_KEY, LIBRARY_STORAGE_KEY,
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

/// Sends a line to the console's kernel log, where it can be read from a PC even after
/// the app has gone. Does nothing elsewhere.
pub(crate) fn kernel_log(text: &str) {
    #[cfg(target_os = "freebsd")]
    {
        extern "C" {
            fn sceKernelDebugOutText(channel: i32, text: *const c_char) -> i32;
        }
        if let Ok(line) = std::ffi::CString::new(format!("[stremio] {text}\n")) {
            unsafe { sceKernelDebugOutText(0, line.as_ptr()) };
        }
    }
    #[cfg(not(target_os = "freebsd"))]
    eprintln!("[stremio] {text}");
}

/// A panic ends the app (it is built to abort), so what it had to say is written to the
/// kernel log and to a file beside the storage folder first.
fn report_panics(storage_dir: &std::path::Path) {
    let file = storage_dir.with_file_name("panic.log");
    std::panic::set_hook(Box::new(move |info| {
        let text = format!("panic: {info}");
        kernel_log(&text);
        let _ = std::fs::write(&file, &text);
    }));
}

fn start(storage_dir: PathBuf) -> Result<(), EnvError> {
    report_panics(&storage_dir);
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
/// An event that does not fit the buffer is taken off the queue all the same (it is not
/// copied); its length, which is then `capacity` or more, tells the caller it was dropped.
#[no_mangle]
pub extern "C" fn stremio_core_poll_event(out: *mut c_char, capacity: usize) -> usize {
    let Ok(mut queue) = EVENTS.lock() else {
        return 0;
    };
    let Some(event) = queue.pop_front() else {
        return 0;
    };
    copy_out(&event, out, capacity)
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
    /// The row's position among all of the board's catalogs, shown or not. Rows are
    /// requested from the core by this number.
    index: usize,
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
    // What the featured area at the top of the board shows about the focused item.
    background: Option<&'a str>,
    logo: Option<&'a str>,
    description: Option<&'a str>,
    runtime: Option<&'a str>,
    imdb_rating: Option<&'a str>,
    genres: Vec<&'a str>,
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
        .enumerate()
        .filter_map(|(index, catalog)| Some((index, catalog.first()?)))
        .filter_map(|(index, page)| {
            // A catalog that answered with nothing, or failed, takes no row at all.
            match &page.content {
                Some(Loadable::Ready(items)) if items.is_empty() => return None,
                Some(Loadable::Err(_)) => return None,
                _ => {}
            }
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
                            background: item.background.as_ref().map(|url| url.as_str()),
                            logo: item.logo.as_ref().map(|url| url.as_str()),
                            description: item.description.as_deref(),
                            runtime: item.runtime.as_deref(),
                            imdb_rating: item
                                .links
                                .iter()
                                .find(|link| link.category == IMDB_LINK_CATEGORY)
                                .map(|link| link.name.as_str()),
                            genres: item
                                .links
                                .iter()
                                .filter(|link| link.category == GENRES_LINK_CATEGORY)
                                .map(|link| link.name.as_str())
                                .take(3)
                                .collect(),
                        })
                        .collect();
                    ("ready", None, items)
                }
                Some(Loadable::Err(error)) => ("error", Some(format!("{error:?}")), vec![]),
                Some(Loadable::Loading) | None => ("loading", None, vec![]),
            };
            Some(BoardRow {
                index,
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

/// Loads a title's details (see `details::load`). `video` may be null.
#[no_mangle]
pub extern "C" fn stremio_core_load_details(
    r#type: *const c_char,
    id: *const c_char,
    video: *const c_char,
) -> i32 {
    let Some(runtime) = RUNTIME.get() else {
        set_error("the core is not running");
        return -1;
    };
    let (Some(r#type), Some(id)) = (c_str(r#type), c_str(id)) else {
        set_error("type or id is not valid text");
        return -2;
    };
    details::load(runtime, r#type, id, c_str(video));
    0
}

/// Serialises the selected title's details for the UI as JSON (see `details::Details`).
/// Returns the JSON's length, or 0 when no title is selected or on failure.
#[no_mangle]
pub extern "C" fn stremio_core_details(out: *mut c_char, capacity: usize) -> usize {
    let Some(runtime) = RUNTIME.get() else {
        set_error("the core is not running");
        return 0;
    };
    let Ok(model) = runtime.model() else {
        set_error("model read failed");
        return 0;
    };
    let Some(details) = details::details(&model) else {
        return 0;
    };
    match serde_json::to_string(&details) {
        Ok(json) => copy_out(&json, out, capacity),
        Err(error) => {
            set_error(error.to_string());
            0
        }
    }
}

/// Starts signing in: asks Stremio for a link code, which `stremio_core_account` then
/// reports. Call `stremio_core_account_advance` every couple of seconds while it is shown.
#[no_mangle]
pub extern "C" fn stremio_core_sign_in_start() {
    if let Some(runtime) = RUNTIME.get() {
        account::start_link(runtime);
    }
}

/// Abandons a sign-in in progress.
#[no_mangle]
pub extern "C" fn stremio_core_sign_in_cancel() {
    if let Some(runtime) = RUNTIME.get() {
        account::cancel_link(runtime);
    }
}

/// Checks whether the link code has been entered and signs in once it has.
#[no_mangle]
pub extern "C" fn stremio_core_account_advance() {
    if let Some(runtime) = RUNTIME.get() {
        account::advance(runtime);
    }
}

#[no_mangle]
pub extern "C" fn stremio_core_sign_out() {
    if let Some(runtime) = RUNTIME.get() {
        account::sign_out(runtime);
    }
}

/// Serialises the account's state for the UI as JSON (see `account::Account`). Returns the
/// JSON's length, or 0 on failure.
#[no_mangle]
pub extern "C" fn stremio_core_account(out: *mut c_char, capacity: usize) -> usize {
    let Some(runtime) = RUNTIME.get() else {
        return 0;
    };
    let Ok(model) = runtime.model() else {
        return 0;
    };
    match serde_json::to_string(&account::account(&model)) {
        Ok(json) => copy_out(&json, out, capacity),
        Err(_) => 0,
    }
}

/// Requests the board's rows `start` up to (not including) `end`. Rows already loaded are
/// kept; the UI calls this as the focus moves down the board.
#[no_mangle]
pub extern "C" fn stremio_core_board_load_range(start: u32, end: u32) {
    if let Some(runtime) = RUNTIME.get() {
        runtime.dispatch(RuntimeAction {
            field: Some(Ps5ModelField::Board),
            action: Action::CatalogsWithExtra(ActionCatalogsWithExtra::LoadRange(
                start as usize..end as usize,
            )),
        });
    }
}

/// Whether a download started with `stremio_core_fetch_file` for `url` has failed.
#[no_mangle]
pub extern "C" fn stremio_core_fetch_failed(url: *const c_char) -> bool {
    c_str(url).is_some_and(env::fetch_failed)
}

/// Trims the folder of downloaded images to `limit` bytes, deleting the files that were
/// written longest ago. Returns how many files were deleted.
#[no_mangle]
pub extern "C" fn stremio_core_trim_folder(folder: *const c_char, limit: u64) -> usize {
    c_str(folder).map_or(0, |folder| env::trim_folder(std::path::Path::new(folder), limit))
}
