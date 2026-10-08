//! The PS5 implementation of the core's environment: HTTP through blocking sockets on
//! worker threads, storage as one JSON file per key, and thread pools as executors.

use std::path::PathBuf;
use std::sync::OnceLock;
use std::time::Duration;

use chrono::{DateTime, Utc};
use futures::channel::oneshot;
use futures::executor::{ThreadPool, ThreadPoolBuilder};
use futures::{future, Future, FutureExt};
use http::{Method, Request};
use once_cell::sync::Lazy;
use serde::{Deserialize, Serialize};

use stremio_core::models::ctx::Ctx;
use stremio_core::models::streaming_server::StreamingServer;
use stremio_core::runtime::{Env, EnvError, EnvFuture, EnvFutureExt, TryEnvFuture};

/// Network requests block a thread each, so they get a pool of their own.
static IO_POOL: Lazy<ThreadPool> = Lazy::new(|| pool("core-io", 8));
static CONCURRENT_POOL: Lazy<ThreadPool> = Lazy::new(|| pool("core-work", 4));
/// One thread: tasks given to `exec_sequential` run in the order they were queued.
static SEQUENTIAL_POOL: Lazy<ThreadPool> = Lazy::new(|| pool("core-seq", 1));

static STORAGE_DIR: OnceLock<PathBuf> = OnceLock::new();

static HTTP: Lazy<ureq::Agent> = Lazy::new(|| {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(15))
        .timeout(Duration::from_secs(60))
        .user_agent("stremio-ps5")
        .build()
});

fn pool(name: &str, size: usize) -> ThreadPool {
    ThreadPoolBuilder::new()
        .pool_size(size)
        .name_prefix(name)
        .create()
        .expect("thread pool creation failed")
}

/// Runs `job` on the network pool and resolves with its result.
fn blocking<T: Send + 'static>(
    job: impl FnOnce() -> T + Send + 'static,
) -> impl Future<Output = Result<T, EnvError>> + Send {
    let (sender, receiver) = oneshot::channel();
    IO_POOL.spawn_ok(async move {
        let _ = sender.send(job());
    });
    receiver.map(|result| result.map_err(|_| EnvError::Other("worker stopped".to_owned())))
}

/// Moves a finished file into place, over whatever is there.
pub(crate) fn put_in_place(from: &std::path::Path, to: &std::path::Path) -> std::io::Result<()> {
    // The Switch's file system will not rename onto a file that exists.
    #[cfg(target_os = "horizon")]
    let _ = std::fs::remove_file(to);
    std::fs::rename(from, to)
}

/// Downloads `url` into the file `path` on the network pool. The bytes are written
/// beside the target and renamed into place, so a reader never sees half a file. A failed
/// download leaves no file. Used for poster images.
pub(crate) fn fetch_to_file(url: String, path: PathBuf) {
    /// Posters are tens of kilobytes; anything far larger is not an image worth keeping.
    const LIMIT: u64 = 16 << 20;
    IO_POOL.spawn_ok(async move {
        let saved = (|| {
            let response = HTTP.get(&url).call().ok()?;
            let mut bytes = Vec::new();
            let mut limited = std::io::Read::take(response.into_reader(), LIMIT);
            std::io::Read::read_to_end(&mut limited, &mut bytes).ok()?;
            if bytes.is_empty() {
                return None;
            }
            let temporary = path.with_extension("part");
            std::fs::write(&temporary, &bytes).ok()?;
            put_in_place(&temporary, &path).ok()
        })();
        if saved.is_none() {
            if let Ok(mut failed) = FAILED_DOWNLOADS.lock() {
                failed.insert(url);
            }
        }
    });
}

/// Addresses whose download failed in this run. Kept in memory only, so a failure caused
/// by a passing network problem is tried again the next time the app starts.
static FAILED_DOWNLOADS: Lazy<std::sync::Mutex<std::collections::HashSet<String>>> =
    Lazy::new(Default::default);

pub(crate) fn fetch_failed(url: &str) -> bool {
    FAILED_DOWNLOADS
        .lock()
        .map(|failed| failed.contains(url))
        .unwrap_or(false)
}

/// Deletes the files in `folder` that were used longest ago until what is left is no
/// larger than `limit` bytes. Returns how many files were deleted.
pub(crate) fn trim_folder(folder: &std::path::Path, limit: u64) -> usize {
    let Ok(entries) = std::fs::read_dir(folder) else {
        return 0;
    };
    let mut files: Vec<(std::time::SystemTime, u64, PathBuf)> = entries
        .filter_map(|entry| {
            // By path: the console cannot report a file's status through its folder's
            // handle, which is what asking the entry itself would do.
            let path = entry.ok()?.path();
            let status = std::fs::metadata(&path).ok()?;
            status
                .is_file()
                .then(|| (status.modified().unwrap_or(std::time::UNIX_EPOCH), status.len(), path))
        })
        .collect();
    let mut total: u64 = files.iter().map(|(_, size, _)| size).sum();
    // Oldest first.
    files.sort();
    let mut deleted = 0;
    for (_, size, path) in files {
        if total <= limit {
            break;
        }
        if std::fs::remove_file(&path).is_ok() {
            total -= size;
            deleted += 1;
        }
    }
    deleted
}

