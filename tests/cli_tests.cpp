// Tests for the --cli client: argument conversion, help, endpoint resolution,
// and whole runs against a real TcpServer + JsonRpcHandler (no database).

#include "TestHarness.hpp"

#include "caelitus/api/JsonRpc.hpp"
#include "caelitus/api/OpenRpc.hpp"
#include "caelitus/api/Schema.hpp"
#include "caelitus/cli/Cli.hpp"
#include "caelitus/core/DomainErrors.hpp"
#include "caelitus/log/Log.hpp"
#include "caelitus/net/TcpClient.hpp"
#include "caelitus/net/TcpServer.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <unistd.h>

using namespace caelitus;
using namespace std::chrono_literals;
namespace S = api::schema;

namespace {

// An OpenRPC method object, as rpc.discover returns it.
Json echoMethod() {
    return Json::parse(R"({
      "name": "test.echo", "summary": "Echoes its parameters.",
      "params": [
        {"name": "id",      "required": true,  "description": "An id",  "schema": {"type": "integer", "minimum": 1}},
        {"name": "label",   "required": true,  "description": "A label", "schema": {"type": "string"}},
        {"name": "ratio",   "required": false, "description": "A ratio", "schema": {"type": "number"}},
        {"name": "enabled", "required": false, "description": "A flag",  "schema": {"type": "boolean"}},
        {"name": "tags",    "required": false, "description": "Tags",
         "schema": {"type": "array", "items": {"type": "string"}, "maxItems": 5}},
        {"name": "ids",     "required": false, "description": "Ids",
         "schema": {"type": "array", "items": {"type": "integer"}}},
        {"name": "order",   "required": false, "description": "Order", "schema": {"type": "string", "enum": ["asc", "desc"]}}
      ],
      "result": {"name": "echo", "schema": {"type": "object"}},
      "errors": [{"$ref": "#/components/errors/InvalidParams"}],
      "tags": [{"$ref": "#/components/tags/test"}]
    })");
}

