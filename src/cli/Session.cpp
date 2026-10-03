/// @file
/// cli::Session, cli::splitWords(), cli::runLines().
/// @ingroup cli

#include "caelitus/cli/Session.hpp"

#include "caelitus/net/TcpServer.hpp"
#include "cli/Detail.hpp"

#include <algorithm>
#include <ostream>
#include <sstream>

namespace caelitus::cli {

namespace {

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool isFlag(const std::string& word) { return word.size() > 2 && startsWith(word, "--"); }

// "--name=value" or "--name" -> "name"
std::string flagName(const std::string& word) { return word.substr(2, word.find('=') - 2); }

const Json* findParam(const Json& method, const std::string& name) {
    for (const auto& p : detail::arrayAt(method, "params"))
        if (p.value("name", "") == name) return &p;
    return nullptr;
}

bool isBoolean(const Json& param) { return param.at("schema").value("type", "") == "boolean"; }

// The values a parameter can take, when there is a short list of them.
std::vector<std::string> knownValues(const Json& param) {
    const Json& schema = param.at("schema");
    std::vector<std::string> values;
    if (schema.contains("enum"))
        for (const auto& v : schema["enum"]) values.push_back(v.is_string() ? v.get<std::string>() : v.dump());
    else if (schema.value("type", "") == "boolean") values = {"true", "false"};
    return values;
}

// The input up to the last blank outside quotes, and the word after it.
// Returns false inside an open quote (nothing sensible to complete).
bool splitLast(const std::string& input, std::string& before, std::string& context) {
    char quote = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"') ++i;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '\\') {
            ++i;
        } else if (c == ' ' || c == '\t') {
            start = i + 1;
        }
    }
    if (quote) return false;
    before = input.substr(0, start);
    context = input.substr(start);
    return true;
}

void printError(std::ostream& err, const Json& error, bool json) {
    if (json) return void(err << error.dump() << "\n");
    err << "Error " << error.value("code", 0) << ": " << error.value("message", "") << "\n";
    const Json data = error.value("data", Json());
    if (data.is_object() && data.contains("reason") && data["reason"].is_string()) {
        // The server's reason already names the field ("pageSize: must be at most 100").
        std::string reason = data["reason"].get<std::string>();
        const std::string field = data.value("field", "");
        if (!field.empty() && reason.rfind(field + ": ", 0) == 0) reason.erase(0, field.size() + 2);
        err << "  " << (field.empty() ? "" : "--" + field + ": ") << reason << "\n";
        return;
    }
    if (!data.is_null()) err << "  details: " << data.dump() << "\n";
}

const char* const kBuiltins[] = {"help", "exit", "quit"};

}  // namespace

std::vector<std::string> splitWords(const std::string& line) {
    std::vector<std::string> words;
    std::string word;
    bool inWord = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote == '\'') {
            if (c == '\'') quote = 0;
            else word += c;
        } else if (quote == '"') {
            if (c == '"') quote = 0;
            else if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) word += line[++i];
            else word += c;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (inWord) words.push_back(std::move(word)), word.clear(), inWord = false;
        } else {
            inWord = true;
            if (c == '\'' || c == '"') quote = c;
            else if (c == '\\' && i + 1 < line.size()) word += line[++i];
            else word += c;
        }
    }
    if (quote) throw UsageError(std::string("Unterminated ") + (quote == '"' ? "\"" : "'") + " quote");
    if (inWord) words.push_back(std::move(word));
    return words;
}

// ---- Session -----------------------------------------------------------------

Session::Session(std::string host, std::uint16_t port, std::chrono::milliseconds timeout, Connector connect)
    : host_(std::move(host)),
      port_(port),
      timeout_(timeout),
      connector_(std::move(connect)),
      endpoint_(host_ + ":" + std::to_string(port_)) {
    this->connect();
}

void Session::connect() {
    connection_.reset();
    auto connection = connector_(host_, port_, timeout_);
    connection_ = std::move(connection);
    const Json reply = call("rpc.discover", Json::object());
    if (!reply.contains("result") || !reply["result"].is_object())
        throw net::NetError("rpc.discover failed; is this a caelitus server?");
    document_ = reply["result"];
}

