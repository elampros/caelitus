// Unit tests for AppConfig.

#include "TestHarness.hpp"

#include "caelitus/config/AppConfig.hpp"

#include <cstdlib>
#include <fstream>

using caelitus::AppConfig;
using caelitus::ConfigError;

namespace {

// Smallest valid configuration; tests add or break one thing at a time.
const char* kMinimal = R"({
  "mqtt":   { "server": "broker" },
  "db":     { "user": "app", "database": "appdb" },
  "server": { "port": 9000 }
})";

// Expects a ConfigError whose message contains `fragment`.
void expectError(const std::string& json, const std::string& fragment) {
    try {
        AppConfig::fromJson(json, "test.json");
    } catch (const ConfigError& e) {
        const std::string msg = e.what();
        if (msg.find(fragment) == std::string::npos)
            throw test::Failure{"error '" + msg + "' does not mention '" + fragment + "'"};
        return;
    }
    throw test::Failure{"no ConfigError for: " + json};
}

std::string withDb(const std::string& dbBody) {
    return R"({ "mqtt": { "server": "broker" }, "server": { "port": 9000 }, "db": )" + dbBody + "}";
}

}  // namespace

TEST(minimal_config_uses_defaults) {
    auto c = AppConfig::fromJson(kMinimal);
    CHECK_EQ(c.mqtt.server, "broker");
    CHECK_EQ(c.mqtt.port, 1883);
    CHECK_EQ(c.db.connection.host, "127.0.0.1");
    CHECK_EQ(c.db.connection.port, 3306);
    CHECK_EQ(c.db.connection.user, "app");
    CHECK_EQ(c.db.connection.sessionTimeZone, "+00:00");
    CHECK_EQ(c.db.pool.maxSize, 8u);
    CHECK_EQ(c.db.transaction.retry.maxAttempts, 3);
    CHECK_EQ(c.server.port, 9000);
    CHECK_EQ(c.log.level, "info");
}

TEST(full_config_is_read) {
    auto c = AppConfig::fromJson(R"({
      // comments are allowed
      "mqtt": { "server": "10.0.0.5", "port": 8883, "clientId": "c1", "username": "u", "password": "p",
                "keepAliveSec": 60 },
      "db": {
        "host": "db.local", "port": 3307, "user": "app", "password": "secret", "database": "appdb",
        "connectTimeoutMs": 1000, "socketTimeoutMs": 0, "sessionTimeZone": "+02:00",
        "initStatements": ["SET SESSION sql_mode = 'STRICT_ALL_TABLES'"], "slowQueryMs": 100,
        "pool": { "maxSize": 4, "acquireTimeoutMs": 250, "validateAfterIdleMs": 10000 },
        "transaction": { "maxAttempts": 5, "initialBackoffMs": 10, "backoffMultiplier": 1.5,
                         "slowTransactionMs": 900 }
      },
      "server": { "port": 1234, "log": "debug" },
      "log": { "level": "warn", "levels": { "db.sql": "trace" }, "console": false, "file": "/tmp/x.log",
               "maxFileSizeMb": 2, "maxFiles": 3, "pattern": "%v" }
    })");

    CHECK_EQ(c.mqtt.port, 8883);
    CHECK_EQ(c.mqtt.keepAlive.count(), 60);
    CHECK_EQ(c.db.connection.host, "db.local");
    CHECK_EQ(c.db.connection.socketTimeout.count(), 0);
    CHECK_EQ(c.db.connection.initStatements.size(), 1u);
    CHECK_EQ(c.db.executor.slowQueryThreshold.count(), 100);
    CHECK_EQ(c.db.pool.maxSize, 4u);
    CHECK_EQ(c.db.pool.acquireTimeout.count(), 250);
    CHECK_EQ(c.db.transaction.retry.backoffMultiplier, 1.5);
    CHECK_EQ(c.db.transaction.slowTransactionThreshold.count(), 900);
    CHECK_EQ(c.server.port, 1234);
    CHECK_EQ(c.log.level, "warn");
    CHECK_EQ(c.log.levels.at("db.sql"), "trace");
    CHECK_EQ(c.log.maxFileSizeBytes, 2u * 1024 * 1024);
    CHECK(!c.log.console);
}

