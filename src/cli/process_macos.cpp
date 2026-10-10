#if defined(__APPLE__)

#include <libproc.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "cli/hotspot_attach.hpp"
#include "cli/process.hpp"
#include "common/platform.hpp"

namespace umc::cli {

namespace {

// KERN_PROCARGS2 layout: int argc, exec path, NUL padding, then argv strings.
std::vector<std::string> process_args(int pid) {
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int)) return {};
    std::vector<char> buf(size);
    if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0) return {};
    int argc = 0;
    std::memcpy(&argc, buf.data(), sizeof argc);
    size_t i = sizeof argc;
    while (i < size && buf[i] != '\0') ++i;  // exec path
    while (i < size && buf[i] == '\0') ++i;  // padding
    std::vector<std::string> args;
    while (i < size && int(args.size()) < argc) {
        std::string a(&buf[i]);
        i += a.size() + 1;
        args.push_back(a);
    }
    return args;
}

// HotSpot on macOS puts attach files in the per-user Darwin temp dir.
std::string darwin_temp() {
    char buf[1024] = {};
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    std::string t = n > 0 ? std::string(buf) : "/tmp/";
    if (!t.empty() && t.back() == '/') t.pop_back();
    return t;
}

}  // namespace

bool read_process(int pid, GameProcess& out) {
    out = GameProcess{};
    out.pid = out.ns_pid = pid;
    out.args = process_args(pid);
    if (out.args.empty()) return false;
    std::string exe = out.args[0].substr(out.args[0].rfind('/') + 1);
    if (exe.rfind("java", 0) != 0) return false;
    classify(out);
    return true;
}

std::vector<GameProcess> java_processes() {
    std::vector<int> pids(4096);
    int n = proc_listallpids(pids.data(), int(pids.size() * sizeof(int)));
    std::vector<GameProcess> procs;
    for (int i = 0; i < n; ++i) {
        GameProcess p;
        if (pids[size_t(i)] > 0 && read_process(pids[size_t(i)], p)) procs.push_back(std::move(p));
    }
    std::sort(procs.begin(), procs.end(), [](auto& a, auto& b) { return a.pid < b.pid; });
    return procs;
}

bool process_alive(int pid) { return kill(pid, 0) == 0; }

std::string agent_socket(const GameProcess& p) {
    return plat::join(plat::runtime_dir(), plat::agent_socket_name(p.pid));
}

std::string stage_file(const GameProcess&, const std::string& host_path, std::string& error) {
    if (!plat::file_exists(host_path)) {
        error = "missing " + host_path;
        return "";
    }
    return host_path;
}

bool inject_library(const GameProcess& p, const std::string& library, const std::string& options,
                    std::string& message) {
    const std::string pid = std::to_string(p.pid);
    const std::string tmp = darwin_temp();
    char cwd[PROC_PIDPATHINFO_MAXSIZE] = {};
    proc_vnodepathinfo info{};
    std::vector<std::string> triggers;
    if (proc_pidinfo(p.pid, PROC_PIDVNODEPATHINFO, 0, &info, sizeof info) > 0) {
        std::strncpy(cwd, info.pvi_cdir.vip_path, sizeof cwd - 1);
        triggers.push_back(std::string(cwd) + "/.attach_pid" + pid);
    }
    triggers.push_back(tmp + "/.attach_pid" + pid);
    AttachTarget t{p.pid, tmp + "/.java_pid" + pid, triggers};
    return hotspot_load(t, library, options, message);
}

}  // namespace umc::cli

#endif
