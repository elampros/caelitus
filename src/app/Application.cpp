/// @file
/// app::Application: creates every component in dependency order, defines the
/// scheduled jobs, starts them and stops them in reverse order.
/// @ingroup app

#include "app/Application.hpp"

#include "app/HealthMonitor.hpp"
#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/api/MqttReactionListener.hpp"
#include "caelitus/api/OperationsApi.hpp"
#include "caelitus/catalog/mariadb/CatalogMigrations.hpp"
#include "caelitus/catalog/mariadb/MariaDbReactionRepository.hpp"
#include "caelitus/catalog/mariadb/MariaDbRepositories.hpp"
#include "caelitus/core/TimeZone.hpp"
#include "caelitus/db/ConnectionPool.hpp"
#include "caelitus/db/Migrations.hpp"
#include "caelitus/db/SqlExecutor.hpp"
#include "caelitus/db/TransactionManager.hpp"
#include "caelitus/db/mariadb/MariaDbConnection.hpp"
#include "caelitus/json/JsonTypes.hpp"
#include "caelitus/mqtt/mosquitto/MosquittoClient.hpp"
#include "caelitus/net/TcpServer.hpp"

#include <set>
#include <stdexcept>

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
    api::CatalogServices services;
    std::unique_ptr<api::MqttReactionListener> listener;
    std::shared_ptr<HealthMonitor> health;
    std::shared_ptr<api::JsonRpcHandler> rpc;
    std::unique_ptr<net::TcpServer> server;
    std::shared_ptr<scheduler::Scheduler> scheduler;
};

namespace {

using scheduler::JobContext;
using scheduler::JobSpec;
using scheduler::Schedule;
using namespace std::chrono_literals;

// Applies "scheduler.jobs.<name>" from the configuration over a job's defaults.
JobSpec configured(JobSpec spec, const AppConfig& config) {
    auto it = config.scheduler.jobs.find(spec.name);
    if (it == config.scheduler.jobs.end()) return spec;
    const AppConfig::JobSettings& s = it->second;
    if (s.schedule) spec.schedule = Schedule::parse(*s.schedule, TimeZone::named(config.catalog.timeZone));
    if (s.enabled) spec.enabled = *s.enabled;
    if (s.timeout) spec.timeout = *s.timeout;
    if (s.jitter) spec.jitter = *s.jitter;
    if (s.retryAttempts) spec.retryAttempts = *s.retryAttempts;
    if (s.retryDelay) spec.retryDelay = *s.retryDelay;
    return spec;
}

// Today's top 10 as a small JSON array, for the "top-books" job.
std::string topBooksJson(const std::vector<catalog::RankedBook>& top) {
    Json out = Json::array();
    for (const auto& b : top)
        out.push_back({{"bookId", b.id.value},
                       {"title", b.title},
                       {"likes", b.counts.likes},
                       {"dislikes", b.counts.dislikes},
                       {"score", b.counts.score()}});
    return out.dump();
}

}  // namespace

// Every periodic job of the server, with its default schedule. To add a job,
// add an entry here; its name can then be tuned in "scheduler.jobs" of the
// configuration and controlled through the scheduler.* API methods.
std::vector<JobSpec> Application::defineJobs(Components& c, const AppConfig& config) {
    const TimeZone zone = TimeZone::named(config.catalog.timeZone);
    std::vector<JobSpec> jobs;

    {
        JobSpec j{"reaction-flush", "Writes buffered likes/dislikes to the database", Schedule::rate(1s),
                  [reactions = c.services.reactions](JobContext&) {
                      if (reactions->flush().failed)
                          throw std::runtime_error("database unavailable; the likes stay buffered for the next run");
                  }};
        jobs.push_back(j);
    }
    {
        JobSpec j{"book-cache-reload",
                  "Reloads every book into the in-memory cache (catches edits made outside the server)",
                  Schedule::every(5min), [cache = c.bookCache](JobContext&) { cache->reload(); }};
        j.retryAttempts = 3;
        j.retryDelay = 10s;
        jobs.push_back(j);
    }
    {
        JobSpec j{"health", "Checks database, MQTT, cache, server, likes, memory and jobs; logs problems",
                  Schedule::every(15s), [health = c.health](JobContext&) { health->check(); }};
        j.runOnStart = true;  // a full report (server, jobs) right after startup
        jobs.push_back(j);
    }
    {
        JobSpec j{"reaction-cleanup",
                  "Deletes per-day like counts older than catalog.reactions.keepDays (all-time totals stay)",
                  Schedule::cron("0 3 * * *", zone),
                  [reactions = c.services.reactions, keep = config.catalog.reactionKeepDays](JobContext&) {
                      reactions->deleteOlderThan(keep);
                  }};
        j.retryAttempts = 3;
        j.retryDelay = 5min;
        j.timeout = 10min;
        jobs.push_back(j);
    }
    {
        JobSpec j{"top-books", "Publishes today's 10 most liked books to MQTT (retained JSON)", Schedule::every(1min),
                  [reactions = c.services.reactions, mqtt = c.mqtt, topic = config.catalog.topBooksTopic](JobContext&) {
                      const auto top = reactions->top(catalog::Period::Today, catalog::ReactionOrder::MostLiked, 10);
                      if (!mqtt->publish(topic, topBooksJson(top), mqtt::PublishOptions::retained()))
                          throw std::runtime_error("MQTT broker not connected");
                  }};
        j.runOnStart = true;
        jobs.push_back(j);
    }

    // Every job named in the configuration must exist: a typo would otherwise
    // be silently ignored.
    std::set<std::string> known;
    for (const auto& j : jobs) known.insert(j.name);
    for (const auto& [name, settings] : config.scheduler.jobs)
        if (!known.count(name)) {
            std::string list;
            for (const auto& k : known) list += (list.empty() ? "" : ", ") + k;
            throw ConfigError("scheduler.jobs." + name + ": unknown job (known jobs: " + list + ")");
        }
    for (auto& j : jobs) j = configured(std::move(j), config);
    return jobs;
}