std::string usageError(const std::vector<std::string>& words) {
    try {
        cli::buildParams(echoMethod(), words);
    } catch (const cli::UsageError& e) {
        return e.what();
    }
    throw test::Failure{"expected a UsageError"};
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A real server with a few test methods and rpc.discover.
struct Server {
    std::shared_ptr<api::JsonRpcHandler> rpc = std::make_shared<api::JsonRpcHandler>();
    std::unique_ptr<net::TcpServer> tcp;

    Server() {
        api::MethodBuilder(*rpc, "test.echo", "test", "Echoes its parameters.")
            .required("id", S::integer(1), "An id")
            .optional("tags", S::array(S::string(1)), "Tags")
            .optional("enabled", S::boolean(), "A flag")
            .returns("echo", Json{{"type", "object"}})
            .handler([](const api::Params& p) {
                Json out = {{"id", p.required<std::int64_t>("id")}};
                if (auto t = p.optional<std::vector<std::string>>("tags")) out["tags"] = *t;
                if (auto e = p.optional<bool>("enabled")) out["enabled"] = *e;
                return out;
            });
        api::MethodBuilder(*rpc, "test.find", "test", "Finds nothing.")
            .required("id", S::integer(1), "An id")
            .returns("thing", Json{{"type", "object"}})
            .errors({"NotFound"})
            .handler(
                [](const api::Params& p) -> Json { throw NotFoundError("thing", p.required<std::int64_t>("id")); });
        api::MethodBuilder(*rpc, "test.slow", "test", "Takes 500 ms.")
            .returns("done", S::boolean())
            .handler([](const api::Params&) {
                std::this_thread::sleep_for(500ms);
                return Json(true);
            });
        api::addDiscover(*rpc, {"test API", "9.9.9", "For tests"});

        net::TcpServerConfig config;
        config.bindAddress = "127.0.0.1";
        config.port = 0;
        tcp = std::make_unique<net::TcpServer>(config, rpc);
        tcp->start();
    }

    // Runs the client against this server; returns {exit code, stdout, stderr}.
    struct Result {
        int code;
        std::string out, err;
    };
    Result run(std::vector<std::string> args) const {
        args.insert(args.begin(), {"--port", std::to_string(tcp->port()), "--host", "127.0.0.1"});
        std::ostringstream out, err;
        const int code = cli::run(args, out, err);
        return {code, out.str(), err.str()};
    }
};

// Sets (or clears) an environment variable for the lifetime of the object.
struct EnvVar {
    std::string name;
    std::optional<std::string> old;
    EnvVar(std::string n, const char* value) : name(std::move(n)) {
        if (const char* o = std::getenv(name.c_str())) old = o;
        if (value) ::setenv(name.c_str(), value, 1);
        else ::unsetenv(name.c_str());
    }
    ~EnvVar() {
        if (old) ::setenv(name.c_str(), old->c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

std::filesystem::path writeConfig(const std::string& text) {
    const auto path =
        std::filesystem::temp_directory_path() / ("caelitus-cli-test-" + std::to_string(::getpid()) + ".json");
    std::ofstream(path) << text;
    return path;
}

}  // namespace

// ---- buildParams -------------------------------------------------------------

TEST(values_are_converted_to_the_schema_type) {
    const Json p =
        cli::buildParams(echoMethod(), {"--id=42", "--label", "x", "--ratio=2.5", "--enabled", "--order=asc"});
    CHECK_EQ(p, Json({{"id", 42}, {"label", "x"}, {"ratio", 2.5}, {"enabled", true}, {"order", "asc"}}));
}

TEST(strings_that_look_like_numbers_stay_strings) {
    CHECK_EQ(cli::buildParams(echoMethod(), {"--id=1", "--label=007"})["label"], Json("007"));
}

TEST(booleans_accept_a_bare_flag_or_a_value) {
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--enabled"})["enabled"], Json(true));
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--enabled=false"})["enabled"], Json(false));
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--enabled", "no"})["enabled"], Json(false));
    // A bare flag does not swallow the next parameter.
    const Json p = cli::buildParams(echoMethod(), {"--enabled", "--id", "3", "--label=x"});
    CHECK_EQ(p["enabled"], Json(true));
    CHECK_EQ(p["id"], Json(3));
    CHECK(contains(usageError({"1", "x", "--enabled=maybe"}), "expected true or false"));
}

TEST(arrays_from_commas_repetition_or_json) {
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--tags=a,b"})["tags"], Json({"a", "b"}));
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--tags", "a", "--tags", "b,c"})["tags"], Json({"a", "b", "c"}));
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--ids=3,4"})["ids"], Json({3, 4}));
    CHECK_EQ(cli::buildParams(echoMethod(), {"1", "x", "--ids=[5, 6]"})["ids"], Json({5, 6}));
    CHECK(contains(usageError({"1", "x", "--ids=3,x"}), "--ids: expected an integer, got 'x'"));
}

TEST(positional_words_fill_the_required_parameters_in_order) {
    CHECK_EQ(cli::buildParams(echoMethod(), {"7", "hello"}), Json({{"id", 7}, {"label", "hello"}}));
    // A named required parameter is skipped by the positional ones.
    CHECK_EQ(cli::buildParams(echoMethod(), {"--id=7", "hello"}), Json({{"id", 7}, {"label", "hello"}}));
    CHECK(contains(usageError({"7", "hello", "extra"}), "Unexpected argument 'extra'"));
}

TEST(a_json_object_gives_several_parameters_and_named_words_win) {
    const Json p = cli::buildParams(echoMethod(), {R"({"id": 1, "label": "json", "ids": [9]})", "--label=named"});
    CHECK_EQ(p, Json({{"id", 1}, {"label", "named"}, {"ids", Json::array({9})}}));
    CHECK(contains(usageError({R"({"nope": 1})"}), "no parameter --nope"));
}

TEST(mistakes_are_reported_before_anything_is_sent) {
    CHECK(contains(usageError({"--id=1", "--labl=x"}), "did you mean --label?"));
    CHECK(contains(usageError({"--id=1", "--zzzzzz=x"}), "its parameters are --id, --label"));
    CHECK(contains(usageError({"--id=abc", "--label=x"}), "--id: expected an integer, got 'abc'"));
    CHECK(contains(usageError({"--id=1.5", "--label=x"}), "expected an integer"));
    CHECK(contains(usageError({"--id=1"}), "missing --label (A label)"));
    CHECK(contains(usageError({"1", "x", "--ratio"}), "--ratio needs a value"));
}

// ---- command line and endpoint ---------------------------------------------------

TEST(client_options_come_before_the_method_and_json_also_after) {
    const auto inv = cli::parseCommandLine({"--port=9100", "--timeout", "1.5", "books.get", "42", "--json"});
    CHECK_EQ(*inv.options.port, 9100);
    CHECK_EQ(inv.options.timeout.count(), 1500);
    CHECK(inv.options.json);
    CHECK_EQ(inv.command, (std::vector<std::string>{"books.get", "42"}));
    // After the method, other words belong to the method.
    CHECK_EQ(cli::parseCommandLine({"m", "--port=1"}).command, (std::vector<std::string>{"m", "--port=1"}));
    CHECK_THROWS_AS(cli::parseCommandLine({"--bogus", "m"}), cli::UsageError);
    CHECK_THROWS_AS(cli::parseCommandLine({"--port", "70000"}), cli::UsageError);
    CHECK_THROWS_AS(cli::parseCommandLine({"--host"}), cli::UsageError);
}

TEST(endpoint_from_options_environment_or_configuration) {
    const EnvVar host("CAELITUS_RPC_HOST", nullptr), port("CAELITUS_RPC_PORT", nullptr);
    const auto config = writeConfig(R"({ // comments are allowed
        "server": {"port": 9555, "bindAddress": "127.0.0.2"}, "database": {"password": "${UNSET_SECRET}"}})");
    cli::Options o;
    o.config = config.string();
    CHECK_EQ(cli::resolveEndpoint(o), (std::pair<std::string, std::uint16_t>{"127.0.0.2", 9555}));
    {
        const EnvVar envPort("CAELITUS_RPC_PORT", "9600"), envHost("CAELITUS_RPC_HOST", "example.org");
        CHECK_EQ(cli::resolveEndpoint(o), (std::pair<std::string, std::uint16_t>{"example.org", 9600}));
        o.port = 9700;
        o.host = "localhost";
        CHECK_EQ(cli::resolveEndpoint(o), (std::pair<std::string, std::uint16_t>{"localhost", 9700}));
    }
    // A wildcard bind address means "connect locally".
    std::ofstream(config) << R"({"server": {"port": 9556, "bindAddress": "0.0.0.0"}})";
    cli::Options wildcard;
    wildcard.config = config.string();
    CHECK_EQ(cli::resolveEndpoint(wildcard), (std::pair<std::string, std::uint16_t>{"127.0.0.1", 9556}));
    std::filesystem::remove(config);

    cli::Options missing;
    missing.config = "/no/such/config.json";
    CHECK_THROWS_AS(cli::resolveEndpoint(missing), cli::UsageError);
}

// ---- output ------------------------------------------------------------------------

TEST(formatted_json_matches_a_plain_dump_without_color) {
    const Json v = Json::parse(R"({"a": [1, 2.5, {"b": null}], "c": "x\"y", "d": {}, "e": [], "f": true})");
    CHECK_EQ(cli::formatJson(v, false), v.dump(2));
    const std::string colored = cli::formatJson(v, true);
    CHECK(contains(colored, "\033[36m\"a\"\033[0m"));
    CHECK_EQ(Json::parse(std::string(R"({"x": 1})")), Json::parse(cli::formatJson(Json{{"x", 1}}, false)));
}

TEST(help_shows_usage_parameters_limits_and_errors) {
    const std::string help = cli::methodHelp(echoMethod());
    CHECK(contains(help, "test.echo: Echoes its parameters."));
    CHECK(contains(help, "Usage: caelitus --cli test.echo --id <integer> --label <string> [options]"));
    CHECK(contains(help, "--tags <string,...>"));
    CHECK(contains(help, "--order asc|desc"));
    CHECK(contains(help, "[required, >= 1]"));
    CHECK(contains(help, "[up to 5 items]"));
    CHECK(contains(help, "Errors:  InvalidParams"));
}

// ---- whole runs against a server -----------------------------------------------------

TEST(calls_a_method_and_prints_its_result) {
    const Server server;
    const auto r = server.run({"test.echo", "5", "--tags=a,b", "--enabled"});
    CHECK_EQ(r.code, cli::kOk);
    CHECK_EQ(Json::parse(r.out), Json({{"id", 5}, {"tags", {"a", "b"}}, {"enabled", true}}));
    CHECK(r.err.empty());

    const auto json = server.run({"--json", "test.echo", "--id=6"});
    CHECK_EQ(json.out, "{\"id\":6}\n");
}

TEST(lists_methods_and_shows_help_from_the_servers_description) {
    const Server server;
    const auto list = server.run({});
    CHECK_EQ(list.code, cli::kOk);
    CHECK(contains(list.out, "(9.9.9)"));
    CHECK(contains(list.out, "test\n  test.echo"));
    CHECK(contains(list.out, "rpc.discover"));

    const auto help = server.run({"help", "test.echo"});
    CHECK_EQ(help.code, cli::kOk);
    CHECK(contains(help.out, "--id <integer>"));
    CHECK_EQ(server.run({"test.echo", "--help"}).out, help.out);
}

TEST(server_errors_exit_1_and_usage_errors_exit_2) {
    const Server server;
    const auto notFound = server.run({"test.find", "3"});
    CHECK_EQ(notFound.code, cli::kCallFailed);
    CHECK(contains(notFound.err, "Error -32001: thing 3 not found"));
    CHECK(contains(notFound.err, R"("entity":"thing")"));

    // Limits are the server's job; its reason is shown without repeating the field.
    const auto invalid = server.run({"test.echo", "--id=0"});
    CHECK_EQ(invalid.code, cli::kCallFailed);
    CHECK(contains(invalid.err, "Error -32602: Invalid params\n  --id: "));
    CHECK(!contains(invalid.err, "--id: id:"));

    const auto typo = server.run({"test.ecoh"});
    CHECK_EQ(typo.code, cli::kBadUsage);
    CHECK(contains(typo.err, "did you mean test.echo?"));
    CHECK_EQ(server.run({"test.echo", "--id=x"}).code, cli::kBadUsage);
    CHECK_EQ(server.run({"help", "nope.nope"}).code, cli::kBadUsage);
}

TEST(an_unreachable_or_slow_server_exits_3) {
    const Server server;
    const auto slow = server.run({"--timeout=0.1", "test.slow"});
    CHECK_EQ(slow.code, cli::kUnreachable);
    CHECK(contains(slow.err, "no answer from 127.0.0.1:"));

    // Nothing listens on the port of a stopped server.
    std::uint16_t freePort = 0;
    {
        const Server stopped;
        freePort = stopped.tcp->port();
        stopped.tcp->stop();
    }
    std::ostringstream out, err;
    CHECK_EQ(cli::run({"--port", std::to_string(freePort), "--host", "127.0.0.1", "system.ping"}, out, err),
             cli::kUnreachable);
    CHECK(contains(err.str(), "Connection refused"));
    CHECK(contains(err.str(), "Is a caelitus server running at 127.0.0.1:" + std::to_string(freePort)));
}

TEST(a_peer_that_is_not_json_rpc_exits_3) {
    const cli::Connector fake = [](const std::string&, std::uint16_t, std::chrono::milliseconds) {
        return cli::Transport([](const std::string&) { return std::string("HTTP/1.1 400 Bad Request"); });
    };
    std::ostringstream out, err;
    CHECK_EQ(cli::run({"--port=1", "system.ping"}, out, err, fake), cli::kUnreachable);
    CHECK(contains(err.str(), "not JSON-RPC"));
}

TEST(help_option_needs_no_server) {
    const cli::Connector never = [](const std::string&, std::uint16_t, std::chrono::milliseconds) -> cli::Transport {
        throw test::Failure{"should not connect"};
    };
    std::ostringstream out, err;
    CHECK_EQ(cli::run({"--help"}, out, err, never), cli::kOk);
    CHECK(contains(out.str(), "Usage: caelitus --cli"));
}

// ---- TcpClient -----------------------------------------------------------------------

TEST(tcp_client_reuses_one_connection_for_several_requests) {
    const Server server;
    net::TcpClient client("127.0.0.1", server.tcp->port());
    for (int i = 1; i <= 3; ++i) {
        const Json reply = Json::parse(client.request(
            Json({{"jsonrpc", "2.0"}, {"id", i}, {"method", "test.echo"}, {"params", {{"id", i}}}}).dump()));
        CHECK_EQ(reply["result"]["id"], Json(i));
    }
    CHECK_EQ(server.tcp->stats().totalConnections, 1u);
}

int main() {
    caelitus::log::LogConfig config;
    config.console = false;  // the test servers' logs would bury the results
    caelitus::log::init(config);
    return test::runAll();
}
