/// @file
/// cli::run(): the `caelitus --cli` command flow.
/// @ingroup cli

#include "caelitus/cli/Cli.hpp"

#include "caelitus/config/AppConfig.hpp"
#include "caelitus/net/TcpClient.hpp"
#include "cli/Detail.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace caelitus::cli {

const char* const kUsage = R"(Usage: caelitus --cli [options] [<method> [parameters...]]
       caelitus --cli [options] help [<method>]

Calls a method of a running caelitus server and prints the result. The list of
methods and their parameters is read from the server itself (rpc.discover), so
every method the server has can be called.

  caelitus --cli                                  list the methods
  caelitus --cli help books.search                a method's parameters
  caelitus --cli system.health
  caelitus --cli books.get 42                     a required parameter by position
  caelitus --cli books.search --title=dune --pageSize 5 --tags=sci-fi,classic
  caelitus --cli scheduler.pause --name=top-books
  caelitus --cli books.get '{"id": 42}'           parameters as JSON
  caelitus --cli --json books.search | jq '.items[].title'

Parameters: --name=value or --name value; a bare --flag sets a boolean to true;
arrays as a,b,c, as repeated --name, or as JSON ('[1,2]'). Values are
converted to each parameter's type; the server checks the limits.

Options (before the method):
  --host <name>     Server address. Default: $CAELITUS_RPC_HOST, else the
                    server's bindAddress if specific, else 127.0.0.1.
  --port <port>     Server port. Default: $CAELITUS_RPC_PORT, else server.port
                    of the configuration file, else 9000.
  --config <file>   Configuration file to read server.port from (found like the
                    server finds it; only its "server" section is read).
  --timeout <sec>   How long to wait for the server. Default: 10.
  --json            Print the result as compact JSON (for scripts).
  --no-color        No colors (also when $NO_COLOR is set or the output is not
                    a terminal).
                    --json and --no-color may also come after the method.
  --help            Print this help.

Exit codes: 0 success, 1 the server returned an error, 2 bad usage,
3 the server cannot be reached.
)";

namespace {

bool isOption(const std::string& word) { return word.size() > 1 && word[0] == '-'; }

std::uint16_t parsePort(const std::string& text, const std::string& source) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (text.empty() || *end != '\0' || v < 1 || v > 65535)
        throw UsageError(source + ": expected a port number (1-65535), got '" + text + "'");
    return static_cast<std::uint16_t>(v);
}

