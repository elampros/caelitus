/// @file
/// AppConfig: locating, parsing and validating `config.json` (strict keys,
/// ranges, `${ENV}` substitution).
/// @ingroup config

#include "caelitus/config/AppConfig.hpp"

#include "caelitus/core/TimeZone.hpp"
#include "caelitus/json/JsonTypes.hpp"
#include "caelitus/mqtt/Topic.hpp"
#include "caelitus/scheduler/Schedule.hpp"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace caelitus {

namespace {

using nlohmann::json;

// Typed, path-aware reader over one JSON object. Remembers which keys were
// read so that leftovers can be reported as unknown.
class Section {
public:
    Section(const json* node, std::string path, const std::string& source)
        : node_(node),
          path_(std::move(path)),
          source_(source) {
        if (node_ && !node_->is_object()) failSelf("must be an object");
    }

    std::string requiredString(const char* key) {
        const json* v = find(key);
        if (!v) fail(key, "is required");
        return toString(*v, key);
    }

    std::string string(const char* key, std::string fallback) {
        const json* v = find(key);
        return v ? toString(*v, key) : std::move(fallback);
    }

    template <typename Int>
    Int integer(const char* key, Int fallback, Int min = std::numeric_limits<Int>::min(),
                Int max = std::numeric_limits<Int>::max()) {
        const json* v = find(key);
        return v ? toInteger<Int>(*v, key, min, max) : fallback;
    }

    template <typename Int>
    Int requiredInteger(const char* key, Int min, Int max) {
        const json* v = find(key);
        if (!v) fail(key, "is required");
        return toInteger<Int>(*v, key, min, max);
    }

    std::chrono::milliseconds millis(const char* key, std::chrono::milliseconds fallback, std::int64_t min = 0) {
        return std::chrono::milliseconds(integer<std::int64_t>(key, fallback.count(), min));
    }

    double number(const char* key, double fallback, double min, double max) {
        const json* v = find(key);
        if (!v) return fallback;
        if (!v->is_number()) fail(key, "must be a number");
        const double d = v->get<double>();
        if (d < min || d > max) fail(key, "must be between " + fmt(min) + " and " + fmt(max));
        return d;
    }

    bool boolean(const char* key, bool fallback) {
        const json* v = find(key);
        if (!v) return fallback;
        if (!v->is_boolean()) fail(key, "must be true or false");
        return v->get<bool>();
    }

    std::vector<std::string> stringList(const char* key, std::vector<std::string> fallback) {
        const json* v = find(key);
        if (!v) return fallback;
        if (!v->is_array()) fail(key, "must be an array of strings");
        std::vector<std::string> out;
        for (std::size_t i = 0; i < v->size(); ++i)
            out.push_back(toString((*v)[i], std::string(key) + "[" + std::to_string(i) + "]"));
        return out;
    }

    std::map<std::string, std::string> stringMap(const char* key) {
        const json* v = find(key);
        if (!v) return {};
        if (!v->is_object()) fail(key, "must be an object of strings");
        std::map<std::string, std::string> out;
        for (const auto& [k, item] : v->items()) out.emplace(k, toString(item, std::string(key) + "." + k));
        return out;
    }

    Section section(const char* key) { return Section(find(key), qualified(key), source_); }

    bool present() const noexcept { return node_ != nullptr; }

    /// Every key of this object, marked as read (for maps keyed by name).
    std::vector<std::string> keys() {
        std::vector<std::string> out;
        if (!node_) return out;
        for (const auto& [key, value] : node_->items()) {
            used_.insert(key);
            out.push_back(key);
        }
        return out;
    }
    bool has(const char* key) const { return node_ && node_->contains(key); }

    // Call after reading every known key.
    void rejectUnknownKeys() const {
        if (!node_) return;
        for (const auto& [key, value] : node_->items())
            if (!used_.count(key)) fail(key, "unknown key");
    }

    [[noreturn]] void fail(const std::string& key, const std::string& message) const {
        throw ConfigError(source_ + ": " + qualified(key) + ": " + message);
    }

private:
    const json* find(const char* key) {
        used_.insert(key);
        if (!node_) return nullptr;
        auto it = node_->find(key);
        return it == node_->end() ? nullptr : &*it;
    }

    std::string qualified(const std::string& key) const { return path_.empty() ? key : path_ + "." + key; }

    [[noreturn]] void failSelf(const std::string& message) const {
        throw ConfigError(source_ + ": " + (path_.empty() ? "<root>" : path_) + ": " + message);
    }

