# Libraries, one per module, from the bottom of the dependency graph up.
# Technology-specific code (MariaDB, libmosquitto) lives in its own library,
# so the catalog services cannot even link against a database driver:
#
#   log, core, json, cache
#     <- db, mqtt, net                       (abstractions)
#     <- db_mariadb, mqtt_mosquitto          (drivers)
#     <- catalog                             (domain + services; db/mqtt interfaces only)
#     <- catalog_mariadb                     (repositories + schema migrations)
#     <- api, config
#     <- caelitus                            (the executable: wires everything)

caelitus_module(log
        SOURCES log/Log.cpp
        PUBLIC spdlog::spdlog)

caelitus_module(core
        SOURCES core/DateTime.cpp core/PeriodicTask.cpp core/TimeZone.cpp
        PUBLIC caelitus::log Threads::Threads)

caelitus_module(json
        PUBLIC caelitus::core nlohmann_json::nlohmann_json)

caelitus_module(cache
        PUBLIC Threads::Threads)

# ---- database --------------------------------------------------------------------
caelitus_module(db
        SOURCES
        db/ConnectionPool.cpp
        db/Migrations.cpp
        db/SqlExecutor.cpp
        db/TransactionManager.cpp
        db/detail/SqlDateTime.cpp
        db/detail/TransactionBinding.cpp
        PUBLIC caelitus::core caelitus::log Threads::Threads)

caelitus_module(db_mariadb
        SOURCES db/mariadb/MariaDbConnection.cpp db/mariadb/MariaDbErrorTranslator.cpp
        PUBLIC caelitus::db
        PRIVATE ${MARIADBCPP_LIBRARY})
target_include_directories(caelitus_db_mariadb PRIVATE ${MARIADBCPP_INCLUDE_DIR})

# ---- MQTT ------------------------------------------------------------------------
caelitus_module(mqtt
        SOURCES mqtt/MqttClientBase.cpp mqtt/Topic.cpp
        PUBLIC caelitus::log Threads::Threads)

caelitus_module(mqtt_mosquitto
        SOURCES mqtt/mosquitto/MosquittoClient.cpp
        PUBLIC caelitus::mqtt
        PRIVATE ${MOSQUITTO_LIBRARY})
target_include_directories(caelitus_mqtt_mosquitto PRIVATE ${MOSQUITTO_INCLUDE_DIR})

# ---- TCP server ------------------------------------------------------------------
caelitus_module(net
        SOURCES net/TcpServer.cpp
        PUBLIC caelitus::log
        PRIVATE caelitus_asio)

# ---- book catalog ----------------------------------------------------------------
caelitus_module(catalog
        SOURCES
        catalog/domain/Rules.cpp
        catalog/service/AuthorService.cpp
        catalog/service/BookCache.cpp
        catalog/service/BookService.cpp
        catalog/service/CategoryService.cpp
        catalog/service/ReactionService.cpp
        catalog/service/ReviewService.cpp
        PUBLIC caelitus::core caelitus::db caelitus::mqtt caelitus::cache)

caelitus_module(catalog_mariadb
        SOURCES
        catalog/mariadb/CatalogMigrations.cpp
        catalog/mariadb/MariaDbBookRepository.cpp
        catalog/mariadb/MariaDbReactionRepository.cpp
        catalog/mariadb/MariaDbRepositories.cpp
        PUBLIC caelitus::catalog caelitus::db)

# ---- JSON-RPC API and configuration ----------------------------------------------
caelitus_module(api
        SOURCES
        api/CatalogApi.cpp
        api/JsonRpc.cpp
        api/MqttReactionListener.cpp
        api/OpenRpc.cpp
        api/SchemaValidator.cpp
        PUBLIC caelitus::catalog caelitus::json caelitus::net caelitus::mqtt)

caelitus_module(config
        SOURCES config/AppConfig.cpp
        PUBLIC caelitus::db caelitus::mqtt caelitus::net caelitus::log
        PRIVATE caelitus::json)

# ---- the server ----------------------------------------------------------------
add_executable(caelitus app/main.cpp app/Application.cpp)
target_include_directories(caelitus PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(caelitus PRIVATE
        caelitus::config caelitus::api caelitus::catalog_mariadb caelitus::db_mariadb caelitus::mqtt_mosquitto
        caelitus_build_options)

# Regenerate the committed API description and database schema:
#   cmake --build <dir> --target openrpc      -> docs/openrpc.json
#   cmake --build <dir> --target schema       -> db/schema.sql
add_custom_target(openrpc
        COMMAND caelitus --openrpc > ${PROJECT_SOURCE_DIR}/docs/openrpc.json
        DEPENDS caelitus
        COMMENT "Writing docs/openrpc.json"
        VERBATIM)
add_custom_target(schema
        COMMAND caelitus --schema > ${PROJECT_SOURCE_DIR}/db/schema.sql
        DEPENDS caelitus
        COMMENT "Writing db/schema.sql"
        VERBATIM)
