/// @file
/// cli::run(): the `caelitus --cli` command flow; options and endpoint.
/// @ingroup cli

#include "caelitus/cli/Cli.hpp"

#include "caelitus/cli/Session.hpp"

#include "caelitus/config/AppConfig.hpp"
#include "caelitus/net/TcpClient.hpp"
#include "cli/Detail.hpp"
#include "cli/Terminal.hpp"

#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

namespace caelitus::cli {

const char* const kUsage = R"(Usage: caelitus --cli [options] [<method> [parameters...]]
       caelitus --cli [options] help [<method>]
       caelitus --cli [options]                  (interactive)
       caelitus --cli [options] < commands.txt   (one command per line)

Calls methods of a running caelitus server. The methods and their parameters
are read from the server itself (rpc.discover), so every method it has can be
called.

  caelitus --cli help                             list the methods
  caelitus --cli help books.search                a method's parameters
  caelitus --cli system.health
  caelitus --cli books.get 42                     a required parameter by position
  caelitus --cli books.search --title=dune --pageSize 5 --tags=sci-fi,classic
  caelitus --cli scheduler.pause top-books
  caelitus --cli books.get '{"id": 42}'           parameters as JSON
  caelitus --cli --json books.search | jq '.items[].title'

Without a method, on a terminal, it opens a prompt: the same commands, with
history (arrow keys, Ctrl-R), Tab completion of methods, parameters and values,
and hints. exit, quit or Ctrl-D leaves. Quote values with blanks:
  caelitus> books.search --title="Ο Μικρός Πρίγκιπας"

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
  --json            Print results as compact JSON (for scripts).
  --no-color        No colors (also when $NO_COLOR is set or the output is not
                    a terminal).
                    --json and --no-color may also come after the method.
  --help            Print this help.

Exit codes: 0 success, 1 the server returned an error, 2 bad usage,
3 the server cannot be reached. With commands from a file: the code of the
last command that failed.
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

// Commands from a stream (a script on stdin), one per line.
class StreamReader final : public LineReader {
public:
    explicit StreamReader(std::istream& in) : in_(in) {}
    std::optional<std::string> read() override {
        std::string line;
        if (!std::getline(in_, line)) return std::nullopt;
        return line;
    }

private:
    std::istream& in_;
};

bool colorTerminal(const std::ostream& out) {
    return &out == &std::cout && ::isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
}

}  // namespace

std::unique_ptr<Connection> connectTcp(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
    class TcpConnection final : public Connection {
    public:
        TcpConnection(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout)
            : client_(host, port, timeout) {}
        std::string request(const std::string& text) override { return client_.request(text); }
        bool isOpen() const override { return client_.isOpen(); }

    private:
        net::TcpClient client_;
    };
    return std::make_unique<TcpConnection>(host, port, timeout);
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

int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err,
        const Connector& connect) {
    try {
        Invocation inv = parseCommandLine(args);
        Options& options = inv.options;
        if (options.help) return out << kUsage, kOk;
        options.color = options.color && !options.json && colorTerminal(out);

        const auto [host, port] = resolveEndpoint(options);
        std::optional<Session> session;
        try {
            session.emplace(host, port, options.timeout, connect);
        } catch (const net::NetError& e) {
            err << "caelitus --cli: " << e.what() << "\n"
                << "Is a caelitus server running at " << host << ":" << port
                << "? Another address: --host/--port, or CAELITUS_RPC_HOST/CAELITUS_RPC_PORT.\n";
            return kUnreachable;
        }

        if (!inv.command.empty()) {
            try {
                return session->execute(inv.command, out, err, options.json, options.color);
            } catch (const net::NetError& e) {
                err << "caelitus --cli: no answer from " << session->endpoint() << ": " << e.what() << "\n";
                return kUnreachable;
            }
        }

        if (&in == &std::cin && ::isatty(STDIN_FILENO)) {
            const Json& info = session->document()["info"];
            out << "caelitus " << info.value("version", "") << " at " << session->endpoint() << ", "
                << detail::arrayAt(session->document(), "methods").size()
                << " methods. help lists them, Tab completes, Ctrl-D or exit quits.\n";
            auto reader = terminalReader(*session, options.color);
            return runLines(*session, *reader, out, err, options, /*interactive=*/true);
        }
        StreamReader reader(in);
        return runLines(*session, reader, out, err, options, /*interactive=*/false);
    } catch (const UsageError& e) {
        err << "caelitus --cli: " << e.what() << "\n";
        return kBadUsage;
    }
}

}  // namespace caelitus::cli
