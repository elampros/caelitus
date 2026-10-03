/// @file
/// cli::buildParams(): command-line words to JSON-RPC params, by schema.
/// @ingroup cli

#include "caelitus/cli/Cli.hpp"

#include "cli/Detail.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace caelitus::cli {

namespace {

std::string typeOf(const Json& schema) { return schema.is_object() ? schema.value("type", "") : ""; }

const Json* findParam(const Json& method, const std::string& name) {
    for (const auto& p : detail::arrayAt(method, "params"))
        if (p.value("name", "") == name) return &p;
    return nullptr;
}

[[noreturn]] void unknownParam(const Json& method, const std::string& name) {
    const std::string methodName = method.value("name", "");
    std::string best;
    std::size_t bestDistance = 3;  // suggest only close matches
    std::string all;
    for (const auto& p : detail::arrayAt(method, "params")) {
        const std::string candidate = p.value("name", "");
        all += (all.empty() ? "" : ", ") + ("--" + candidate);
        if (const auto d = detail::editDistance(name, candidate); d < bestDistance) bestDistance = d, best = candidate;
    }
    if (!best.empty()) throw UsageError(methodName + " has no parameter --" + name + "; did you mean --" + best + "?");
    if (all.empty()) throw UsageError(methodName + " takes no parameters (got --" + name + ")");
    throw UsageError(methodName + " has no parameter --" + name + "; its parameters are " + all);
}

Json parseJsonValue(const std::string& name, const std::string& text) {
    try {
        return Json::parse(text);
    } catch (const Json::parse_error&) {
        throw UsageError("--" + name + ": not valid JSON: " + text);
    }
}

// One value converted to `schema`'s type; `text` as typed on the command line.
Json convert(const std::string& name, const Json& schema, const std::string& text) {
    const std::string type = typeOf(schema);
    if (type == "string") return text;
    if (type == "integer") {
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || *end != '\0' || errno == ERANGE)
            throw UsageError("--" + name + ": expected an integer, got '" + text + "'");
        return v;
    }
    if (type == "number") {
        errno = 0;
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (text.empty() || *end != '\0' || errno == ERANGE)
            throw UsageError("--" + name + ": expected a number, got '" + text + "'");
        return v;
    }
    if (type == "boolean") {
        std::string t = text;
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
        if (t == "true" || t == "yes" || t == "on" || t == "1") return true;
        if (t == "false" || t == "no" || t == "off" || t == "0") return false;
        throw UsageError("--" + name + ": expected true or false, got '" + text + "'");
    }
    if (type == "array") {
        if (!text.empty() && text.front() == '[') return parseJsonValue(name, text);
        Json items = Json::array();
        const Json itemSchema = schema.value("items", Json::object());
        std::size_t start = 0;
        for (;;) {
            const auto comma = text.find(',', start);
            const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!item.empty()) items.push_back(convert(name, itemSchema, item));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return items;
    }
    return parseJsonValue(name, text);  // objects, or a schema without a type
}

bool isFlag(const std::string& word) { return word.size() > 2 && word.compare(0, 2, "--") == 0; }

bool isBooleanWord(const std::string& word) {
    try {
        convert("", Json{{"type", "boolean"}}, word);
        return true;
    } catch (const UsageError&) {
        return false;
    }
}

void set(Json& params, const std::string& name, const Json& schema, Json value) {
    // A repeated array parameter (--tags a --tags b) accumulates.
    if (typeOf(schema) == "array" && params.contains(name) && params[name].is_array() && value.is_array()) {
        for (auto& v : value) params[name].push_back(std::move(v));
        return;
    }
    params[name] = std::move(value);
}

}  // namespace

Json buildParams(const Json& method, const std::vector<std::string>& words) {
    const std::string methodName = method.value("name", "");
    Json params = Json::object();
    std::vector<std::string> positional;

    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::string& word = words[i];
        if (!isFlag(word)) {
            positional.push_back(word);
            continue;
        }
        const auto eq = word.find('=');
        const std::string name = word.substr(2, eq == std::string::npos ? std::string::npos : eq - 2);
        const Json* param = findParam(method, name);
        if (!param) unknownParam(method, name);
        const Json& schema = param->at("schema");

        std::string text;
        if (eq != std::string::npos) {
            text = word.substr(eq + 1);
        } else if (typeOf(schema) == "boolean") {
            // --flag alone means true; "--flag false" is accepted too.
            text = i + 1 < words.size() && isBooleanWord(words[i + 1]) ? words[++i] : "true";
        } else if (i + 1 < words.size()) {
            text = words[++i];
        } else {
            throw UsageError("--" + name + " needs a value");
        }
        set(params, name, schema, convert(name, schema, text));
    }

    // Positional words: a JSON object of parameters, or values for the
    // required parameters not given by name, in declaration order.
    for (const auto& word : positional) {
        if (!word.empty() && word.front() == '{') {
            const Json object = parseJsonValue("params", word);
            if (!object.is_object()) throw UsageError("Parameters in JSON must be an object: " + word);
            for (const auto& [name, value] : object.items()) {
                if (!findParam(method, name)) unknownParam(method, name);
                if (!params.contains(name)) params[name] = value;  // named words win
            }
            continue;
        }
        const Json* target = nullptr;
        for (const auto& p : detail::arrayAt(method, "params"))
            if (p.value("required", false) && !params.contains(p.value("name", ""))) {
                target = &p;
                break;
            }
        if (!target)
            throw UsageError("Unexpected argument '" + word + "'; give parameters as --name=value (see: help " +
                             methodName + ")");
        const std::string name = target->value("name", "");
        params[name] = convert(name, target->at("schema"), word);
    }

    for (const auto& p : detail::arrayAt(method, "params")) {
        const std::string name = p.value("name", "");
        if (p.value("required", false) && !params.contains(name))
            throw UsageError(methodName + ": missing --" + name + " (" + p.value("description", "required") + ")");
    }
    return params;
}

}  // namespace caelitus::cli
