#include "caelitus/api/SchemaValidator.hpp"

#include <set>

namespace caelitus::api {

namespace {

std::size_t charCount(const std::string& s) {
    std::size_t n = 0;
    for (char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

bool hasType(const Json& value, const std::string& type) {
    if (type == "string") return value.is_string();
    if (type == "integer") return value.is_number_integer();  // 3.0 is a number, not an integer
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    if (type == "null") return value.is_null();
    return false;
}

std::string article(const std::string& type) {
    return (type == "array" || type == "integer" || type == "object") ? "an " + type : "a " + type;
}

std::string at(const std::string& path, const std::string& problem) { return path + ": " + problem; }

}  // namespace

std::optional<std::string> SchemaValidator::check(const Json& value, const Json& schema,
                                                  const std::string& path) const {
    if (auto r = schema.find("$ref"); r != schema.end()) {
        const std::string target = r->get<std::string>();
        const std::string prefix = "#/components/schemas/";
        auto it = target.rfind(prefix, 0) == 0 ? components_.find(target.substr(prefix.size())) : components_.end();
        if (it == components_.end()) return at(path, "unresolvable schema reference " + target);
        return check(value, it->second, path);
    }

    if (auto o = schema.find("oneOf"); o != schema.end()) {
        int matches = 0;
        std::string firstProblem;
        for (const auto& alternative : *o) {
            auto problem = check(value, alternative, path);
            if (!problem) ++matches;
            else if (firstProblem.empty()) firstProblem = *problem;
        }
        if (matches != 1) return matches == 0 ? firstProblem : at(path, "matches more than one allowed form");
    }

    if (auto t = schema.find("type"); t != schema.end()) {
        bool ok = false;
        std::string expected;
        if (t->is_array()) {
            for (const auto& each : *t) {
                ok = ok || hasType(value, each.get<std::string>());
                expected += (expected.empty() ? "" : " or ") + article(each.get<std::string>());
            }
        } else {
            ok = hasType(value, t->get<std::string>());
            expected = article(t->get<std::string>());
        }
        if (!ok) return at(path, "must be " + expected);
    }

    if (auto e = schema.find("enum"); e != schema.end()) {
        if (std::find(e->begin(), e->end(), value) == e->end()) {
            std::string allowed;
            for (const auto& v : *e)
                allowed += (allowed.empty() ? "" : ", ") + (v.is_string() ? v.get<std::string>() : v.dump());
            return at(path, "must be one of: " + allowed);
        }
    }
    if (auto c = schema.find("const"); c != schema.end() && value != *c) return at(path, "must be " + c->dump());

    if (value.is_number()) {
        const double v = value.get<double>();
        if (auto m = schema.find("minimum"); m != schema.end() && v < m->get<double>())
            return at(path, "must be at least " + m->dump());
        if (auto m = schema.find("maximum"); m != schema.end() && v > m->get<double>())
            return at(path, "must be at most " + m->dump());
    }

    if (value.is_string()) {
        const auto& s = value.get_ref<const std::string&>();
        if (auto m = schema.find("minLength"); m != schema.end() && charCount(s) < m->get<std::size_t>())
            return at(path, m->get<std::size_t>() == 1 ? "must not be empty"
                                                       : "must be at least " + m->dump() + " characters");
        if (auto m = schema.find("maxLength"); m != schema.end() && charCount(s) > m->get<std::size_t>())
            return at(path, "must be at most " + m->dump() + " characters");
        if (auto f = schema.find("format"); f != schema.end()) {
            try {
                if (*f == "date") Date::parse(s);
                if (*f == "date-time") parseIsoTimestamp(s);
            } catch (const std::invalid_argument&) {
                return at(path, *f == "date" ? "must be a date \"YYYY-MM-DD\"" : "must be an RFC 3339 timestamp");
            }
        }
    }

    if (value.is_array()) {
        if (auto m = schema.find("minItems"); m != schema.end() && value.size() < m->get<std::size_t>())
            return at(path, "must have at least " + m->dump() + " items");
        if (auto m = schema.find("maxItems"); m != schema.end() && value.size() > m->get<std::size_t>())
            return at(path, "must have at most " + m->dump() + " items");
        if (schema.value("uniqueItems", false)) {
            std::set<std::string> seen;
            for (const auto& item : value)
                if (!seen.insert(item.dump()).second) return at(path, "must not contain duplicates");
        }
        if (auto items = schema.find("items"); items != schema.end())
            for (std::size_t i = 0; i < value.size(); ++i)
                if (auto problem = check(value[i], *items, path + "[" + std::to_string(i) + "]")) return problem;
    }

    if (value.is_object()) {
        if (auto req = schema.find("required"); req != schema.end())
            for (const auto& name : *req)
                if (!value.contains(name.get<std::string>()))
                    return at(path.empty() ? name.get<std::string>() : path + "." + name.get<std::string>(),
                              "is required");
        const auto props = schema.find("properties");
        for (const auto& [key, item] : value.items()) {
            const std::string itemPath = path.empty() ? key : path + "." + key;
            if (props != schema.end() && props->contains(key)) {
                if (auto problem = check(item, (*props)[key], itemPath)) return problem;
            } else if (schema.contains("additionalProperties") && schema["additionalProperties"] == false) {
                return at(itemPath, "is not allowed");
            }
        }
    }
    return std::nullopt;
}

}  // namespace caelitus::api
