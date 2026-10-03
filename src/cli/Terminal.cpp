/// @file
/// cli::terminalReader(): the interactive prompt, on replxx.
/// @ingroup cli

#include "cli/Terminal.hpp"

#include <replxx.hxx>

#include <cerrno>
#include <cstdlib>

namespace caelitus::cli {

namespace {

using replxx::Replxx;

// replxx counts in Unicode code points, not bytes ("Πρίγκιπας" is 9, not 18).
int codePoints(const std::string& utf8) {
    int n = 0;
    for (const char c : utf8)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;  // not a continuation byte
    return n;
}

bool blank(const std::string& line) { return line.find_first_not_of(" \t") == std::string::npos; }

class TerminalReader final : public LineReader {
public:
    TerminalReader(const Session& session, bool color)
        : session_(session),
          prompt_(color ? "\033[1;36mcaelitus>\033[0m " : "caelitus> "),
          history_(historyFile()) {
        rx_.install_window_change_handler();
        rx_.set_max_history_size(1000);
        rx_.set_unique_history(true);
        rx_.set_word_break_characters(" \t");
        rx_.set_max_hint_rows(0);  // one inline hint, no list under the line
        rx_.set_no_color(!color);  // (replxx shows hints only in color)
        rx_.set_completion_callback([this](const std::string& input, int& contextLen) {
            const auto s = session_.complete(input);
            contextLen = codePoints(s.context);
            Replxx::completions_t completions;
            for (const auto& item : s.items) completions.emplace_back(item);
            return completions;
        });
        rx_.set_hint_callback([this](const std::string& input, int& contextLen, Replxx::Color& hintColor) {
            const auto s = session_.hint(input);
            contextLen = codePoints(s.context);
            hintColor = Replxx::Color::GRAY;
            return Replxx::hints_t(s.items.begin(), s.items.end());
        });
        if (!history_.empty()) rx_.history_load(history_.string());
    }

    std::optional<std::string> read() override {
        for (;;) {
            errno = 0;
            if (const char* line = rx_.input(prompt_)) return std::string(line);
            if (errno == EAGAIN) continue;  // Ctrl-C: start a new line
            return std::nullopt;            // Ctrl-D
        }
    }

    void ran(const std::string& line) override {
        if (blank(line)) return;
        rx_.history_add(line);
        if (history_.empty()) return;
        std::error_code ec;  // a history that cannot be saved is not worth an error
        std::filesystem::create_directories(history_.parent_path(), ec);
        rx_.history_save(history_.string());
    }

private:
    const Session& session_;
    const std::string prompt_;
    const std::filesystem::path history_;
    Replxx rx_;
};

}  // namespace

std::filesystem::path historyFile() {
    std::filesystem::path dir;
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) dir = state;
    else if (const char* home = std::getenv("HOME"); home && *home)
        dir = std::filesystem::path(home) / ".local" / "state";
    else return {};
    return dir / "caelitus" / "cli-history";
}

std::unique_ptr<LineReader> terminalReader(const Session& session, bool color) {
    return std::make_unique<TerminalReader>(session, color);
}

}  // namespace caelitus::cli