std::optional<std::string> env(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

// The "server" section of a configuration file; null when the file is unreadable.
Json serverSection(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return nullptr;
    std::stringstream text;
    text << in.rdbuf();
    const Json root = Json::parse(text.str(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    return root.is_object() && root.contains("server") && root["server"].is_object() ? root["server"] : Json();
}

// One JSON-RPC call; returns the parsed reply (with "result" or "error").
Json call(const Transport& send, const std::string& method, const Json& params) {
    static int nextId = 1;
    Json request = {{"jsonrpc", "2.0"}, {"id", nextId++}, {"method", method}};
    if (!params.empty()) request["params"] = params;
    const std::string text = send(request.dump());
    Json reply = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!reply.is_object() || (!reply.contains("result") && !reply.contains("error")))
        throw net::NetError("The reply is not JSON-RPC: " + text.substr(0, 200));
    return reply;
}

const Json* findMethod(const Json& document, const std::string& name) {
    for (const auto& m : detail::arrayAt(document, "methods"))
        if (m.value("name", "") == name) return &m;
    return nullptr;
}

[[noreturn]] void unknownMethod(const Json& document, const std::string& name) {
    std::string best;
    std::size_t bestDistance = 4;
    for (const auto& m : detail::arrayAt(document, "methods")) {
        const std::string candidate = m.value("name", "");
        if (const auto d = detail::editDistance(name, candidate); d < bestDistance) bestDistance = d, best = candidate;
    }
    throw UsageError("Unknown method '" + name + "'" + (best.empty() ? "" : "; did you mean " + best + "?") +
                     " (caelitus --cli lists them)");
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

bool colorTerminal(const std::ostream& out) {
    return &out == &std::cout && ::isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
}

}  // namespace

Transport connectTcp(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
    auto client = std::make_shared<net::TcpClient>(host, port, timeout);
    return [client](const std::string& request) { return client->request(request); };
}

Invocation parseCommandLine(const std::vector<std::string>& args) {
    Invocation inv;
    std::size_t i = 0;
    for (; i < args.size() && isOption(args[i]); ++i) {
        std::string word = args[i], value;
        const auto eq = word.find('=');
        const bool inlineValue = eq != std::string::npos;
        if (inlineValue) value = word.substr(eq + 1), word = word.substr(0, eq);
        const auto takeValue = [&]() -> std::string {
            if (inlineValue) return value;
            if (i + 1 >= args.size()) throw UsageError(word + " needs a value");
            return args[++i];
        };

        if (word == "--help" || word == "-h") {
            inv.options.help = true;
        } else if (word == "--json") {
            inv.options.json = true;
        } else if (word == "--no-color") {
            inv.options.color = false;
        } else if (word == "--host") {
            inv.options.host = takeValue();
        } else if (word == "--port") {
            inv.options.port = parsePort(takeValue(), "--port");
        } else if (word == "--config") {
            inv.options.config = takeValue();
        } else if (word == "--timeout") {
            const std::string text = takeValue();
            char* end = nullptr;
            const double seconds = std::strtod(text.c_str(), &end);
            if (text.empty() || *end != '\0' || !(seconds > 0) || seconds > 3600)
                throw UsageError("--timeout: expected seconds (up to 3600), got '" + text + "'");
            inv.options.timeout = std::chrono::milliseconds(static_cast<long long>(std::ceil(seconds * 1000)));
        } else {
            throw UsageError("Unknown option " + word + " (client options go before the method name)");
        }
    }
    // --json and --no-color may also follow the method (no method has
    // parameters by those names).
    for (; i < args.size(); ++i) {
        if (args[i] == "--json") inv.options.json = true;
        else if (args[i] == "--no-color") inv.options.color = false;
        else inv.command.push_back(args[i]);
    }
    return inv;
}

std::pair<std::string, std::uint16_t> resolveEndpoint(const Options& options) {
    std::optional<std::string> host = options.host ? options.host : env("CAELITUS_RPC_HOST");
    std::optional<std::uint16_t> port = options.port;
    if (!port)
        if (const auto p = env("CAELITUS_RPC_PORT")) port = parsePort(*p, "CAELITUS_RPC_PORT");

    if (!host || !port) {
        Json server;
        if (options.config) {
            if (!std::filesystem::is_regular_file(*options.config))
                throw UsageError("--config: no such file: " + *options.config);
            server = serverSection(*options.config);
            if (server.is_null()) throw UsageError("--config: " + *options.config + " is not a configuration file");
        } else {
            try {
                const char* const noArgs[] = {"caelitus"};
                server = serverSection(AppConfig::locate(1, noArgs));
            } catch (const ConfigError&) {
                // No configuration file: use the defaults.
            }
        }
        if (!port && server.is_object() && server.contains("port") && server["port"].is_number_integer())
            port = parsePort(server["port"].dump(), "server.port");
        if (!host && server.is_object() && server.contains("bindAddress") && server["bindAddress"].is_string()) {
            const std::string bind = server["bindAddress"].get<std::string>();
            if (!bind.empty() && bind != "0.0.0.0" && bind != "::") host = bind;
        }
    }
    return {host.value_or("127.0.0.1"), port.value_or(9000)};
}

int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err, const Connector& connect) {
    try {
        const Invocation inv = parseCommandLine(args);
        const Options& options = inv.options;
        if (options.help) return out << kUsage, kOk;
        const bool color = options.color && !options.json && colorTerminal(out);

        const auto [host, port] = resolveEndpoint(options);
        const std::string where = host + ":" + std::to_string(port);
        Transport send;
        Json document;
        try {
            send = connect(host, port, options.timeout);
            const Json reply = call(send, "rpc.discover", Json::object());
            if (!reply.contains("result") || !reply["result"].is_object())
                throw net::NetError("rpc.discover failed; is this a caelitus server?");
            document = reply["result"];
        } catch (const net::NetError& e) {
            err << "caelitus --cli: " << e.what() << "\n"
                << "Is a caelitus server running at " << where
                << "? Another address: --host/--port, or CAELITUS_RPC_HOST/CAELITUS_RPC_PORT.\n";
            return kUnreachable;
        }

        const std::vector<std::string>& command = inv.command;
        if (command.empty()) {
            out << "Methods of the caelitus server at " << where << " (" << document["info"].value("version", "")
                << "):\n\n"
                << methodList(document) << "\ncaelitus --cli help <method> shows a method's parameters.\n";
            return kOk;
        }
        if (command[0] == "help") {
            if (command.size() == 1) return out << methodList(document), kOk;
            const Json* method = findMethod(document, command[1]);
            if (!method) unknownMethod(document, command[1]);
            return out << methodHelp(*method), kOk;
        }

        const Json* method = findMethod(document, command[0]);
        if (!method) unknownMethod(document, command[0]);
        const std::vector<std::string> words(command.begin() + 1, command.end());
        for (const auto& w : words)
            if (w == "--help" || w == "-h") return out << methodHelp(*method), kOk;

        const Json params = buildParams(*method, words);
        Json reply;
        try {
            reply = call(send, command[0], params);
        } catch (const net::NetError& e) {
            err << "caelitus --cli: no answer from " << where << ": " << e.what() << "\n";
            return kUnreachable;
        }
        if (reply.contains("error")) {
            printError(err, reply["error"], options.json);
            return kCallFailed;
        }
        out << (options.json ? reply["result"].dump() : formatJson(reply["result"], color)) << "\n";
        return kOk;
    } catch (const UsageError& e) {
        err << "caelitus --cli: " << e.what() << "\n";
        return kBadUsage;
    }
}

}  // namespace caelitus::cli
