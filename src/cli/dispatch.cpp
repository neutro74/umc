#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <iostream>

#include "cli/dispatch.hpp"
#include "cli/inject.hpp"
#include "cli/process.hpp"
#include "cli/scripts.hpp"
#include "common/shared.hpp"

using namespace umc;

namespace umc::cli {

const char* kVersion = "0.1.0";

const char* kBanner = R"(
  ██╗   ██╗███╗   ███╗ ██████╗
  ██║   ██║████╗ ████║██╔════╝
  ██║   ██║██╔████╔██║██║
  ██║   ██║██║╚██╔╝██║██║
  ╚██████╔╝██║ ╚═╝ ██║╚██████╗
   ╚═════╝ ╚═╝     ╚═╝ ╚═════╝
  Universal Minecraft Connector
)";

void usage() {
    std::cout << kBanner << "  v" << kVersion << R"(

usage: umc [-p PID] <command> [args...]

local
  ps [-a]                          list running Minecraft JVMs (-a: every JVM)
  attach [PID] [-o OPTS] [-l LIB]  inject into a running game (-l adds a plugin, repeatable)
  mappings fetch|import|list       manage obfuscation mappings
  install <file|url> [--yes]       install a script plugin (shows what it asks for first)
  uninstall <id> | scripts         remove a plugin, or list installed plugins

in the game (-p picks the process when several have UMC)
  status | threads | mem | gc
  modules | enable <m> | disable <m> | mod <m> [args...]
  load <plugin.so|script.js> | unload <plugin> | reload <plugin>
  classes <filter> | fields <class> | methods <class> | map <class> [member]
  get <class> <field> | set <class> <field> <value> | call <class> <method> [args...]
  dump <class> <file> | hooks | trace <class> <method> [desc] | untrace <id|all>
  log [n] | log -f

agent options (-o), comma separated
  version=26.3  side=client|server  mappings=FILE  enable=mod1+mod2
)";
}

// Picks the target: an explicit pid, or the only candidate.
bool pick(int pid, bool need_agent, GameProcess& out) {
    if (pid > 0) {
        if (!read_process(pid, out)) {
            std::cerr << "no java process with pid " << pid << "\n";
            return false;
        }
        return true;
    }
    std::vector<GameProcess> found;
    for (auto& p : java_processes())
        if (need_agent ? p.has_agent : (p.minecraft && !p.has_agent)) found.push_back(p);
    if (found.size() == 1) {
        out = found[0];
        return true;
    }
    if (found.empty())
        std::cerr << (need_agent ? "no game with UMC in it (try: umc attach)\n" : "no Minecraft JVM found (try: umc ps -a)\n");
    else
        std::cerr << "several candidates, pick one with -p PID (see: umc ps)\n";
    return false;
}

int cmd_ps(bool all) {
    std::printf("%-8s %-7s %-12s %-5s %s\n", "PID", "SIDE", "VERSION", "UMC", "COMMAND");
    for (auto& p : java_processes()) {
        if (!all && !p.minecraft) continue;
        std::string cmd;
        for (auto& a : p.args) cmd += a + " ";
        if (cmd.size() > 60) cmd = cmd.substr(0, 57) + "...";
        std::printf("%-8d %-7s %-12s %-5s %s\n", p.pid, p.side.c_str(), p.version.empty() ? "?" : p.version.c_str(),
                    p.has_agent ? "yes" : "no", cmd.c_str());
    }
    return 0;
}

int cmd_attach(int pid, const std::vector<std::string>& args) {
    InjectOptions opts;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "-o" && i + 1 < args.size()) opts.agent_options = args[++i];
        else if (args[i] == "-l" && i + 1 < args.size()) opts.plugins.push_back(args[++i]);
        else if (args[i] == "--no-fetch") opts.fetch_mappings = false;
        else pid = std::atoi(args[i].c_str());
    }
    GameProcess p;
    if (!pick(pid, false, p)) return 1;
    std::vector<std::string> log;
    bool ok = inject(p, opts, log);
    if (ok && !p.has_agent) {  // freshly injected: bring the installed scripts along
        GameProcess live = p;
        live.has_agent = true;
        load_installed_scripts(live, log);
    }
    for (auto& line : log) (ok ? std::cout : std::cerr) << line << "\n";
    return ok ? 0 : 1;
}

int cmd_follow(const GameProcess& p) {
    plat::Socket s = plat::connect_local(agent_socket(p));
    if (s == plat::kNoSocket) {
        std::cerr << "cannot reach the agent in " << p.pid << "\n";
        return 1;
    }
    plat::send_all(s, shared::encode_request({"follow"}));
    std::string chunk, pending;
    bool header = true;
    while (!(chunk = plat::recv_some(s)).empty()) {
        pending += chunk;
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (header) header = false;          // the leading "ok"
            else if (!line.empty()) std::cout << line << std::endl;  // empty lines are keepalives
        }
    }
    plat::close_socket(s);
    return 0;
}

int dispatch(std::vector<std::string> args, int pid) {
    if (args.empty() || args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 1 : 0;
    }
    if (args[0] == "--version" || args[0] == "version") {
        std::cout << kBanner << "  v" << kVersion << "\n";
        return 0;
    }

    const std::string& cmd = args[0];
    if (cmd == "ps") return cmd_ps(args.size() > 1 && args[1] == "-a");
    if (cmd == "mappings") return mappings_command({args.begin() + 1, args.end()});
    if (cmd == "attach") return cmd_attach(pid, args);
    if (cmd == "daemon") return daemon_command({args.begin() + 1, args.end()});
    if (cmd == "shell" || cmd == "repl") return shell(pid);
    if (cmd == "install" || cmd == "uninstall" || cmd == "scripts") {
        GameProcess target;
        bool have = false;
        for (auto& p : java_processes())
            if (p.has_agent && (pid == 0 || p.pid == pid)) { target = p; have = true; break; }
        return scripts_command(cmd, args, have ? &target : nullptr);
    }

    GameProcess p;
    if (!pick(pid, true, p)) return 1;
    if (cmd == "log" && args.size() > 1 && args[1] == "-f") return cmd_follow(p);

    // Files named on the command line must be readable by the game, which may be sandboxed.
    auto stage_arg = [&](size_t index) {
        if (index >= args.size() || !plat::file_exists(args[index])) return;
        std::string error, staged = stage_file(p, std::filesystem::absolute(args[index]).string(), error);
        if (staged.empty()) std::cerr << "warning: " << error << "\n";
        else args[index] = staged;
    };
    if (cmd == "load") stage_arg(1);
    if (cmd == "script" && args.size() > 2 && args[1] == "load") stage_arg(2);

    std::string reply;
    if (!agent_request(p, args, reply)) {
        std::cerr << "cannot reach the agent in " << p.pid << "\n";
        return 1;
    }
    bool ok = reply.rfind("ok\n", 0) == 0;
    size_t nl = reply.find('\n');
    (ok ? std::cout : std::cerr) << (nl == std::string::npos ? "" : reply.substr(nl + 1));
    return ok ? 0 : 1;
}

}  // namespace umc::cli