fn storage_path(key: &str) -> Result<PathBuf, EnvError> {
    let dir = STORAGE_DIR.get().ok_or(EnvError::StorageUnavailable)?;
    // Keys are fixed names from the core; anything else is kept out of the file name.
    let name: String = key
        .chars()
        .map(|c| if c.is_ascii_alphanumeric() || c == '_' || c == '-' { c } else { '_' })
        .collect();
    Ok(dir.join(format!("{name}.json")))
}

pub enum Ps5Env {}

impl Ps5Env {
    /// Sets the folder storage lives in and creates it. Must be called once before the
    /// runtime starts.
    pub fn init(storage_dir: PathBuf) -> Result<(), EnvError> {
        std::fs::create_dir_all(&storage_dir)
            .map_err(|error| EnvError::StorageWriteError(error.to_string()))?;
        STORAGE_DIR
            .set(storage_dir)
            .map_err(|_| EnvError::Other("storage folder already set".to_owned()))
    }
}

impl Env for Ps5Env {
    fn fetch<IN, OUT>(request: Request<IN>) -> TryEnvFuture<OUT>
    where
        IN: Serialize + Send + 'static,
        OUT: for<'de> Deserialize<'de> + Send + 'static,
    {
        let (parts, body) = request.into_parts();
        let body = match serde_json::to_string(&body) {
            Ok(body) if body != "null" && parts.method != Method::GET => Some(body),
            Ok(_) => None,
            Err(error) => return future::err(EnvError::Serde(error.to_string())).boxed_env(),
        };
        blocking(move || {
            let mut call = HTTP.request(parts.method.as_str(), &parts.uri.to_string());
            for (name, value) in parts.headers.iter() {
                if let Ok(value) = value.to_str() {
                    call = call.set(name.as_str(), value);
                }
            }
            let response = match body {
                Some(body) => call.set("content-type", "application/json").send_string(&body),
                None => call.call(),
            };
            let response = match response {
                Ok(response) => response,
                Err(ureq::Error::Status(code, _)) => {
                    return Err(EnvError::Fetch(format!("Unexpected HTTP status code {code}")))
                }
                Err(error) => {
                    #[cfg(target_os = "horizon")]
                    crate::horizon::note_failure(&parts.uri.to_string(), &error.to_string());
                    return Err(EnvError::Fetch(error.to_string()));
                }
            };
            let mut deserializer = serde_json::Deserializer::from_reader(response.into_reader());
            OUT::deserialize(&mut deserializer).map_err(|error| EnvError::Fetch(error.to_string()))
        })
        .map(|result| result.and_then(|inner| inner))
        .boxed_env()
    }

    fn get_storage<T>(key: &str) -> TryEnvFuture<Option<T>>
    where
        T: for<'de> Deserialize<'de> + Send + 'static,
    {
        let result = storage_path(key).and_then(|path| match std::fs::read(&path) {
            Ok(bytes) => serde_json::from_slice(&bytes)
                .map(Some)
                .map_err(|error| EnvError::StorageReadError(error.to_string())),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(error) => Err(EnvError::StorageReadError(error.to_string())),
        });
        future::ready(result).boxed_env()
    }

    fn set_storage<T: Serialize>(key: &str, value: Option<&T>) -> TryEnvFuture<()> {
        let result = storage_path(key).and_then(|path| match value {
            Some(value) => {
                let bytes = serde_json::to_vec(value)?;
                // Written beside the target and renamed, so a crash cannot leave half a file.
                let temporary = path.with_extension("tmp");
                std::fs::write(&temporary, bytes)
                    .and_then(|_| put_in_place(&temporary, &path))
                    .map_err(|error| EnvError::StorageWriteError(error.to_string()))
            }
            None => match std::fs::remove_file(&path) {
                Ok(()) => Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => Err(EnvError::StorageWriteError(error.to_string())),
            },
        });
        future::ready(result).boxed_env()
    }

    fn exec_concurrent<F: Future<Output = ()> + Send + 'static>(future: F) {
        CONCURRENT_POOL.spawn_ok(future);
    }

    fn exec_sequential<F: Future<Output = ()> + Send + 'static>(future: F) {
        SEQUENTIAL_POOL.spawn_ok(future);
    }

    fn now() -> DateTime<Utc> {
        Utc::now()
    }

    fn flush_analytics() -> EnvFuture<'static, ()> {
        future::ready(()).boxed_env()
    }

    fn analytics_context(
        _ctx: &Ctx,
        _streaming_server: &StreamingServer,
        _path: &str,
    ) -> serde_json::Value {
        serde_json::Value::Null
    }

    #[cfg(debug_assertions)]
    fn log(message: String) {
        eprintln!("{message}");
    }
}
