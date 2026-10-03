#include "app/Application.hpp"

#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/api/MqttReactionListener.hpp"
#include "caelitus/catalog/mariadb/CatalogMigrations.hpp"
#include "caelitus/catalog/mariadb/MariaDbReactionRepository.hpp"
#include "caelitus/catalog/mariadb/MariaDbRepositories.hpp"
#include "caelitus/core/PeriodicTask.hpp"
#include "caelitus/core/TimeZone.hpp"
#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/Migrations.hpp"
#include "caelitus/db/SqlExecutor.hpp"
#include "caelitus/db/TransactionManager.hpp"
#include "caelitus/db/mariadb/MariaDbConnection.hpp"
#include "caelitus/mqtt/mosquitto/MosquittoClient.hpp"
#include "caelitus/net/TcpServer.hpp"

namespace caelitus::app {

api::CatalogServices makeCatalogServices(const AppConfig::Catalog& config, const std::shared_ptr<db::SqlExecutor>& sql,
                                         const std::shared_ptr<db::ITransactionManager>& tx,
                                         const std::shared_ptr<mqtt::IMqttPublisher>& events,
                                         const std::shared_ptr<catalog::BookCache>& bookCache) {
    using namespace catalog::mariadb;
    auto categories = std::make_shared<MariaDbCategoryRepository>(sql);
    auto authors = std::make_shared<MariaDbAuthorRepository>(sql);
    auto tags = std::make_shared<MariaDbTagRepository>(sql);
    auto books = std::make_shared<MariaDbBookRepository>(sql);
    auto reviews = std::make_shared<MariaDbReviewRepository>(sql);
    auto reactions = std::make_shared<MariaDbReactionRepository>(sql);
    return {
        std::make_shared<catalog::CategoryService>(categories),
        std::make_shared<catalog::AuthorService>(authors, tx),
        std::make_shared<catalog::BookService>(books, authors, categories, tags, tx, events, catalog::systemClock(),
                                               bookCache),
        std::make_shared<catalog::ReviewService>(reviews, books, tx, events),
        std::make_shared<catalog::ReactionService>(reactions, books, bookCache, tx, TimeZone::named(config.timeZone),
                                                   config.maxBufferedReactions),
    };
}

// Declared in start order; destroyed (and stopped in stop()) in reverse.
struct Application::Components {
    std::shared_ptr<mqtt::MosquittoClient> mqtt;
    std::shared_ptr<db::ConnectionPool> pool;
    std::shared_ptr<db::SqlExecutor> sql;
    std::shared_ptr<db::TransactionManager> tx;
    std::shared_ptr<catalog::BookCache> bookCache;
    std::unique_ptr<PeriodicTask> cacheReloader;
    api::CatalogServices services;
    std::unique_ptr<api::MqttReactionListener> listener;
    std::unique_ptr<PeriodicTask> flusher;
    std::shared_ptr<api::JsonRpcHandler> rpc;
    std::unique_ptr<net::TcpServer> server;
};

Application::Application(AppConfig config)
    : config_(std::move(config)),
      log_(log::get("app")),
      c_(std::make_unique<Components>()) {}

Application::~Application() { stop(); }

void Application::start() {
    Components& c = *c_;

    // MQTT first and non-blocking: a missing broker must not stop the server.
    c.mqtt = std::make_shared<mqtt::MosquittoClient>(config_.mqtt);
    c.mqtt->start();
    if (!c.mqtt->waitUntilConnected(std::chrono::seconds(2)))
        log_->warn("MQTT broker not reachable yet; continuing, messages are dropped until it is");

    // The database is required: fail fast on bad credentials or an unreachable server.
    c.pool = db::ConnectionPool::create(std::make_shared<db::mariadb::MariaDbConnectionFactory>(config_.db.connection),
                                        config_.db.pool);
    c.pool->warmUp(1);
    db::MigrationRunner(c.pool).migrate(catalog::mariadb::catalogMigrations());
    c.sql = std::make_shared<db::SqlExecutor>(c.pool, config_.db.executor);
    c.tx = std::make_shared<db::TransactionManager>(c.pool, config_.db.transaction);

    // Book cache: loaded now, refreshed by BookService on every change, and
    // fully reloaded periodically to pick up edits made outside the server.
    c.bookCache =
        std::make_shared<catalog::BookCache>(std::make_shared<catalog::mariadb::MariaDbBookRepository>(c.sql));
    c.bookCache->reload();
    log_->info("Book cache loaded: {} books", c.bookCache->stats().size);
    c.cacheReloader = std::make_unique<PeriodicTask>("book cache reload", config_.catalog.bookCacheReload,
                                                     [cache = c.bookCache] { cache->reload(); });
    c.cacheReloader->start();

    c.services = makeCatalogServices(config_.catalog, c.sql, c.tx, c.mqtt, c.bookCache);

    // Inbound: likes/dislikes over MQTT, counted in memory and written periodically.
    c.listener =
        std::make_unique<api::MqttReactionListener>(c.mqtt, c.services.reactions, config_.catalog.reactionTopicPrefix);
    c.flusher = std::make_unique<PeriodicTask>("reaction flush", config_.catalog.reactionFlushInterval,
                                               [reactions = c.services.reactions] { reactions->flush(); });
    c.flusher->start();

    // Inbound: JSON-RPC requests over TCP.
    c.rpc = std::make_shared<api::JsonRpcHandler>();
    api::registerCatalogApi(*c.rpc, c.services);
    c.server = std::make_unique<net::TcpServer>(config_.server, c.rpc);
    c.server->start();
    log_->info("Ready on port {}: {} API methods", c.server->port(), c.rpc->methods().size());
}

void Application::stop() {
    Components& c = *c_;
    if (c.server) c.server->stop();
    c.listener.reset();  // no new likes from here on...
    if (c.flusher) c.flusher->stop();
    if (c.cacheReloader) c.cacheReloader->stop();
    if (c.services.reactions) {  // ...so this flush stores every like received
        const auto last = c.services.reactions->flush();
        if (last.written > 0) log_->info("Stored {} buffered reactions", last.written);
        c.services.reactions.reset();
    }
    if (c.mqtt) c.mqtt->stop();
    c_ = std::make_unique<Components>();  // releases everything; stop() is now a no-op
}

std::uint16_t Application::port() const { return c_->server ? c_->server->port() : 0; }

}  // namespace caelitus::app