Json Session::call(const std::string& method, const Json& params) {
    Json request = {{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", method}};
    if (!params.empty()) request["params"] = params;
    std::string text;
    try {
        text = connection_->request(request.dump());
    } catch (const net::NetError&) {
        connection_.reset();  // its state is unknown; the next command connects again
        throw;
    }
    Json reply = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!reply.is_object() || (!reply.contains("result") && !reply.contains("error")))
        throw net::NetError("The reply is not JSON-RPC: " + text.substr(0, 200));
    return reply;
}

const Json* Session::findMethod(const std::string& name) const {
    for (const auto& m : detail::arrayAt(document_, "methods"))
        if (m.value("name", "") == name) return &m;
    return nullptr;
}

int Session::execute(std::vector<std::string> words, std::ostream& out, std::ostream& err, bool json, bool color) {
    if (words.empty()) return kOk;
    // Output switches for this command only.
    const auto removed = std::remove_if(words.begin() + 1, words.end(), [&](const std::string& w) {
        if (w == "--json") json = true;
        else if (w == "--no-color") color = false;
        else return false;
        return true;
    });
    words.erase(removed, words.end());
    color = color && !json;

    if (!connection_ || !connection_->isOpen()) connect();

    const auto unknownMethod = [&](const std::string& name) {
        std::string best;
        std::size_t bestDistance = 4;
        for (const auto& m : detail::arrayAt(document_, "methods")) {
            const std::string candidate = m.value("name", "");
            if (const auto d = detail::editDistance(name, candidate); d < bestDistance)
                bestDistance = d, best = candidate;
        }
        return UsageError("Unknown method '" + name + "'" + (best.empty() ? "" : "; did you mean " + best + "?") +
                          " (help lists them)");
    };

    if (words[0] == "help") {
        if (words.size() == 1) return out << methodList(document_), kOk;
        const Json* method = findMethod(words[1]);
        if (!method) throw unknownMethod(words[1]);
        return out << methodHelp(*method), kOk;
    }

    const Json* method = findMethod(words[0]);
    if (!method) throw unknownMethod(words[0]);
    const std::vector<std::string> args(words.begin() + 1, words.end());
    for (const auto& a : args)
        if (a == "--help" || a == "-h") return out << methodHelp(*method), kOk;

    const Json reply = call(words[0], buildParams(*method, args));
    if (reply.contains("error")) {
        printError(err, reply["error"], json);
        return kCallFailed;
    }
    out << (json ? reply["result"].dump() : formatJson(reply["result"], color)) << "\n";
    return kOk;
}

Session::Suggestions Session::complete(const std::string& input) const {
    Suggestions s;
    std::string before;
    if (!splitLast(input, before, s.context)) return {};
    std::vector<std::string> words;
    try {
        words = splitWords(before);
    } catch (const UsageError&) {
        return {};
    }
    const auto add = [&](const std::string& candidate) {
        if (startsWith(candidate, s.context)) s.items.push_back(candidate);
    };

    if (words.empty()) {
        for (const char* b : kBuiltins) add(b);
        for (const auto& m : detail::arrayAt(document_, "methods")) add(m.value("name", ""));
        return s;
    }
    if (words[0] == "help") {
        if (words.size() == 1)
            for (const auto& m : detail::arrayAt(document_, "methods")) add(m.value("name", ""));
        return s;
    }
    const Json* method = findMethod(words[0]);
    if (!method) return {};

    // A value: "--sort=pub" or "--sort pub".
    if (isFlag(s.context) && s.context.find('=') != std::string::npos) {
        const std::string name = flagName(s.context);
        if (const Json* p = findParam(*method, name))
            for (const auto& v : knownValues(*p)) add("--" + name + "=" + v);
        return s;
    }
    if (words.size() > 1 && isFlag(words.back()) && words.back().find('=') == std::string::npos &&
        !startsWith(s.context, "-")) {
        const Json* p = findParam(*method, flagName(words.back()));
        if (p && !isBoolean(*p)) {
            for (const auto& v : knownValues(*p)) add(v);
            return s;  // free text otherwise: nothing to suggest
        }
    }

    // Parameter names not given yet.
    std::vector<std::string> used;
    for (std::size_t i = 1; i < words.size(); ++i)
        if (isFlag(words[i])) used.push_back(flagName(words[i]));
    for (const auto& p : detail::arrayAt(*method, "params")) {
        const std::string name = p.value("name", "");
        if (std::find(used.begin(), used.end(), name) != used.end()) continue;
        add(isBoolean(p) ? "--" + name : "--" + name + "=");
    }
    if (startsWith(s.context, "--")) add("--json"), add("--no-color"), add("--help");
    return s;
}

Session::Suggestions Session::hint(const std::string& input) const {
    std::string before, context;
    if (!splitLast(input, before, context)) return {};
    std::vector<std::string> words;
    try {
        words = splitWords(before);
    } catch (const UsageError&) {
        return {};
    }
    const Json* method = words.empty() || words[0] == "help" ? nullptr : findMethod(words[0]);

    if (!context.empty()) {
        // A parameter whose value is being typed: its type and description.
        if (method && isFlag(context) && context.find('=') != std::string::npos) {
            const Json* p = findParam(*method, flagName(context));
            if (p && context.back() == '=' && knownValues(*p).empty())
                return {{detail::placeholder(p->at("schema")) + "  " + p->value("description", "")}, ""};
        }
        const Suggestions c = complete(input);
        if (c.items.size() != 1 || c.items[0] == context) return {};
        const std::string& item = c.items[0];
        std::string text = item;
        if (words.empty() || (words.size() == 1 && words[0] == "help")) {
            if (const Json* m = findMethod(item)) text += "  " + m->value("summary", "");
        } else if (method && isFlag(item)) {
            if (const Json* p = findParam(*method, flagName(item))) {
                if (item.back() == '=') text += detail::placeholder(p->at("schema"));
                text += "  " + p->value("description", "");
            }
        }
        return {{text}, context};
    }

    // After "<method> ": the required parameters still missing.
    if (!method) return {};
    std::vector<std::string> named;
    std::size_t positional = 0;
    for (std::size_t i = 1; i < words.size(); ++i) {
        if (isFlag(words[i])) {
            named.push_back(flagName(words[i]));
            const Json* p = findParam(*method, named.back());
            if (p && !isBoolean(*p) && words[i].find('=') == std::string::npos) ++i;  // its value
        } else if (!words[i].empty() && words[i][0] != '{') {
            ++positional;
        }
    }
    std::string text;
    for (const auto& p : detail::arrayAt(*method, "params")) {
        const std::string name = p.value("name", "");
        if (!p.value("required", false) || std::find(named.begin(), named.end(), name) != named.end()) continue;
        if (positional > 0) {
            --positional;  // given by position
            continue;
        }
        text += (text.empty() ? "" : " ") + ("--" + name + " " + detail::placeholder(p.at("schema")));
    }
    if (text.empty()) return {};
    return {{text}, ""};
}

// ---- runLines ------------------------------------------------------------------

int runLines(Session& session, LineReader& reader, std::ostream& out, std::ostream& err, const Options& options,
             bool interactive) {
    int result = kOk;
    std::size_t lineNumber = 0;
    while (const auto line = reader.read()) {
        ++lineNumber;
        std::ostringstream problems;
        int code = kOk;
        try {
            const std::vector<std::string> words = splitWords(*line);
            if (words.empty() || words[0][0] == '#') continue;
            if (words[0] == "exit" || words[0] == "quit") {
                reader.ran(*line);
                break;
            }
            code = session.execute(words, out, problems, options.json, options.color);
        } catch (const UsageError& e) {
            problems << e.what() << "\n";
            code = kBadUsage;
        } catch (const net::NetError& e) {
            problems << "Server " << session.endpoint() << ": " << e.what() << "\n";
            code = kUnreachable;
        }
        reader.ran(*line);
        out.flush();
        // In a script, say which line went wrong.
        std::istringstream text(problems.str());
        for (std::string l; std::getline(text, l);)
            err << (interactive ? "" : "line " + std::to_string(lineNumber) + ": ") << l << "\n";
        if (code != kOk) result = code;
    }
    return interactive ? kOk : result;
}

}  // namespace caelitus::cli
