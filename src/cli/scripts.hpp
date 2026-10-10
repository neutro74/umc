#pragma once

#include <string>
#include <vector>

#include "cli/process.hpp"

namespace umc::cli {

// umc install | uninstall | scripts. Returns the exit code.
int scripts_command(const std::string& cmd, const std::vector<std::string>& args, const GameProcess* running);

// Loads every installed script into a game that has the agent. Messages go to `log`.
void load_installed_scripts(const GameProcess& p, std::vector<std::string>& log);

}  // namespace umc::cli
