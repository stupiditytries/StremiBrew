//! The app's model: the parts of the core the PS5 app uses. It starts with the user
//! context, the board and a title's details, and grows a field per screen as screens are
//! built.

use stremio_core::models::catalogs_with_extra::CatalogsWithExtra;
use stremio_core::models::ctx::Ctx;
use stremio_core::models::meta_details::MetaDetails;
use stremio_core::runtime::Effects;
use stremio_core::types::events::DismissedEventsBucket;
use stremio_core::types::library::LibraryBucket;
use stremio_core::types::notifications::NotificationsBucket;
use stremio_core::types::profile::Profile;
use stremio_core::types::search_history::SearchHistoryBucket;
use stremio_core::types::server_urls::ServerUrlsBucket;
use stremio_core::types::streams::StreamsBucket;
use stremio_core::Model;

use crate::env::Ps5Env;

#[derive(Model, Clone)]
#[model(Ps5Env)]
pub struct Ps5Model {
    pub ctx: Ctx,
    pub board: CatalogsWithExtra,
    pub meta_details: MetaDetails,
}

impl Ps5Model {
    pub fn new(
        profile: Profile,
        library: LibraryBucket,
        streams: StreamsBucket,
        server_urls: ServerUrlsBucket,
        notifications: NotificationsBucket,
        search_history: SearchHistoryBucket,
        dismissed_events: DismissedEventsBucket,
    ) -> (Ps5Model, Effects) {
        let model = Ps5Model {
            ctx: Ctx::new(
                profile,
                library,
                streams,
                server_urls,
                notifications,
                search_history,
                dismissed_events,
            ),
            board: CatalogsWithExtra::default(),
            meta_details: MetaDetails::default(),
        };
        (model, Effects::none().unchanged())
    }
}