TEST(mqtt_settings_are_read_and_validated) {
    auto c = AppConfig::fromJson(R"({ "mqtt": { "server": "b", "qos": 1, "retain": true, "cleanSession": false,
                                                "reconnectMinDelaySec": 2, "reconnectMaxDelaySec": 60,
                                                "incomingQueueSize": 50 },
                                      "db": { "user": "u", "database": "d" }, "server": { "port": 1 } })");
    CHECK(c.mqtt.qos == caelitus::mqtt::QoS::AtLeastOnce);
    CHECK(c.mqtt.retain);
    CHECK(!c.mqtt.cleanSession);
    CHECK_EQ(c.mqtt.reconnectMaxDelay.count(), 60);
    CHECK_EQ(c.mqtt.incomingQueueSize, 50u);

    auto mqttWith = [](const std::string& body) {
        return R"({ "db": { "user": "u", "database": "d" }, "server": { "port": 1 }, "mqtt": )" + body + "}";
    };
    expectError(mqttWith(R"({ "server": "b", "qos": 3 })"), "mqtt.qos: must be between 0 and 2");
    expectError(mqttWith(R"({ "server": "b", "retain": 1 })"), "mqtt.retain: must be true or false");
    expectError(mqttWith(R"({ "server": "b", "reconnectMinDelaySec": 10, "reconnectMaxDelaySec": 5 })"),
                "mqtt.reconnectMaxDelaySec");
    expectError(mqttWith(R"({ "server": "b", "clientId": "", "cleanSession": false })"), "mqtt.clientId");
}

TEST(mqtt_will_is_read_and_validated) {
    auto mqttWith = [](const std::string& body) {
        return R"({ "db": { "user": "u", "database": "d" }, "server": { "port": 1 }, "mqtt": )" + body + "}";
    };
    auto c = AppConfig::fromJson(mqttWith(R"({ "server": "b" })"));
    CHECK(!c.mqtt.will.has_value());

    c = AppConfig::fromJson(mqttWith(R"({ "server": "b", "will": { "topic": "app/status" } })"));
    CHECK(c.mqtt.will.has_value());
    CHECK_EQ(c.mqtt.will->payload, "offline");
    CHECK_EQ(c.mqtt.will->onlinePayload.value(), "online");
    CHECK(c.mqtt.will->qos == caelitus::mqtt::QoS::AtLeastOnce);
    CHECK(c.mqtt.will->retain);

    c = AppConfig::fromJson(mqttWith(R"({ "server": "b", "will": { "topic": "t", "payload": "dead",
                                          "onlinePayload": "", "qos": 0, "retain": false } })"));
    CHECK_EQ(c.mqtt.will->payload, "dead");
    CHECK(!c.mqtt.will->onlinePayload.has_value());
    CHECK(!c.mqtt.will->retain);

    expectError(mqttWith(R"({ "server": "b", "will": {} })"), "mqtt.will.topic: is required");
    expectError(mqttWith(R"({ "server": "b", "will": { "topic": "a/#" } })"),
                "mqtt.will.topic: Topic 'a/#' must not contain wildcards");
    expectError(mqttWith(R"({ "server": "b", "will": { "topic": "t", "msg": "x" } })"), "mqtt.will.msg: unknown key");
}

