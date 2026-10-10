// Finding Java processes and injecting the agent into them. Implemented per OS.
#pragma once

#include <string>
#include <vector>

namespace umc::cli {

struct GameProcess {
    int pid = 0;
    int ns_pid = 0;  // pid inside the process's own namespace (differs under Flatpak)
    std::vector<std::string> args;
    bool minecraft = false;
    bool has_agent = false;
    std::string side, version;
};

std::vector<GameProcess> java_processes();
bool read_process(int pid, GameProcess& out);
bool process_alive(int pid);

// Control socket path as seen by this process.
std::string agent_socket(const GameProcess& p);
// Makes a host file readable by the target (sandboxes can't see our paths) and returns the path
// the target should use.
std::string stage_file(const GameProcess& p, const std::string& host_path, std::string& error);
// Loads the agent library into the running JVM.
bool inject_library(const GameProcess& p, const std::string& library, const std::string& options,
                    std::string& message);

// Shared helpers (process.cpp).
void classify(GameProcess& p);
bool agent_request(const GameProcess& p, const std::vector<std::string>& args, std::string& reply);

}  // namespace umc::cli
