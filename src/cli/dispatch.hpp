#pragma once

#include <string>
#include <vector>

#include "cli/process.hpp"

namespace umc::cli {

extern const char* kVersion;
extern const char* kBanner;

void usage();
// Picks the target game: an explicit pid, or the only candidate. Prints why when it can't.
bool pick(int pid, bool need_agent, GameProcess& out);
// Runs one command line as the CLI would (the shell reuses this for everything it types).
int dispatch(std::vector<std::string> args, int pid);

int daemon_command(const std::vector<std::string>& args);
int shell(int pid);

}  // namespace umc::cli