TEST(server_settings_are_read) {
    auto c = AppConfig::fromJson(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" },
        "server": { "port": 9000, "bindAddress": "127.0.0.1", "ioThreads": 1, "workerThreads": 16,
                    "maxConnections": 500, "maxMessageBytes": 4096, "maxPendingRequests": 4,
                    "idleTimeoutSec": 60, "slowRequestMs": 100, "shutdownTimeoutSec": 5 } })");
    CHECK_EQ(c.server.bindAddress, "127.0.0.1");
    CHECK_EQ(c.server.workerThreads, 16u);
    CHECK_EQ(c.server.maxMessageBytes, 4096u);
    CHECK_EQ(c.server.idleTimeout.count(), 60);
    CHECK_EQ(c.server.slowRequestThreshold.count(), 100);
    expectError(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" },
                     "server": { "port": 1, "ioThreads": 0 } })",
                "server.ioThreads: must be between 1 and 64");
}

TEST(server_log_sets_server_logger_level) {
    auto c = AppConfig::fromJson(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" },
                                      "server": { "port": 1, "log": "debug" } })");
    CHECK_EQ(c.log.levels.at("server"), "debug");

    // An explicit log.levels.server wins.
    c = AppConfig::fromJson(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" },
                                 "server": { "port": 1, "log": "debug" },
                                 "log": { "levels": { "server": "error" } } })");
    CHECK_EQ(c.log.levels.at("server"), "error");
}

TEST(missing_required_keys_are_reported_with_path) {
    expectError(R"({ "db": { "user": "u", "database": "d" }, "server": { "port": 1 } })", "mqtt.server: is required");
    expectError(withDb(R"({ "database": "d" })"), "db.user: is required");
    expectError(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" } })", "server.port: is required");
}

TEST(unknown_keys_are_rejected) {
    expectError(withDb(R"({ "user": "u", "database": "d", "prot": 3306 })"), "db.prot: unknown key");
    expectError(withDb(R"({ "user": "u", "database": "d", "pool": { "max": 3 } })"), "db.pool.max: unknown key");
    expectError(std::string(kMinimal).insert(1, R"("extra": 1,)"), "extra: unknown key");
}

TEST(wrong_types_and_ranges_are_rejected) {
    expectError(withDb(R"({ "user": "u", "database": "d", "port": "3306" })"), "db.port: must be an integer");
    expectError(withDb(R"({ "user": "u", "database": "d", "port": 3306.5 })"), "db.port: must be an integer");
    expectError(withDb(R"({ "user": "u", "database": "d", "port": 70000 })"), "db.port: must be between 1 and 65535");
    expectError(withDb(R"({ "user": "u", "database": "d", "port": -1 })"), "db.port: must be between");
    expectError(withDb(R"({ "user": "u", "database": "d", "pool": { "maxSize": 0 } })"), "db.pool.maxSize");
    expectError(withDb(R"({ "user": "u", "database": "d", "pool": { "maxSize": 18446744073709551615 } })"),
                "db.pool.maxSize");
    expectError(withDb(R"({ "user": 5, "database": "d" })"), "db.user: must be a string");
    expectError(withDb(R"({ "user": "u", "database": "d", "pool": 3 })"), "db.pool: must be an object");
    expectError(withDb(R"({ "user": "u", "database": "d", "transaction": { "backoffMultiplier": 0.5 } })"),
                "db.transaction.backoffMultiplier");
}

TEST(log_levels_are_validated) {
    expectError(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" },
                     "server": { "port": 1, "log": "verbose" } })",
                "server.log: unknown log level 'verbose'");
    expectError(R"({ "mqtt": { "server": "b" }, "db": { "user": "u", "database": "d" }, "server": { "port": 1 },
                     "log": { "levels": { "db": "loud" } } })",
                "log.levels.db");
}