    // Strings may reference environment variables: "${NAME}".
    std::string toString(const json& v, const std::string& key) const {
        if (!v.is_string()) fail(key, "must be a string");
        const auto& s = v.get_ref<const std::string&>();
        if (s.size() > 3 && s.compare(0, 2, "${") == 0 && s.back() == '}') {
            const std::string name = s.substr(2, s.size() - 3);
            const char* value = std::getenv(name.c_str());
            if (!value) fail(key, "environment variable " + name + " is not set");
            return value;
        }
        return s;
    }

    template <typename Int>
    Int toInteger(const json& v, const std::string& key, Int min, Int max) const {
        if (!v.is_number_integer()) fail(key, "must be an integer");
        const bool tooBig =
            v.is_number_unsigned() &&
            v.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const std::int64_t i = tooBig ? 0 : v.get<std::int64_t>();
        // Bounds as int64 (an unsigned 64-bit max would otherwise wrap to -1).
        constexpr auto int64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        const std::int64_t hi = static_cast<std::uint64_t>(max) > int64Max ? std::numeric_limits<std::int64_t>::max()
                                                                           : static_cast<std::int64_t>(max);
        if (tooBig || i < static_cast<std::int64_t>(min) || i > hi)
            fail(key, "must be between " + std::to_string(min) + " and " + std::to_string(max));
        return static_cast<Int>(i);
    }

    static std::string fmt(double d) {
        std::ostringstream os;
        os << d;
        return os.str();
    }

