/// @file
/// Logger set-up (console, file, extra sinks), named loggers and levels.
/// @ingroup log

#include "caelitus/log/Log.hpp"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>
#include <stdexcept>

namespace caelitus::log {

namespace {

struct State {
    std::mutex mutex;
    LogConfig config;
    std::vector<spdlog::sink_ptr> sinks;  // empty until first use / init()
    std::map<std::string, Logger> loggers;
};

State& state() {
    static State s;
    return s;
}

spdlog::level::level_enum parseLevel(const std::string& text) {
    auto level = spdlog::level::from_str(text);
    // from_str() maps unknown names to "off"; treat that as a config error.
    if (level == spdlog::level::off && text != "off") throw std::invalid_argument("Unknown log level '" + text + "'");
    return level;
}

// Most specific configured level: "db.sql" -> "db.sql", then "db", then default.
spdlog::level::level_enum levelFor(const LogConfig& config, std::string name) {
    for (;;) {
        auto it = config.levels.find(name);
        if (it != config.levels.end()) return parseLevel(it->second);
        auto dot = name.rfind('.');
        if (dot == std::string::npos) return parseLevel(config.level);
        name.erase(dot);
    }
}

void applyConfig(spdlog::logger& logger, const State& s) {
    logger.sinks() = s.sinks;
    logger.set_level(levelFor(s.config, logger.name()));
    logger.flush_on(spdlog::level::warn);
}

std::vector<spdlog::sink_ptr> buildSinks(const LogConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (!config.filePath.empty())
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(config.filePath, config.maxFileSizeBytes,
                                                                               config.maxFiles));
    sinks.insert(sinks.end(), config.extraSinks.begin(), config.extraSinks.end());
    for (auto& sink : sinks) sink->set_pattern(config.pattern);
    return sinks;
}

}  // namespace

bool isValidLevel(const std::string& level) noexcept {
    try {
        parseLevel(level);
        return true;
    } catch (...) {
        return false;
    }
}

void init(const LogConfig& config) {
    // Validate everything before touching the live state.
    parseLevel(config.level);
    for (const auto& [name, level] : config.levels) parseLevel(level);
    auto sinks = buildSinks(config);

    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.config = config;
    s.sinks = std::move(sinks);
    for (auto& [name, logger] : s.loggers) applyConfig(*logger, s);
}

Logger get(const std::string& name) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    auto it = s.loggers.find(name);
    if (it != s.loggers.end()) return it->second;

    if (s.sinks.empty()) s.sinks = buildSinks(s.config);  // used before init(): defaults
    auto logger = std::make_shared<spdlog::logger>(name);
    applyConfig(*logger, s);
    s.loggers.emplace(name, logger);
    return logger;
}

void shutdown() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (auto& [name, logger] : s.loggers) logger->flush();
    s.loggers.clear();
}

}  // namespace caelitus::log
