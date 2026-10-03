#pragma once

/// @file
/// The assembled server: every component created, wired and started in order.
/// @ingroup app

#include "caelitus/api/CatalogApi.hpp"
#include "caelitus/catalog/service/BookCache.hpp"
#include "caelitus/config/AppConfig.hpp"
#include "caelitus/db/ITransactionManager.hpp"
#include "caelitus/db/SqlExecutor.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/mqtt/IMqttClient.hpp"

#include <cstdint>
#include <memory>

namespace caelitus::app {

/// The running caelitus server.
///
/// Owns every component and the order in which they start and stop:
///
/// | Start (in this order)                          | Stop (reverse)                                  |
/// |------------------------------------------------|-------------------------------------------------|
/// | MQTT client (background reconnects)            | MQTT client (publishes "offline")               |
/// | MariaDB pool, migrations                       | final flush of buffered likes                   |
/// | book cache + periodic reload                   | cache reload task                               |
/// | catalog services                               | flush task                                      |
/// | MQTT like/dislike listener + flush task        | MQTT listener (no new likes)                    |
/// | JSON-RPC handler + TCP server                  | TCP server (in-flight requests finish)          |
///
/// The MQTT client starts first so that a broker outage never blocks startup
/// (the server keeps working; likes resume when the broker returns), while
/// the database is required: start() fails if it cannot be reached.
class Application {
public:
    /// Takes the validated configuration. Logging must already be initialized
    /// (log::init) so components pick up the configured loggers.
    explicit Application(AppConfig config);

    /// Stops the server if still running.
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Starts every component.
    /// @throws db::DatabaseError if the database cannot be reached or migrated.
    /// @throws net::NetError if the TCP port cannot be bound.
    void start();

    /// Stops gracefully: the TCP server lets in-flight requests finish, the
    /// MQTT listener stops taking likes, buffered likes are written, and the
    /// MQTT client publishes the "offline" status. Idempotent.
    void stop();

    /// The TCP port the server listens on (useful with port 0 in tests).
    std::uint16_t port() const;

private:
    struct Components;

    AppConfig config_;
    log::Logger log_;
    std::unique_ptr<Components> c_;
};

/// Builds the catalog services on MariaDB repositories.
///
/// @param config     The "catalog" configuration section (time zone, reaction buffer).
/// @param sql        Statement executor used by every repository.
/// @param tx         Transaction manager the services group their work with.
/// @param events     Where catalog events go ("catalog/books/<id>/created", ...); may be null.
/// @param bookCache  The in-memory book cache, refreshed by BookService and read by ReactionService.
api::CatalogServices makeCatalogServices(const AppConfig::Catalog& config, const std::shared_ptr<db::SqlExecutor>& sql,
                                         const std::shared_ptr<db::ITransactionManager>& tx,
                                         const std::shared_ptr<mqtt::IMqttPublisher>& events,
                                         const std::shared_ptr<catalog::BookCache>& bookCache);

}  // namespace caelitus::app