    const json* node_;
    std::string path_;
    const std::string& source_;
    std::set<std::string> used_;
};

AppConfig::Mqtt readMqtt(Section s) {
    AppConfig::Mqtt m;
    m.server = s.requiredString("server");
    m.port = s.integer<std::uint16_t>("port", m.port, 1);
    m.clientId = s.string("clientId", m.clientId);
    m.username = s.string("username", m.username);
    m.password = s.string("password", m.password);
    m.keepAlive = std::chrono::seconds(s.integer<std::int64_t>("keepAliveSec", m.keepAlive.count(), 5, 65535));
    m.qos = static_cast<mqtt::QoS>(s.integer<int>("qos", mqtt::toInt(m.qos), 0, 2));
    m.retain = s.boolean("retain", m.retain);
    m.cleanSession = s.boolean("cleanSession", m.cleanSession);
    m.reconnectMinDelay =
        std::chrono::seconds(s.integer<std::int64_t>("reconnectMinDelaySec", m.reconnectMinDelay.count(), 1, 3600));
    m.reconnectMaxDelay =
        std::chrono::seconds(s.integer<std::int64_t>("reconnectMaxDelaySec", m.reconnectMaxDelay.count(), 1, 3600));
    m.incomingQueueSize = s.integer<std::size_t>("incomingQueueSize", m.incomingQueueSize, 1, 10'000'000);
    if (m.reconnectMaxDelay < m.reconnectMinDelay) s.fail("reconnectMaxDelaySec", "must be >= reconnectMinDelaySec");
    if (m.clientId.empty() && !m.cleanSession) s.fail("clientId", "is required when cleanSession is false");

    Section w = s.section("will");
    if (w.present()) {
        mqtt::LastWill will;
        will.topic = w.requiredString("topic");
        try {
            mqtt::validateTopicName(will.topic);
        } catch (const mqtt::MqttError& e) {
            w.fail("topic", e.what());
        }
        will.payload = w.string("payload", will.payload);
        if (w.has("onlinePayload")) will.onlinePayload = w.string("onlinePayload", "");
        if (w.has("onlinePayload") && will.onlinePayload->empty())
            will.onlinePayload.reset();  // "" disables the online message
        will.qos = static_cast<mqtt::QoS>(w.integer<int>("qos", mqtt::toInt(will.qos), 0, 2));
        will.retain = w.boolean("retain", will.retain);
        w.rejectUnknownKeys();
        m.will = std::move(will);
    }
    s.rejectUnknownKeys();
    return m;
}

AppConfig::Database readDatabase(Section s) {
    AppConfig::Database d;
    auto& c = d.connection;
    c.host = s.string("host", c.host);
    c.port = s.integer<std::uint16_t>("port", c.port, 1);
    c.user = s.requiredString("user");
    c.password = s.string("password", c.password);
    c.database = s.requiredString("database");
    c.connectTimeout = s.millis("connectTimeoutMs", c.connectTimeout, 1);
    c.socketTimeout = s.millis("socketTimeoutMs", c.socketTimeout, 0);
    c.sessionTimeZone = s.string("sessionTimeZone", c.sessionTimeZone);
    c.initStatements = s.stringList("initStatements", c.initStatements);
    d.executor.slowQueryThreshold = s.millis("slowQueryMs", d.executor.slowQueryThreshold);

    Section pool = s.section("pool");
    d.pool.maxSize = pool.integer<std::size_t>("maxSize", d.pool.maxSize, 1, 1000);
    d.pool.acquireTimeout = pool.millis("acquireTimeoutMs", d.pool.acquireTimeout);
    d.pool.validateAfterIdle = pool.millis("validateAfterIdleMs", d.pool.validateAfterIdle);
    pool.rejectUnknownKeys();

    Section tx = s.section("transaction");
    auto& retry = d.transaction.retry;
    retry.maxAttempts = tx.integer<int>("maxAttempts", retry.maxAttempts, 1, 100);
    retry.initialBackoff = tx.millis("initialBackoffMs", retry.initialBackoff);
    retry.backoffMultiplier = tx.number("backoffMultiplier", retry.backoffMultiplier, 1.0, 10.0);
    d.transaction.slowTransactionThreshold = tx.millis("slowTransactionMs", d.transaction.slowTransactionThreshold);
    tx.rejectUnknownKeys();

    s.rejectUnknownKeys();
    return d;
}

std::string readLevel(Section& s, const char* key, const std::string& fallback) {
    std::string level = s.string(key, fallback);
    if (!log::isValidLevel(level))
        s.fail(key, "unknown log level '" + level + "' (trace, debug, info, warn, error, critical, off)");
    return level;
}

AppConfig::Server readServer(Section s) {
    AppConfig::Server srv;
    srv.port = s.requiredInteger<std::uint16_t>("port", 1, 65535);
    srv.bindAddress = s.string("bindAddress", srv.bindAddress);
    srv.ioThreads = s.integer<std::size_t>("ioThreads", srv.ioThreads, 1, 64);
    srv.workerThreads = s.integer<std::size_t>("workerThreads", srv.workerThreads, 1, 1024);
    srv.maxConnections = s.integer<std::size_t>("maxConnections", srv.maxConnections, 1, 1'000'000);
    srv.maxMessageBytes = s.integer<std::size_t>("maxMessageBytes", srv.maxMessageBytes, 16, 256u * 1024 * 1024);
    srv.maxPendingRequests = s.integer<std::size_t>("maxPendingRequests", srv.maxPendingRequests, 1, 10'000);
    srv.idleTimeout = std::chrono::seconds(s.integer<std::int64_t>("idleTimeoutSec", srv.idleTimeout.count(), 1));
    srv.slowRequestThreshold = s.millis("slowRequestMs", srv.slowRequestThreshold);
    srv.shutdownTimeout =
        std::chrono::seconds(s.integer<std::int64_t>("shutdownTimeoutSec", srv.shutdownTimeout.count(), 0, 3600));
    srv.log = readLevel(s, "log", srv.log);
    s.rejectUnknownKeys();
    return srv;
}

AppConfig::Catalog readCatalog(Section s) {
    AppConfig::Catalog c;
    c.timeZone = s.string("timeZone", c.timeZone);
    try {
        TimeZone::named(c.timeZone);
    } catch (const std::invalid_argument& e) {
        s.fail("timeZone", e.what());
    }
    Section r = s.section("reactions");
    c.reactionTopicPrefix = r.string("topicPrefix", c.reactionTopicPrefix);
    try {
        mqtt::validateTopicName(c.reactionTopicPrefix + "/1/like");
    } catch (const mqtt::MqttError& e) {
        r.fail("topicPrefix", e.what());
    }
    c.maxBufferedReactions = r.integer<std::size_t>("maxBuffered", c.maxBufferedReactions, 100, 100'000'000);
    c.reactionKeepDays = r.integer<int>("keepDays", c.reactionKeepDays, 366, 100'000);
    c.topBooksTopic = r.string("topTopic", c.topBooksTopic);
    try {
        mqtt::validateTopicName(c.topBooksTopic);
    } catch (const mqtt::MqttError& e) {
        r.fail("topTopic", e.what());
    }
    r.rejectUnknownKeys();
    s.rejectUnknownKeys();
    return c;
}

std::chrono::milliseconds secondsToMillis(std::int64_t s) { return std::chrono::seconds(s); }

AppConfig::Scheduler readScheduler(Section s, const std::string& timeZone) {
    AppConfig::Scheduler sc;
    sc.threads = s.integer<std::size_t>("threads", sc.threads, 1, 64);
    Section jobs = s.section("jobs");
    for (const std::string& name : jobs.keys()) {
        Section j = jobs.section(name.c_str());
        AppConfig::JobSettings job;
        if (j.has("schedule")) {
            job.schedule = j.requiredString("schedule");
            try {
                scheduler::Schedule::parse(*job.schedule, TimeZone::named(timeZone));
            } catch (const std::invalid_argument& e) {
                j.fail("schedule", e.what());
            }
        }
        if (j.has("enabled")) job.enabled = j.boolean("enabled", true);
        if (j.has("timeoutSec")) job.timeout = secondsToMillis(j.integer<std::int64_t>("timeoutSec", 0, 0, 86400));
        if (j.has("jitterSec")) job.jitter = secondsToMillis(j.integer<std::int64_t>("jitterSec", 0, 0, 3600));
        if (j.has("retryAttempts")) job.retryAttempts = j.integer<int>("retryAttempts", 0, 0, 100);
        if (j.has("retryDelaySec"))
            job.retryDelay = secondsToMillis(j.integer<std::int64_t>("retryDelaySec", 10, 1, 86400));
        j.rejectUnknownKeys();
        sc.jobs.emplace(name, job);
    }
    s.rejectUnknownKeys();
    return sc;
}

AppConfig::Health readHealth(Section s) {
    AppConfig::Health h;
    h.slowDatabase = s.millis("slowDatabaseMs", h.slowDatabase, 1);
    h.maxMemoryMb = s.integer<std::size_t>("maxMemoryMb", h.maxMemoryMb, 0, 1'000'000);
    h.reactionBufferWarnPercent = s.integer<int>("reactionBufferWarnPercent", h.reactionBufferWarnPercent, 1, 100);
    s.rejectUnknownKeys();
    return h;
}

log::LogConfig readLog(Section s) {
    log::LogConfig l;
    l.level = readLevel(s, "level", l.level);
    l.levels = s.stringMap("levels");
    for (const auto& [name, level] : l.levels)
        if (!log::isValidLevel(level)) s.fail("levels." + name, "unknown log level '" + level + "'");
    l.console = s.boolean("console", l.console);
    l.filePath = s.string("file", l.filePath);
    l.maxFileSizeBytes =
        s.integer<std::size_t>("maxFileSizeMb", l.maxFileSizeBytes / (1024 * 1024), 1, 10240) * 1024 * 1024;
    l.maxFiles = s.integer<std::size_t>("maxFiles", l.maxFiles, 1, 1000);
    l.pattern = s.string("pattern", l.pattern);
    s.rejectUnknownKeys();
    return l;
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ConfigError(file.string() + ": cannot open file");
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

AppConfig AppConfig::fromJson(std::string_view text, const std::string& source) {
    json root;
    try {
        root = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const json::parse_error& e) {
        throw ConfigError(source + ": invalid JSON: " + e.what());
    }

    Section top(&root, "", source);
    AppConfig config;
    config.mqtt = readMqtt(top.section("mqtt"));
    config.db = readDatabase(top.section("db"));
    config.server = readServer(top.section("server"));
    config.catalog = readCatalog(top.section("catalog"));
    config.scheduler = readScheduler(top.section("scheduler"), config.catalog.timeZone);
    config.health = readHealth(top.section("health"));
    config.log = readLog(top.section("log"));
    top.rejectUnknownKeys();

    // server.log is shorthand for log.levels.server.
    config.log.levels.emplace("server", config.server.log);
    return config;
}

AppConfig AppConfig::load(const std::filesystem::path& file) { return fromJson(readFile(file), file.string()); }

std::filesystem::path AppConfig::locate(int argc, const char* const* argv) {
    namespace fs = std::filesystem;

    // Explicit choices must exist; no silent fallback to another file.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--config") {
            if (i + 1 >= argc) throw ConfigError("--config requires a path");
            value = argv[i + 1];
        } else if (arg.rfind("--config=", 0) == 0) {
            value = arg.substr(9);
        } else {
            continue;
        }
        if (!fs::is_regular_file(value)) throw ConfigError("--config: no such file: " + value);
        return value;
    }
    if (const char* env = std::getenv("CAELITUS_CONFIG")) {
        if (!fs::is_regular_file(env)) throw ConfigError("CAELITUS_CONFIG: no such file: " + std::string(env));
        return env;
    }

    const fs::path relative = fs::path("config") / "config.json";
    std::vector<fs::path> candidates{fs::current_path() / relative};
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        // The executable's directory and each one above it, so a build in
        // build/<preset>/src/ or cmake-build-*/ finds the project's config.
        for (fs::path dir = exe.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            candidates.push_back(dir / relative);
            if (dir == dir.root_path()) break;
        }
    }

    std::string tried;
    for (const auto& c : candidates) {
        if (fs::is_regular_file(c)) return c.lexically_normal();
        tried += "\n  " + c.lexically_normal().string();
    }
    throw ConfigError("config.json not found; looked in:" + tried + "\n(use --config <path> or CAELITUS_CONFIG)");
}

}  // namespace caelitus
