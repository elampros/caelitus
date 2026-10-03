#pragma once

/// @file
/// The interactive prompt on a terminal (replxx).
/// @ingroup cli

#include "caelitus/cli/Session.hpp"

#include <filesystem>
#include <memory>

namespace caelitus::cli {

/// A LineReader on the terminal: the `caelitus>` prompt, line editing, a
/// history kept across runs, Tab completion and hints, all from `session`.
/// Ctrl-C abandons the line being typed; Ctrl-D ends the input.
std::unique_ptr<LineReader> terminalReader(const Session& session, bool color);

/// Where the prompt's history is kept: `$XDG_STATE_HOME/caelitus/cli-history`,
/// else `~/.local/state/caelitus/cli-history`; empty without a home directory.
std::filesystem::path historyFile();

}  // namespace caelitus::cli
