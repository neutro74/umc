#if defined(__linux__)

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "cli/hotspot_attach.hpp"
#include "cli/process.hpp"
#include "common/platform.hpp"

namespace umc::cli {

namespace {

// Everything goes through /proc/<pid>/root so sandboxed launchers (Flatpak) work.
std::string root_of(int pid) { return "/proc/" + std::to_string(pid) + "/root"; }

int read_ns_pid(int pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("NSpid:", 0) != 0) continue;
        std::istringstream ss(line.substr(6));
        int v = pid;
        while (ss >> v) {}
        return v;
    }
    return pid;
}

bool is_jvm(int pid) {
    std::ifstream maps("/proc/" + std::to_string(pid) + "/maps");
    std::string line;
    while (std::getline(maps, line))
        if (line.find("libjvm.so") != std::string::npos) return true;
    return false;
}

}  // namespace

bool read_process(int pid, GameProcess& out) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!in) return false;
    out = GameProcess{};
    out.pid = pid;
    std::string arg;
    while (std::getline(in, arg, '\0')) out.args.push_back(arg);
    if (out.args.empty()) return false;
    std::string exe = out.args[0].substr(out.args[0].rfind('/') + 1);
    if (exe.rfind("java", 0) != 0) return false;
    out.ns_pid = read_ns_pid(pid);
    classify(out);
    return true;
}

std::vector<GameProcess> java_processes() {
    std::vector<GameProcess> procs;
    DIR* d = opendir("/proc");
    if (!d) return procs;
    while (dirent* e = readdir(d)) {
        if (!std::isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
        GameProcess p;
        if (read_process(std::atoi(e->d_name), p)) procs.push_back(std::move(p));
    }
    closedir(d);
    std::sort(procs.begin(), procs.end(), [](auto& a, auto& b) { return a.pid < b.pid; });
    return procs;
}

bool process_alive(int pid) { return kill(pid, 0) == 0; }

std::string agent_socket(const GameProcess& p) {
    return root_of(p.pid) + plat::join(plat::runtime_dir(), plat::agent_socket_name(p.ns_pid));
}

std::string stage_file(const GameProcess& p, const std::string& host_path, std::string& error) {
    struct stat st{};
    if (stat(host_path.c_str(), &st) != 0) {
        error = "missing " + host_path;
        return "";
    }
    // One directory per file version, so overwriting a library the game has mapped can't happen
    // and the file keeps its real name.
    std::string base = host_path.substr(host_path.rfind('/') + 1);
    std::string name = base;
    std::string version = std::to_string(st.st_size) + "-" + std::to_string(st.st_mtime);
    std::string inside = plat::join(plat::join(plat::runtime_dir(), "staged"), version);
    std::string outside = root_of(p.pid) + inside;
    if (!plat::make_dirs(outside)) {
        error = "cannot create " + outside;
        return "";
    }
    std::string target = plat::join(outside, name);
    if (!plat::file_exists(target) && !plat::copy_file(host_path, target)) {
        error = "cannot copy " + host_path + " into the game's sandbox";
        return "";
    }
    return plat::join(inside, name);
}

bool inject_library(const GameProcess& p, const std::string& library, const std::string& options,
                    std::string& message) {
    if (!is_jvm(p.pid)) {
        message = "process " + std::to_string(p.pid) + " is not a HotSpot JVM";
        return false;
    }
    const std::string root = root_of(p.pid);
    const std::string ns = std::to_string(p.ns_pid);
    AttachTarget t{p.pid, root + "/tmp/.java_pid" + ns,
                   {"/proc/" + std::to_string(p.pid) + "/cwd/.attach_pid" + ns, root + "/tmp/.attach_pid" + ns}};
    return hotspot_load(t, library, options, message);
}

}  // namespace umc::cli

#endif
