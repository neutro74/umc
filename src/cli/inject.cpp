#include "cli/inject.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

#include "common/shared.hpp"

namespace fs = std::filesystem;

namespace umc::cli {

namespace {

// Pulls "minecraft <version> (<side>, ..." out of the agent's status text.
void parse_status(const std::string& status, std::string& version, std::string& side, bool& has_mappings) {
    std::istringstream in(status);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("minecraft ", 0) == 0) {
            std::istringstream ls(line.substr(10));
            ls >> version >> side;
            if (!side.empty() && side[0] == '(') side = side.substr(1);
            if (!side.empty() && side.back() == ',') side.pop_back();
        }
        if (line.rfind("mappings ", 0) == 0) has_mappings = line.find("none") == std::string::npos;
    }
}

}  // namespace

bool agent_game_info(const GameProcess& p, std::string& version, std::string& side) {
    std::string reply;
    if (!agent_request(p, {"status"}, reply) || reply.rfind("ok\n", 0) != 0) return false;
    bool has_mappings = false;
    parse_status(reply, version, side, has_mappings);
    return !version.empty();
}

std::string agent_library() {
    if (const char* env = std::getenv("UMC_AGENT")) return env;
    fs::path dir = fs::path(plat::self_exe()).parent_path();
    const std::string ext = plat::kLibExt;
    for (const fs::path& candidate : {dir / ("libumc" + ext), dir / ("umc" + ext),
                                      dir / ".." / "lib" / "umc" / ("libumc" + ext),
                                      dir / ".." / "lib64" / "umc" / ("libumc" + ext)}) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) return fs::canonical(candidate, ec).string();
    }
    return "";
}

bool inject(const GameProcess& p, const InjectOptions& opts, std::vector<std::string>& log) {
    if (p.has_agent) {
        log.push_back("UMC is already running in " + std::to_string(p.pid));
        return true;
    }
    std::string lib = agent_library(), error;
    if (lib.empty()) {
        log.push_back("cannot find the agent library next to the umc binary (set UMC_AGENT)");
        return false;
    }
    std::string staged = stage_file(p, lib, error);
    if (staged.empty()) {
        log.push_back(error);
        return false;
    }

    std::string options = opts.agent_options;
    std::string message;
    if (!inject_library(p, staged, options, message)) {
        log.push_back(message);
        return false;
    }

    // Windows injection returns before the agent finishes starting, so always wait for it.
    std::string reply;
    bool up = false;
    for (int i = 0; i < 100 && !up; ++i) {
        up = agent_request(p, {"status"}, reply) && reply.rfind("ok\n", 0) == 0;
        if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!up) {
        log.push_back("injected, but the agent never answered; check the game log for [UMC] lines");
        return false;
    }

    std::string version, side;
    bool has_mappings = false;
    parse_status(reply, version, side, has_mappings);
    log.push_back("injected into " + std::to_string(p.pid) + ": minecraft " + version + " " + side);
    if (has_mappings || version.empty() || version == "unknown") return true;

    std::string path = shared::mappings_path(version, side);
    if (!plat::file_exists(path) && opts.fetch_mappings && !plat::file_exists(path + ".none")) {
        bool available = false;
        if (fetch_mappings(version, available, message) && !available) {
            log.push_back(version + " is unobfuscated, no mappings needed");
            return true;
        }
    }
    if (!plat::file_exists(path)) {
        if (!plat::file_exists(path + ".none")) log.push_back("no mappings for " + version + ", names are used as-is");
        return true;
    }
    std::string inside = stage_file(p, path, error);
    if (inside.empty() || !agent_request(p, {"mappings", "load", inside}, reply) || reply.rfind("ok\n", 0) != 0) {
        log.push_back("could not load mappings: " + (inside.empty() ? error : reply.substr(reply.find('\n') + 1)));
        return true;
    }
    log.push_back(reply.substr(3, reply.find('\n', 3) - 3));
    return true;
}

}  // namespace umc::cli
