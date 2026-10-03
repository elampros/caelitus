#pragma once

/// @file
/// A client session: one connection, the server's API description, and the
/// commands, completions and hints built on it.
/// @ingroup cli

#include "caelitus/cli/Cli.hpp"

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace caelitus::cli {

/// Splits a command line into words the way a shell does: blanks separate
/// words; `'…'` keeps everything literally; `"…"` keeps blanks and honours
/// `\"` and `\\`; outside quotes `\` escapes the next character.
///
/// @code
/// splitWords(R"(books.search --title="Ο Μικρός Πρίγκιπας" --tags=a,b)")
///   == {"books.search", "--title=Ο Μικρός Πρίγκιπας", "--tags=a,b"}
/// @endcode
/// @throws UsageError for an unterminated quote.
std::vector<std::string> splitWords(const std::string& line);

/// A connection to a server together with its OpenRPC document.
///
/// Before every command it checks that the connection is still open and, if
/// the server closed it (idle timeout, restart), connects again and re-reads
/// the description. A command whose reply was lost is **not** repeated: it may
/// have been carried out (think `books.create`).
class Session {
public:
    /// Connects and reads the API description (`rpc.discover`).
    /// @throws net::NetError if the server cannot be reached or is not a caelitus server.
    Session(std::string host, std::uint16_t port, std::chrono::milliseconds timeout, Connector connect);

    /// `host:port`
    const std::string& endpoint() const { return endpoint_; }

    /// The server's OpenRPC document.
    const Json& document() const { return document_; }

    /// Runs one command: `help`, `help <method>`, or `<method> [arguments]`
    /// (see buildParams()). `--json` and `--no-color` may appear among the
    /// arguments and apply to this command only.
    ///
    /// @param json   Compact JSON output unless the command says otherwise.
    /// @param color  Colored output unless the command says `--no-color`.
    /// @return kOk, or kCallFailed after printing the server's error to `err`.
    /// @throws UsageError for an unknown method, parameter or bad value.
    /// @throws net::NetError if the server cannot be reached or does not reply.
    int execute(std::vector<std::string> words, std::ostream& out, std::ostream& err, bool json, bool color);

    /// Candidates for the word being typed, and that word (`context`).
    struct Suggestions {
        std::vector<std::string> items;  ///< Full words that may replace `context`.
        std::string context;             ///< The part of the input they replace.
    };

    /// Tab completion for `input` (the line up to the cursor): built-in
    /// commands and method names first; then the method's parameters not used
    /// yet (`--title=`, a bare `--flag`), and enum or boolean values after
    /// `--sort=` or `--sort `.
    Suggestions complete(const std::string& input) const;

    /// What to show greyed out after the cursor. While a word is typed: the
    /// single completion it can become, with a method's summary or a
    /// parameter's type and description. After a method and a blank: the
    /// required parameters still missing. `context` is empty when the hint is
    /// shown as is.
    Suggestions hint(const std::string& input) const;

private:
    void connect();
    Json call(const std::string& method, const Json& params);
    const Json* findMethod(const std::string& name) const;

    std::string host_;
    std::uint16_t port_;
    std::chrono::milliseconds timeout_;
    Connector connector_;
    std::string endpoint_;
    std::unique_ptr<Connection> connection_;
    Json document_;
    int nextId_ = 1;
};

/// Where the commands of runLines() come from.
class LineReader {
public:
    virtual ~LineReader() = default;
    /// The next line without its line break; nullopt at the end of the input.
    virtual std::optional<std::string> read() = 0;
    /// Called after a line was run (the terminal reader saves it in the history).
    virtual void ran(const std::string& /*line*/) {}
};

/// Runs the commands read from `reader`, one per line. Blank lines and lines
/// starting with `#` are skipped; `exit` or `quit` stops.
///
/// @param interactive  true: errors are printed and the session goes on; the
///                     result is kOk. false (a script): each error is
///                     prefixed with `line N:`, and the result is the exit code
///                     of the last command that failed (kOk if none did).
int runLines(Session& session, LineReader& reader, std::ostream& out, std::ostream& err, const Options& options,
             bool interactive);

}  // namespace caelitus::cli
