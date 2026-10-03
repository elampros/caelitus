# caelitus C++ API {#mainpage}

caelitus is a book catalog server written in C++17. Clients call it with
**JSON-RPC 2.0 over TCP**, the data lives in **MariaDB**, and readers' likes and
dislikes arrive as **MQTT** messages. This is the reference for its C++ code.
For the full picture start with `README.md` in the repository: it explains how
to build, run and test the server, and walks through the design.

## How the code is organized

The code is split into small libraries ("modules"), each with one job. The
arrows point from a module to what it uses:

@dot
digraph modules {
    rankdir=BT;
    bgcolor="transparent";
    node [shape=box, style="rounded,filled", fillcolor="#eef4fc", color="#2f6fc4", fontname="Helvetica", fontsize=11];
    edge [color="#8a94a6", arrowsize=0.7];

    app [label="app\n(main, Application)", fillcolor="#dbe8fa", URL="\ref app"];
    api [label="api\nJSON-RPC methods", URL="\ref api"];
    config [label="config", URL="\ref config"];
    catalog_mariadb [label="catalog_mariadb\nSQL repositories", URL="\ref catalog_mariadb"];
    catalog [label="catalog\ndomain + services", fillcolor="#fff4d6", color="#c99a1a", URL="\ref catalog"];
    db_mariadb [label="db_mariadb\ndriver", URL="\ref db_mariadb"];
    mqtt_mosquitto [label="mqtt_mosquitto\ndriver", URL="\ref mqtt_mosquitto"];
    db [label="db\npool, SQL, transactions", URL="\ref db"];
    mqtt [label="mqtt\nclient core", URL="\ref mqtt"];
    net [label="net\nTCP server", URL="\ref net"];
    base [label="core, log, json, cache", URL="\ref core"];

    app -> api; app -> config; app -> catalog_mariadb; app -> db_mariadb; app -> mqtt_mosquitto;
    api -> catalog; api -> net; api -> mqtt;
    catalog_mariadb -> catalog; catalog_mariadb -> db;
    catalog -> db; catalog -> mqtt; catalog -> base;
    db_mariadb -> db; mqtt_mosquitto -> mqtt;
    db -> base; mqtt -> base; net -> base; config -> db; config -> mqtt; config -> net;
}
@enddot

The rule that shapes it: **the business logic (@ref catalog) knows neither the
database nor the MQTT library.** Services talk to interfaces
(@ref caelitus::catalog::IBookRepository "catalog::IBookRepository", @ref caelitus::db::ITransactionManager "db::ITransactionManager", @ref caelitus::mqtt::IMqttPublisher "mqtt::IMqttPublisher"); the
MariaDB and libmosquitto code lives in separate libraries that the services
cannot even link against. Only @ref caelitus::app::Application "app::Application", the wiring, sees everything.
That is why every service can be unit-tested with in-memory fakes, in
milliseconds, without a database.

| Module | What it does | Start reading at |
|--------|--------------|------------------|
| @ref catalog | Books, authors, categories, reviews, likes: the rules | @ref caelitus::catalog::BookService "catalog::BookService" |
| @ref api | JSON-RPC protocol, the API methods, OpenRPC, MQTT likes | @ref caelitus::api::JsonRpcHandler "api::JsonRpcHandler", @ref caelitus::api::registerCatalogApi "api::registerCatalogApi()" |
| @ref db | Connection pool, statements, transactions with retries, migrations | @ref caelitus::db::SqlExecutor "db::SqlExecutor", @ref caelitus::db::ITransactionManager "db::ITransactionManager" |
| @ref catalog_mariadb | The SQL behind the catalog repositories; the schema | @ref caelitus::catalog::mariadb::catalogMigrations "catalog::mariadb::catalogMigrations()" |
| @ref mqtt | MQTT client: subscriptions, reconnects, online/offline status | @ref caelitus::mqtt::IMqttClient "mqtt::IMqttClient" |
| @ref net | Asynchronous TCP server for `\0`-terminated messages | @ref caelitus::net::TcpServer "net::TcpServer" |
| @ref config | `config.json` loading and validation | @ref caelitus::AppConfig "AppConfig" |
| @ref core | Dates, time zones, domain errors, periodic tasks | @ref caelitus::DomainError "DomainError", @ref caelitus::TimeZone "TimeZone" |
| @ref app | `main()` and the start/stop order | @ref caelitus::app::Application "app::Application" |

The full list is under **Topics** in the menu.

## One request, end to end

`books.get {"id": 42}` sent by a client:

1. @ref caelitus::net::TcpServer "net::TcpServer" reads bytes until a `\0` and hands the message to a worker thread.
2. @ref caelitus::api::JsonRpcHandler "api::JsonRpcHandler" parses it, finds the method, and checks `params`
   against the method's JSON Schema (unknown or missing fields are rejected here).
3. The method calls @ref caelitus::catalog::BookService::get "catalog::BookService::get()", which opens a read-only
   transaction through @ref caelitus::db::ITransactionManager "db::ITransactionManager".
4. @ref caelitus::catalog::mariadb::MariaDbBookRepository "catalog::mariadb::MariaDbBookRepository" runs its SQL through
   @ref caelitus::db::SqlExecutor "db::SqlExecutor", which uses that transaction's pooled connection.
5. The result travels back up as @ref caelitus::catalog::BookDetails "catalog::BookDetails", is converted to JSON and
   sent with a trailing `\0`.

If anything fails, the exception type decides the JSON-RPC error code (see
@ref caelitus::api::JsonRpcHandler "api::JsonRpcHandler"): a @ref caelitus::catalog::BookService::get "catalog::BookService::get()" of a missing book throws
@ref caelitus::NotFoundError "NotFoundError", which becomes error `-32001` with `data.code = "not_found"`.

## A like, end to end

A message on `catalog/in/books/42/like`:

1. @ref caelitus::mqtt::MosquittoClient "mqtt::MosquittoClient" receives it on its network thread and queues it; the
   dispatcher thread calls @ref caelitus::api::MqttReactionListener "api::MqttReactionListener".
2. @ref caelitus::catalog::ReactionService::record "catalog::ReactionService::record()" asks @ref caelitus::catalog::BookCache "catalog::BookCache" (memory only)
   whether book 42 accepts reactions, and adds 1 to an in-memory counter.
3. Once a second, a @ref caelitus::PeriodicTask "PeriodicTask" calls @ref caelitus::catalog::ReactionService::flush "catalog::ReactionService::flush()", which
   writes every accumulated counter in a single transaction.

## Conventions

- **Errors**: services throw @ref caelitus::DomainError "DomainError" subclasses (@ref caelitus::ValidationError "ValidationError",
  @ref caelitus::NotFoundError "NotFoundError", @ref caelitus::ConflictError "ConflictError"); the database layer throws @ref caelitus::db::DatabaseError "db::DatabaseError"
  subclasses; nothing above `db` ever sees a driver exception.
- **Thread safety** is stated in each class's description. Services,
  repositories, the pool and the clients are safe to share between threads.
- **Logging**: each component has a named logger (`db.sql`, `mqtt`, `server`,
  ...), listed in its class description; repeating messages are throttled.
- **Time**: @ref caelitus::Timestamp "Timestamp" is always UTC; local days (for likes) use the
  configured @ref caelitus::TimeZone "TimeZone".