TEST(environment_variables_are_substituted) {
    setenv("CAELITUS_TEST_SECRET", "s3cret", 1);
    auto c = AppConfig::fromJson(withDb(R"({ "user": "u", "database": "d", "password": "${CAELITUS_TEST_SECRET}" })"));
    CHECK_EQ(c.db.connection.password, "s3cret");

    unsetenv("CAELITUS_TEST_MISSING");
    expectError(withDb(R"({ "user": "u", "database": "d", "password": "${CAELITUS_TEST_MISSING}" })"),
                "db.password: environment variable CAELITUS_TEST_MISSING is not set");

    // Only a whole-string reference is substituted.
    c = AppConfig::fromJson(withDb(R"({ "user": "u", "database": "d", "password": "a${CAELITUS_TEST_SECRET}" })"));
    CHECK_EQ(c.db.connection.password, "a${CAELITUS_TEST_SECRET}");
}

TEST(invalid_json_reports_position) {
    expectError(R"({ "mqtt": { "server": "b", } })", "test.json: invalid JSON");
    expectError(R"({ "mqtt": { "server": "b" )", "line 1");
}

TEST(load_reads_file_and_reports_missing_file) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "caelitus_config_test";
    fs::create_directories(dir);
    const fs::path file = dir / "config.json";
    std::ofstream(file) << kMinimal;

    CHECK_EQ(AppConfig::load(file).server.port, 9000);
    CHECK_THROWS_AS(AppConfig::load(dir / "nope.json"), ConfigError);
    fs::remove_all(dir);
}

TEST(locate_honours_flag_and_environment) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "caelitus_locate_test";
    fs::create_directories(dir);
    const fs::path file = dir / "custom.json";
    std::ofstream(file) << kMinimal;
    const std::string fileStr = file.string();

    const char* withFlag[] = {"app", "--config", fileStr.c_str()};
    CHECK(AppConfig::locate(3, withFlag) == file);
    const std::string eqForm = "--config=" + fileStr;
    const char* withEq[] = {"app", eqForm.c_str()};
    CHECK(AppConfig::locate(2, withEq) == file);

    const char* missing[] = {"app", "--config", "/no/such/file.json"};
    CHECK_THROWS_AS(AppConfig::locate(3, missing), ConfigError);

    setenv("CAELITUS_CONFIG", fileStr.c_str(), 1);
    const char* plain[] = {"app"};
    CHECK(AppConfig::locate(1, plain) == file);
    unsetenv("CAELITUS_CONFIG");

    // Falls back to ./config/config.json.
    const fs::path cwd = fs::current_path();
    fs::create_directories(dir / "config");
    std::ofstream(dir / "config" / "config.json") << kMinimal;
    fs::current_path(dir);
    const bool found = AppConfig::locate(1, plain) == dir / "config" / "config.json";
    fs::current_path(cwd);
    CHECK(found);
    fs::remove_all(dir);
}

TEST(locate_finds_config_above_the_executable) {
    // Run from a directory without a config: the search walks up from the
    // executable (build/<preset>/tests/) and finds the project's config.
    namespace fs = std::filesystem;
    const fs::path source = fs::path(CAELITUS_SOURCE_DIR).lexically_normal();
    const fs::path exe = fs::read_symlink("/proc/self/exe");
    const auto rel = exe.lexically_relative(source);
    if (rel.empty() || *rel.begin() == "..") return;  // build tree outside the sources: nothing to find

    const fs::path cwd = fs::current_path();
    const fs::path empty = fs::temp_directory_path() / "caelitus_locate_empty";
    fs::create_directories(empty);
    fs::current_path(empty);
    const char* plain[] = {"app"};
    fs::path found;
    try {
        found = AppConfig::locate(1, plain);
    } catch (...) {
    }
    fs::current_path(cwd);
    fs::remove_all(empty);
    CHECK(found == source / "config" / "config.json");
}

TEST(project_config_file_is_valid) {
    // The shipped config/config.json must always parse.
    setenv("CAELITUS_DB_PASSWORD", "x", 1);
    auto c = AppConfig::load(std::filesystem::path(CAELITUS_SOURCE_DIR) / "config" / "config.json");
    CHECK(c.server.port > 0);
}

int main() { return test::runAll(); }
