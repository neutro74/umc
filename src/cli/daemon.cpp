// The background service: watches for Minecraft JVMs, injects UMC into new ones once they have settled,
// and streams what happens to anyone listening (the interactive shell, `umc daemon log -f`).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "cli/dispatch.hpp"
#include "cli/inject.hpp"
#include "cli/scripts.hpp"
#include "common/shared.hpp"

namespace umc::cli {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kScanMs = 1500;
constexpr int kSettleSeconds = 4;   // let a new JVM get past its own startup before touching it
constexpr int kMaxAttempts = 4;

struct Game {
    std::string version = "?", side = "?", state = "seen";
    Clock::time_point first_seen = Clock::now();
    int attempts = 0;
    bool announced = false;
    std::string note;
};

class Daemon {
public:
    explicit Daemon(bool auto_attach) : auto_attach_(auto_attach) {}

    int run() {
        std::string error;
        plat::make_dirs(plat::runtime_dir());
        listener_ = plat::listen_local(plat::daemon_socket(), error);
        if (listener_ == plat::kNoSocket) {
            std::cerr << "daemon: " << error << "\n";
            return 1;
        }
        log("daemon started, auto-attach " + std::string(auto_attach_ ? "on" : "off"));
        std::thread(&Daemon::accept_loop, this).detach();
        while (!stop_) {
            scan();
            std::unique_lock<std::mutex> lock(wait_mu_);
            wait_cv_.wait_for(lock, std::chrono::milliseconds(kScanMs), [&] { return stop_.load(); });
        }
        log("daemon stopped");
        plat::remove_file(plat::daemon_socket());
        return 0;
    }

private:
    void log(const std::string& line) {
        std::string stamped = line;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::string dir = plat::state_dir();
            plat::make_dirs(dir);
            std::ofstream(plat::join(dir, "daemon.log"), std::ios::app) << stamped << "\n";
            std::string out = stamped + "\n";
            for (auto it = subscribers_.begin(); it != subscribers_.end();)
                it = plat::send_all(*it, out) ? std::next(it) : (plat::close_socket(*it), subscribers_.erase(it));
        }
    }

    void scan() {
        std::vector<GameProcess> procs = java_processes();
        std::map<int, bool> alive;
        for (auto& p : procs) {
            if (!p.minecraft) continue;
            alive[p.pid] = true;
            Game& g = games_[p.pid];  // created on first sight
            if (!p.version.empty()) g.version = p.version;
            if (p.side != "-") g.side = p.side;

            if (p.has_agent) {
                if (g.state != "attached") {
                    g.state = "attached";
                    mark_attached(p, g);
                }
                continue;
            }
            bool settled = Clock::now() - g.first_seen > std::chrono::seconds(kSettleSeconds);
            if (!auto_attach_ || !settled || g.attempts >= kMaxAttempts) {
                if (!g.announced) {
                    g.announced = true;
                    log("saw game " + std::to_string(p.pid) + (auto_attach_ ? ", attaching shortly" : ""));
                }
                continue;
            }
            ++g.attempts;
            std::vector<std::string> out;
            InjectOptions opts;
            GameProcess live = p;
            if (inject(p, opts, out)) {
                live.has_agent = true;
                load_installed_scripts(live, out);
                g.state = "attached";
                g.note.clear();
                mark_attached(live, g);
            } else {
                g.state = g.attempts >= kMaxAttempts ? "failed" : "retrying";
                g.note = out.empty() ? "" : out.back();
            }
            for (auto& line : out) log("pid " + std::to_string(p.pid) + ": " + line);
        }
        for (auto it = games_.begin(); it != games_.end();) {
            if (alive.count(it->first)) {
                ++it;
                continue;
            }
            log("game " + std::to_string(it->first) + " exited");
            it = games_.erase(it);
        }
    }

    void mark_attached(const GameProcess& p, Game& g) {
        std::string version, side;
        if (agent_game_info(p, version, side)) {
            g.version = version;
            g.side = side;
        }
        log("attached " + std::to_string(p.pid) + " (" + g.version + " " + g.side + ")");
    }

    std::string status() {
        std::ostringstream o;
        o << "auto-attach " << (auto_attach_ ? "on" : "off") << "\n";
        if (games_.empty()) o << "no Minecraft running\n";
        for (auto& [pid, g] : games_) o << pid << "  " << g.side << "  " << g.version << "  " << g.state << (g.note.empty() ? "" : "  " + g.note) << "\n";
        return o.str();
    }

    void accept_loop() {
        for (;;) {
            plat::Socket client = plat::accept_local(listener_);
            if (client == plat::kNoSocket) break;
            std::thread(&Daemon::handle, this, client).detach();
        }
    }

    void handle(plat::Socket client) {
        std::vector<std::string> args = shared::read_request(client);
        std::string cmd = args.empty() ? "" : args[0];
        if (cmd == "events") {
            plat::send_all(client, "ok\n");
            std::lock_guard<std::mutex> lock(mu_);
            subscribers_.push_back(client);  // kept open; the daemon writes to it as things happen
            return;
        }
        std::string reply;
        if (cmd == "ping") reply = "ok\npong\n";
        else if (cmd == "status") {
            std::lock_guard<std::mutex> lock(mu_);
            reply = "ok\n" + status();
        } else if (cmd == "stop") {
            reply = "ok\nstopping\n";
            stop_ = true;
            wait_cv_.notify_all();
        } else reply = "err\nunknown daemon command\n";
        plat::send_all(client, reply);
        plat::close_socket(client);
        if (stop_) plat::close_socket(listener_);
    }

