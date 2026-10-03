#pragma once

/// @file
/// The `caelitus --cli` client: calls any JSON-RPC method of a running server
/// from the command line.
/// @ingroup cli

#include "caelitus/json/JsonTypes.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace caelitus::cli {

/// Exit codes of run().
enum ExitCode : int {
    kOk = 0,           ///< The call succeeded.
    kCallFailed = 1,   ///< The server answered with a JSON-RPC error.
    kBadUsage = 2,     ///< Bad command line: unknown method or parameter, bad value.
    kUnreachable = 3,  ///< No server, or no (valid) reply.
};

/// A command-line mistake; run() prints it and exits with kBadUsage.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// A connection to the server.
class Connection {
public:
    virtual ~Connection() = default;
    /// Sends one JSON-RPC request (text) and returns the reply (text).
    /// @throws net::NetError when the server does not reply.
    virtual std::string request(const std::string& text) = 0;
    /// False once the server has closed the connection (idle timeout, restart).
    virtual bool isOpen() const = 0;
};

/// Opens a connection.
/// @throws net::NetError if the connection fails.
using Connector = std::function<std::unique_ptr<Connection>(const std::string& host, std::uint16_t port,
                                                            std::chrono::milliseconds timeout)>;

/// Connects over TCP with net::TcpClient (the default Connector).
std::unique_ptr<Connection> connectTcp(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout);

/// Client options, given before the method name.
struct Options {
    std::optional<std::string> host;           ///< `--host`
    std::optional<std::uint16_t> port;         ///< `--port`
    std::optional<std::string> config;         ///< `--config`: where to read `server.port`.
    std::chrono::milliseconds timeout{10000};  ///< `--timeout <seconds>`
    bool json = false;                         ///< `--json`: raw JSON output.
    bool color = true;                         ///< false with `--no-color` (or not a terminal).
    bool help = false;                         ///< `--help`
};

/// The command line, split into client options and the command (the method
/// name and its arguments, or `help [method]`).
struct Invocation {
    Options options;                   ///< Everything before the method name.
    std::vector<std::string> command;  ///< The method name and what follows it; may be empty.
};

/// Splits the arguments that follow `--cli`.
/// @throws UsageError for an unknown option or a missing option value.
Invocation parseCommandLine(const std::vector<std::string>& args);

/// Where to connect: `--host`, else `$CAELITUS_RPC_HOST`, else the server's
/// `bindAddress` when it is a specific address, else 127.0.0.1; `--port`, else
/// `$CAELITUS_RPC_PORT`, else `server.port` in the configuration file, else 9000.
///
/// The configuration file is found like the server finds it (`--config`,
/// `$CAELITUS_CONFIG`, `config/config.json`), but only its `server` section is
/// read: the client needs neither the database password nor any other setting.
/// @throws UsageError if `--config` names a missing file or a value is invalid.
std::pair<std::string, std::uint16_t> resolveEndpoint(const Options& options);

/// Builds the `params` object of a call from command-line words, using the
/// method's parameter schemas from the OpenRPC document:
///
/// | Words                     | Meaning |
/// |---------------------------|---------|
/// | `--name=value`, `--name value` | Parameter `name` |
/// | `--flag`                  | A boolean parameter set to true (`--flag=false` to clear) |
/// | `--tags=a,b` or `--tags a --tags b` | An array (items converted by the item schema) |
/// | `--ids='[1,2]'`           | Any value as JSON |
/// | `42`, `top-books`         | The next required parameter not given by name |
/// | `'{"id": 42}'`            | Several parameters at once, as a JSON object |
///
/// Values are converted to the schema's type (`"42"` becomes the integer 42).
/// Range and length limits are left to the server, which reports them.
/// @param method  An OpenRPC method object (`name`, `params`).
/// @param words   What follows the method name on the command line.
/// @throws UsageError for an unknown parameter (with a suggestion), a value of
///         the wrong type, or a missing required parameter.
Json buildParams(const Json& method, const std::vector<std::string>& words);

/// Help for one method: summary, description, parameters, result, errors.
std::string methodHelp(const Json& method);

/// Every method of the document, grouped by tag, one line each.
std::string methodList(const Json& document);

/// Pretty-prints a JSON value with two-space indentation; with `color`, keys,
/// strings, numbers and literals get ANSI colors.
std::string formatJson(const Json& value, bool color);

/// Runs the client: parses `args` (what follows `--cli`), connects and reads
/// the API description with `rpc.discover`. Then:
///
/// - with a method (or `help [method]`): runs that one command;
/// - without one, on a terminal: an interactive prompt (history, Tab completion,
///   hints) until `exit` or Ctrl-D;
/// - without one, with `in` not a terminal: one command per line of `in`
///   (`caelitus --cli < commands.txt`).
///
/// @param args     Command-line words after `--cli`.
/// @param in       Commands, when none is given in `args`; the prompt is used
///                 only when this is `std::cin` on a terminal.
/// @param out      Results and help.
/// @param err      Errors.
/// @param connect  How to reach the server (tests pass their own).
/// @return An ExitCode; for several commands, that of the last one that failed.
int run(const std::vector<std::string>& args, std::istream& in, std::ostream& out, std::ostream& err,
        const Connector& connect = connectTcp);

/// The usage text of `caelitus --cli --help`.
extern const char* const kUsage;

}  // namespace caelitus::cli
