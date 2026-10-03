//! The Stremio account: signing in with a link code, signing out, and what the UI shows
//! about the account.
//!
//! Signing in never involves typing a password on the console. The app asks Stremio for a
//! short code, the user enters it at Stremio's link page on a phone or computer where they
//! are signed in, and the app then receives a key for the account.

use serde::Serialize;

use stremio_core::models::common::Loadable;
use stremio_core::models::ctx::CtxStatus;
use stremio_core::runtime::msg::{Action, ActionCtx, ActionLink, ActionLoad};
use stremio_core::runtime::{Runtime, RuntimeAction};
use stremio_core::types::api::AuthRequest;

use crate::model::{Ps5Model, Ps5ModelField};
use crate::Ps5Env;

type Core = Runtime<Ps5Env, Ps5Model>;

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Account {
    signed_in: bool,
    email: Option<String>,
    /// How many add-ons are installed (the account's, once signed in).
    addons: usize,
    /// "idle", "requesting" (asking for a code), "waiting" (code shown, not yet entered),
    /// "signing_in" (code entered, account loading) or "error".
    link: &'static str,
    code: Option<String>,
    /// The page where the code is entered.
    link_page: Option<String>,
    error: Option<String>,
}

fn dispatch(core: &Core, field: Option<Ps5ModelField>, action: Action) {
    core.dispatch(RuntimeAction { field, action });
}

/// Asks Stremio for a new link code.
pub fn start_link(core: &Core) {
    dispatch(core, Some(Ps5ModelField::AuthLink), Action::Load(ActionLoad::Link));
}

/// Abandons a sign-in in progress.
pub fn cancel_link(core: &Core) {
    dispatch(core, Some(Ps5ModelField::AuthLink), Action::Unload);
}

pub fn sign_out(core: &Core) {
    dispatch(core, Some(Ps5ModelField::Ctx), Action::Ctx(ActionCtx::Logout));
}

/// What `advance` should do next, decided while the model is locked and done after it is
/// released (dispatching needs the lock itself).
enum Step {
    Nothing,
    /// Ask whether the code has been entered yet.
    Check,
    /// The code was entered: sign in with the key that came back.
    SignIn(String),
}

/// Moves a sign-in along. The UI calls this every couple of seconds while a code is on
/// screen: it asks Stremio whether the code has been entered and, once it has, signs in.
pub fn advance(core: &Core) {
    let step = {
        let Ok(model) = core.model() else {
            return;
        };
        if model.ctx.profile.auth.is_some() || matches!(model.ctx.status, CtxStatus::Loading(_)) {
            Step::Nothing
        } else {
            match (&model.auth_link.code, &model.auth_link.data) {
                (Some(Loadable::Ready(_)), Some(Loadable::Ready(key))) => {
                    Step::SignIn(key.auth_key.clone())
                }
                // Not entered yet shows up as an error from the read; just ask again.
                (Some(Loadable::Ready(_)), Some(Loadable::Err(_)) | None) => Step::Check,
                _ => Step::Nothing,
            }
        }
    };
    match step {
        Step::Nothing => {}
        Step::Check => dispatch(
            core,
            Some(Ps5ModelField::AuthLink),
            Action::Link(ActionLink::ReadData),
        ),
        Step::SignIn(token) => {
            dispatch(
                core,
                Some(Ps5ModelField::Ctx),
                Action::Ctx(ActionCtx::Authenticate(AuthRequest::LoginWithToken { token })),
            );
            cancel_link(core);
        }
    }
}

pub fn account(model: &Ps5Model) -> Account {
    let auth = model.ctx.profile.auth.as_ref();
    let signing_in = matches!(model.ctx.status, CtxStatus::Loading(_));
    let (link, code, link_page, error) = match &model.auth_link.code {
        _ if auth.is_some() => ("idle", None, None, None),
        _ if signing_in => ("signing_in", None, None, None),
        None => ("idle", None, None, None),
        Some(Loadable::Loading) => ("requesting", None, None, None),
        Some(Loadable::Ready(response)) => (
            "waiting",
            Some(response.code.clone()),
            Some(response.link.clone()),
            None,
        ),
        Some(Loadable::Err(error)) => ("error", None, None, Some(error.to_string())),
    };
    Account {
        signed_in: auth.is_some(),
        email: auth.map(|auth| auth.user.email.clone()),
        addons: model.ctx.profile.addons.len(),
        link,
        code,
        link_page,
        error,
    }
}
