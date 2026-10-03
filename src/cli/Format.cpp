/// @file
/// cli::methodHelp(), cli::methodList(), cli::formatJson().
/// @ingroup cli

#include "caelitus/cli/Cli.hpp"

#include "cli/Detail.hpp"

#include <algorithm>
#include <map>
#include <sstream>

namespace caelitus::cli {

namespace {

// Last segment of a "$ref": "#/components/errors/NotFound" -> "NotFound".
std::string refName(const Json& value) {
    const std::string ref = value.is_object() ? value.value("$ref", "") : "";
    const auto slash = ref.rfind('/');
    return slash == std::string::npos ? ref : ref.substr(slash + 1);
}

std::string number(const Json& v) {
    if (v.is_number_float() && v.get<double>() == static_cast<double>(static_cast<long long>(v.get<double>())))
        return std::to_string(static_cast<long long>(v.get<double>()));
    return v.dump();
}

// "1-100", ">= 1", "up to 300 characters": the limits worth knowing.
std::string limits(const Json& s) {
    const bool hasMin = s.contains("minimum"), hasMax = s.contains("maximum");
    if (hasMin && hasMax) return number(s["minimum"]) + "-" + number(s["maximum"]);
    if (hasMin) return ">= " + number(s["minimum"]);
    if (hasMax) return "<= " + number(s["maximum"]);
    if (s.contains("maxLength") && s["maxLength"].get<int>() > 2)
        return "up to " + s["maxLength"].dump() + " characters";
    if (s.contains("maxItems")) return "up to " + s["maxItems"].dump() + " items";
    return "";
}

// Wider parameter columns put their description on a line of its own.
constexpr std::size_t kMaxLeft = 32;

std::string padded(const std::string& text, std::size_t width) {
    return text.size() >= width ? text + "  " : text + std::string(width - text.size(), ' ');
}

const char* const kReset = "\033[0m";
const char* const kKey = "\033[36m";      // cyan
const char* const kString = "\033[32m";   // green
const char* const kNumber = "\033[33m";   // yellow
const char* const kLiteral = "\033[35m";  // magenta

void writeJson(std::ostringstream& out, const Json& v, bool color, int indent) {
    const auto paint = [&](const char* code, const std::string& text) {
        if (color) out << code << text << kReset;
        else out << text;
    };
    const std::string pad(static_cast<std::size_t>(indent + 2), ' ');
    const std::string closePad(static_cast<std::size_t>(indent), ' ');
    if (v.is_object()) {
        if (v.empty()) return void(out << "{}");
        out << "{\n";
        std::size_t i = 0;
        for (const auto& [key, value] : v.items()) {
            out << pad;
            paint(kKey, Json(key).dump());
            out << ": ";
            writeJson(out, value, color, indent + 2);
            out << (++i < v.size() ? ",\n" : "\n");
        }
        out << closePad << "}";
    } else if (v.is_array()) {
        if (v.empty()) return void(out << "[]");
        out << "[\n";
        for (std::size_t i = 0; i < v.size(); ++i) {
            out << pad;
            writeJson(out, v[i], color, indent + 2);
            out << (i + 1 < v.size() ? ",\n" : "\n");
        }
        out << closePad << "]";
    } else if (v.is_string()) {
        paint(kString, v.dump());
    } else if (v.is_number()) {
        paint(kNumber, v.dump());
    } else {
        paint(kLiteral, v.dump());  // true, false, null
    }
}

}  // namespace

namespace detail {

// "<integer>", "<date>", "any|all", "<string,...>": how a value is typed.
std::string placeholder(const Json& schema) {
    if (schema.contains("enum")) {
        std::string out;
        for (const auto& v : schema["enum"])
            out += (out.empty() ? "" : "|") + (v.is_string() ? v.get<std::string>() : v.dump());
        return out;
    }
    const std::string type = schema.value("type", "");
    if (type == "array") {
        std::string item = detail::placeholder(schema.value("items", Json::object()));
        if (item.size() > 2 && item.front() == '<') item = item.substr(1, item.size() - 2);
        return "<" + item + ",...>";
    }
    if (type == "boolean") return "";  // a bare --flag
    if (schema.contains("format")) return "<" + schema["format"].get<std::string>() + ">";
    return "<" + (type.empty() ? std::string("json") : type) + ">";
}

}  // namespace detail

std::string formatJson(const Json& value, bool color) {
    std::ostringstream out;
    writeJson(out, value, color, 0);
    return out.str();
}

std::string methodHelp(const Json& method) {
    std::ostringstream out;
    const std::string name = method.value("name", "");
    out << name << ": " << method.value("summary", "") << "\n";
    if (method.contains("description")) out << "\n" << method["description"].get<std::string>() << "\n";

    const Json& params = detail::arrayAt(method, "params");
    out << "\nUsage: caelitus --cli " << name;
    for (const auto& p : params)
        if (p.value("required", false))
            out << " --" << p.value("name", "") << " " << detail::placeholder(p.at("schema"));
    if (std::any_of(params.begin(), params.end(), [](const Json& p) { return !p.value("required", false); }))
        out << " [options]";
    out << "\n";

    if (!params.empty()) {
        std::vector<std::string> left;
        std::size_t width = 0;
        for (const auto& p : params) {
            std::string l = "--" + p.value("name", "");
            if (const auto ph = detail::placeholder(p.at("schema")); !ph.empty()) l += " " + ph;
            if (l.size() <= kMaxLeft) width = std::max(width, l.size() + 2);
            left.push_back(std::move(l));
        }
        out << "\nParameters:\n";
        for (std::size_t i = 0; i < params.size(); ++i) {
            const Json& p = params[i];
            std::string right = p.value("description", "");
            std::vector<std::string> notes;
            if (p.value("required", false)) notes.push_back("required");
            if (const auto l = limits(p.at("schema")); !l.empty()) notes.push_back(l);
            if (!notes.empty()) {
                right += "  [";
                for (std::size_t n = 0; n < notes.size(); ++n) right += (n ? ", " : "") + notes[n];
                right += "]";
            }
            // A long left column (a big enum) gets the description on the next line.
            if (left[i].size() > kMaxLeft) out << "  " << left[i] << "\n  " << std::string(width, ' ') << right << "\n";
            else out << "  " << padded(left[i], width) << right << "\n";
        }
    }

    if (method.contains("result")) {
        const Json& result = method["result"];
        const Json& schema = result.value("schema", Json::object());
        std::string type = refName(schema);
        if (type.empty()) type = schema.value("type", "");
        if (type == "array" && schema.contains("items")) type = refName(schema["items"]) + "[]";
        out << "\nReturns: " << result.value("name", "result") << (type.empty() ? "" : " (" + type + ")") << "\n";
    }
    if (method.contains("errors") && !method["errors"].empty()) {
        out << "Errors:  ";
        bool first = true;
        for (const auto& e : method["errors"]) out << (first ? "" : ", ") << refName(e), first = false;
        out << "\n";
    }
    return out.str();
}

std::string methodList(const Json& document) {
    std::map<std::string, std::vector<const Json*>> byTag;
    std::size_t width = 0;
    for (const auto& m : detail::arrayAt(document, "methods")) {
        const auto& tags = detail::arrayAt(m, "tags");
        byTag[tags.empty() ? "other" : refName(tags[0])].push_back(&m);
        width = std::max(width, m.value("name", "").size() + 2);
    }
    std::ostringstream out;
    for (const auto& [tag, methods] : byTag) {
        out << tag << "\n";
        for (const Json* m : methods)
            out << "  " << padded(m->value("name", ""), width) << m->value("summary", "") << "\n";
    }
    return out.str();
}

}  // namespace caelitus::cli
