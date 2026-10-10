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

// Asks a game's agent for its Minecraft version and side. False if it doesn't answer.
bool agent_game_info(const GameProcess& p, std::string& version, std::string& side);

// Mappings (mappings_fetch.cpp). Returns false on network errors; `available` reports whether
// the version ships obfuscation mappings at all.
bool fetch_mappings(std::string version, bool& available, std::string& message);
// Versions before 1.14.4 have no Mojang mappings; these build them from MCP data instead.
bool is_legacy_version(const std::string& version);
bool build_legacy_mappings(const std::string& version, std::string& message);
int mappings_command(const std::vector<std::string>& args);

}  // namespace umc::cli