    bool auto_attach_;
    std::atomic<bool> stop_{false};
    plat::Socket listener_ = plat::kNoSocket;
    std::mutex mu_, wait_mu_;
    std::condition_variable wait_cv_;
    std::map<int, Game> games_;
    std::vector<plat::Socket> subscribers_;
};

bool daemon_alive() {
    std::string reply;
    return shared::request(plat::daemon_socket(), {"ping"}, reply) && reply.rfind("ok\n", 0) == 0;
}

int start_daemon(bool auto_attach) {
    if (daemon_alive()) {
        std::cout << "the daemon is already running\n";
        return 0;
    }
    std::vector<std::string> args = {"daemon", "run"};
    if (!auto_attach) args.push_back("--no-auto");
    if (!plat::spawn_detached(plat::self_exe(), args)) {
        std::cerr << "could not start the daemon\n";
        return 1;
    }
    for (int i = 0; i < 50; ++i) {
        if (daemon_alive()) {
            std::cout << "daemon started, it will attach to Minecraft as soon as it sees it\n";
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::cerr << "the daemon did not come up; see " << plat::join(plat::state_dir(), "daemon.log") << "\n";
    return 1;
}

int stream_events() {
    plat::Socket s = plat::connect_local(plat::daemon_socket());
    if (s == plat::kNoSocket) {
        std::cerr << "the daemon is not running (umc daemon start)\n";
        return 1;
    }
    plat::send_all(s, shared::encode_request({"events"}));
    std::string chunk, pending;
    bool header = true;
    while (!(chunk = plat::recv_some(s)).empty()) {
        pending += chunk;
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (header) header = false;
            else if (!line.empty()) std::cout << line << std::endl;
        }
    }
    return 0;
}

// Starts the daemon at login with whatever the OS uses for that.
int install_autostart(bool remove) {
    std::string exe = plat::self_exe();
#if defined(_WIN32)
    std::string cmd = remove ? "reg delete HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run /v UMC /f"
                             : "reg add HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run /v UMC /t REG_SZ /d \"\\\"" + exe +
                                   "\\\" daemon run\" /f";
    return std::system(cmd.c_str()) == 0 ? 0 : 1;
#elif defined(__APPLE__)
    std::string path = plat::join(plat::join(plat::join(std::getenv("HOME") ? std::getenv("HOME") : "", "Library"), "LaunchAgents"), "dev.umc.daemon.plist");
    if (remove) {
        std::system(("launchctl unload \"" + path + "\" 2>/dev/null").c_str());
        plat::remove_file(path);
        std::cout << "removed " << path << "\n";
        return 0;
    }
    plat::make_dirs(plat::join(std::getenv("HOME") ? std::getenv("HOME") : "", "Library/LaunchAgents"));
    std::ofstream(path) << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>\n"
                           "<key>Label</key><string>dev.umc.daemon</string>\n<key>ProgramArguments</key><array><string>" << exe
                        << "</string><string>daemon</string><string>run</string></array>\n<key>RunAtLoad</key><true/>\n"
                           "<key>KeepAlive</key><true/>\n</dict></plist>\n";
    std::system(("launchctl load \"" + path + "\"").c_str());
    std::cout << "installed " << path << "\n";
    return 0;
#else
    const char* home = std::getenv("HOME");
    std::string dir = std::string(home ? home : "") + "/.config/systemd/user";
    std::string path = dir + "/umc.service";
    if (remove) {
        std::system("systemctl --user disable --now umc.service 2>/dev/null");
        plat::remove_file(path);
        std::cout << "removed " << path << "\n";
        return 0;
    }
    plat::make_dirs(dir);
    std::ofstream(path) << "[Unit]\nDescription=UMC, the Universal Minecraft Connector\n\n[Service]\nExecStart=" << exe
                        << " daemon run\nRestart=on-failure\n\n[Install]\nWantedBy=default.target\n";
    int rc = std::system("systemctl --user daemon-reload && systemctl --user enable --now umc.service");
    std::cout << (rc == 0 ? "installed and started " : "wrote ") << path << (rc == 0 ? "\n" : " (enable it with systemctl --user enable --now umc)\n");
    return 0;
#endif
}

}  // namespace

int daemon_command(const std::vector<std::string>& args) {
    std::string sub = args.empty() ? "status" : args[0];
    if (sub == "run") {  // foreground; what `start` launches
        bool auto_attach = std::find(args.begin(), args.end(), "--no-auto") == args.end();
        return Daemon(auto_attach).run();
    }
    if (sub == "start") return start_daemon(std::find(args.begin(), args.end(), "--no-auto") == args.end());
    if (sub == "events" || sub == "log") return stream_events();
    if (sub == "install") return install_autostart(false);
    if (sub == "uninstall") return install_autostart(true);
    if (sub == "status" || sub == "stop") {
        std::string reply;
        if (!shared::request(plat::daemon_socket(), {sub}, reply)) {
            std::cout << "the daemon is not running (umc daemon start)\n";
            return sub == "stop" ? 0 : 1;
        }
        std::cout << (reply.size() > 3 ? reply.substr(3) : "");
        return 0;
    }
    std::cerr << "usage: umc daemon start [--no-auto] | stop | status | events | install | uninstall\n";
    return 2;
}

}  // namespace umc::cli
