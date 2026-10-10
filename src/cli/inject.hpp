#pragma once

#include <string>
#include <vector>

#include "cli/process.hpp"

namespace umc::cli {

struct InjectOptions {
    std::string agent_options;           // passed through to the agent (key=value,...)
    std::vector<std::string> plugins;    // extra plugin libraries on top of the config dir
    bool fetch_mappings = true;          // download Mojang mappings when the version needs them
};

// Locates the agent library shipped next to the umc binary.
std::string agent_library();

// Injects, waits for the agent to answer and loads mappings. Progress goes to `log`.
bool inject(const GameProcess& p, const InjectOptions& opts, std::vector<std::string>& log);

// Mappings (mappings_fetch.cpp). Returns false on network errors; `available` reports whether
// the version ships obfuscation mappings at all.
bool fetch_mappings(std::string version, bool& available, std::string& message);
int mappings_command(const std::vector<std::string>& args);

}  // namespace umc::cli