Application::Application(AppConfig config)
    : config_(std::move(config)),
      log_(log::get("app")),
      c_(std::make_unique<Components>()) {}

Application::~Application() { stop(); }

void Application::start() {
    Components& c = *c_;
    const Timestamp startedAt = nowUtc();

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
    // fully reloaded by the "book-cache-reload" job.
    c.bookCache =
        std::make_shared<catalog::BookCache>(std::make_shared<catalog::mariadb::MariaDbBookRepository>(c.sql));
    c.bookCache->reload();
    log_->info("Book cache loaded: {} books", c.bookCache->stats().size);

    c.services = makeCatalogServices(config_.catalog, c.sql, c.tx, c.mqtt, c.bookCache);

    // Inbound: likes/dislikes over MQTT, counted in memory and written by the "reaction-flush" job.
    c.listener =
        std::make_unique<api::MqttReactionListener>(c.mqtt, c.services.reactions, config_.catalog.reactionTopicPrefix);

    // Jobs: defined (and the configuration checked) before anything listens.
    c.scheduler = std::make_shared<scheduler::Scheduler>(scheduler::SchedulerConfig{config_.scheduler.threads});
    c.health = std::make_shared<HealthMonitor>(HealthMonitor::Sources{c.pool, config_.db.pool.maxSize, c.sql,
                                                                      c.bookCache, c.services.reactions, c.mqtt,
                                                                      nullptr, c.scheduler.get()},
                                               config_.health, startedAt);
    for (auto& job : defineJobs(c, config_)) c.scheduler->add(std::move(job));
    c.health->check();  // system.health has a report from the first request on

    // Inbound: JSON-RPC requests over TCP.
    c.rpc = std::make_shared<api::JsonRpcHandler>();
    api::registerCatalogApi(*c.rpc, c.services);
    api::registerOperationsApi(*c.rpc, c.scheduler, [health = c.health] { return health->latest(); });
    c.server = std::make_unique<net::TcpServer>(config_.server, c.rpc);
    c.server->start();
    c.health->setServer(c.server.get());

    c.scheduler->start();
    log_->info("Ready on port {}: {} API methods, {} scheduled jobs", c.server->port(), c.rpc->methods().size(),
               c.scheduler->list().size());
}

void Application::stop() {
    Components& c = *c_;
    if (c.server) c.server->stop();
    c.listener.reset();                    // no new likes from here on...
    if (c.scheduler) c.scheduler->stop();  // waits for a running flush or other job
    if (c.services.reactions) {            // ...so this flush stores every like received
        const auto last = c.services.reactions->flush();
        if (last.written > 0) log_->info("Stored {} buffered reactions", last.written);
        c.services.reactions.reset();
    }
    if (c.mqtt) c.mqtt->stop();
    c_ = std::make_unique<Components>();  // releases everything; stop() is now a no-op
}

std::uint16_t Application::port() const { return c_->server ? c_->server->port() : 0; }

std::shared_ptr<scheduler::Scheduler> Application::scheduler() const { return c_->scheduler; }

}  // namespace caelitus::app
