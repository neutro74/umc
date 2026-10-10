// The interactive shell: `umc` with no arguments in a terminal. Everything typed runs through the same
// dispatcher as the one-shot CLI, with history, tab completion and live game events above the prompt.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <thread>

#include "cli/dispatch.hpp"
#include "cli/inject.hpp"
#include "common/shared.hpp"

#if !defined(_WIN32)
#include <poll.h>
extern "C" {
#include "linenoise.h"
}
#endif

namespace umc::cli {

namespace {

using Clock = std::chrono::steady_clock;

// One streamed connection (a game's log, or the daemon's events), read a line at a time.
struct Stream {
    plat::Socket socket = plat::kNoSocket;
    std::string pending;
    bool header = true;

    bool open(const std::string& path, const std::vector<std::string>& request) {
        close();
        socket = plat::connect_local(path);
        if (socket == plat::kNoSocket) return false;
        plat::send_all(socket, shared::encode_request(request));
        header = true;
        pending.clear();
        return true;
    }
    void close() {
        plat::close_socket(socket);
        socket = plat::kNoSocket;
    }
    bool alive() const { return socket != plat::kNoSocket; }

    // Call when the socket is readable. Appends complete lines; false once the peer has gone.
    bool pump(std::vector<std::string>& lines) {
        std::string chunk = plat::recv_some(socket);
        if (chunk.empty()) {
            close();
            return false;
        }
        pending += chunk;
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (header) header = false;      // the leading "ok"
            else if (!line.empty()) lines.push_back(line);  // empty lines are keepalives
        }
        return true;
    }
};

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    bool any = false;
    for (char c : line) {
        if (quote) {
            if (c == quote) quote = 0;
            else cur += c;
        } else if (c == '"' || c == '\'') {
            quote = c;
            any = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty() || any) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (!cur.empty() || any) out.push_back(cur);
    return out;
}

// The game the shell is talking to: pinned by the user, or the only one with UMC in it.
struct Target {
    int pid = 0;
    std::string version = "?", side = "?";
    bool valid() const { return pid != 0; }
};

Target find_target(int pinned, bool& ambiguous) {
    ambiguous = false;
    std::vector<GameProcess> with_agent;
    for (auto& p : java_processes())
        if (p.has_agent && (pinned == 0 || p.pid == pinned)) with_agent.push_back(p);
    if (with_agent.size() > 1) ambiguous = true;
    if (with_agent.size() != 1) return {};
    Target t{with_agent[0].pid};
    agent_game_info(with_agent[0], t.version, t.side);
    return t;
}

std::string prompt_for(const Target& t, bool ambiguous) {
    if (t.valid()) return "umc[" + t.version + " " + t.side + "]> ";
    return ambiguous ? "umc[several games, use <pid>]> " : "umc[no game]> ";
}

const std::vector<std::string>& command_names() {
    static const std::vector<std::string> names = {
        "attach", "bye", "call", "check", "classes", "clear", "daemon", "dump", "exit", "fields", "game",
        "gc", "get", "help", "hooks", "info", "install", "log", "map", "mappings", "mem", "methods",
        "ps", "quit", "search", "script", "scripts", "set", "status", "threads", "trace", "uninstall",
        "untrace", "use", "versions",
    };
    return names;
}

// Plugin IDs used for argument completion. Cached briefly.
struct Names {
    std::vector<std::string> scripts;
    Clock::time_point fetched{};
    int pid = 0;
};

void refresh_names(Names& names, int pid) {
    if (pid == 0) return;
    if (names.pid == pid && Clock::now() - names.fetched < std::chrono::seconds(5)) return;
    names = Names{};
    names.pid = pid;
    names.fetched = Clock::now();
    GameProcess p;
    if (!read_process(pid, p)) return;
    auto collect = [&](const std::vector<std::string>& request, std::vector<std::string>& into, bool skip_marker) {
        std::string reply;
        if (!agent_request(p, request, reply) || reply.rfind("ok\n", 0) != 0) return;
        std::istringstream in(reply.substr(3));
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string first, second;
            ls >> first;
            if (skip_marker) ls >> second;   // "[on]  name  description"
            std::string name = skip_marker ? second : first;
            if (!name.empty() && name[0] != '[' && name != "no") into.push_back(name);
        }
    };
    collect({"script", "list"}, names.scripts, false);
}

std::string history_path() { return plat::join(plat::state_dir(), "history"); }

void print_banner() {
    std::cout << kBanner << "  v" << kVersion << "   type help for commands, tab to complete, quit to leave\n\n";
}

// Commands the shell handles itself. Returns true when it did.
bool builtin(const std::vector<std::string>& args, int& pinned, bool& quit) {
    const std::string& c = args[0];
    if (c == "quit" || c == "exit" || c == "bye") {
        quit = true;
        return true;
    }
    if (c == "clear") {
        std::cout << "\x1b[2J\x1b[H" << std::flush;
        return true;
    }
    if (c == "use") {
        pinned = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
        std::cout << (pinned ? "talking to " + std::to_string(pinned) : std::string("back to automatic")) << "\n";
        return true;
    }
    return false;
}

#if !defined(_WIN32)

Names g_names;   // read by the completion callback
int g_names_pid = 0;

void complete(const char* buf, linenoiseCompletions* lc) {
    std::string line = buf;
    size_t space = line.find(' ');
    if (space == std::string::npos) {
        for (auto& n : command_names())
            if (n.rfind(line, 0) == 0) linenoiseAddCompletion(lc, n.c_str());
        return;
    }
    std::vector<std::string> words = tokenize(line.substr(0, line.find_last_of(' ') + 1));
    if (words.empty()) return;
    std::string prefix = line.substr(line.find_last_of(' ') + 1), head = line.substr(0, line.find_last_of(' ') + 1);
    std::vector<std::string> options;
    const std::string& cmd = words[0];
    if (cmd == "script" && words.size() == 1) {
        options = {"load", "unload", "reload", "list", "call"};
    } else if (cmd == "script" && words.size() == 2 && words[1] == "unload") {
        refresh_names(g_names, g_names_pid);
        options = g_names.scripts;
    } else if (cmd == "daemon" && words.size() == 1) {
        options = {"start", "stop", "status", "events", "install", "uninstall"};
    } else if (cmd == "mappings" && words.size() == 1) {
        options = {"fetch", "import", "list"};
    }
    for (auto& o : options)
        if (o.rfind(prefix, 0) == 0) linenoiseAddCompletion(lc, (head + o).c_str());
}

int run_shell(int pinned) {
    plat::make_dirs(plat::state_dir());
    linenoiseSetCompletionCallback(complete);
    linenoiseHistorySetMaxLen(500);
    linenoiseHistoryLoad(history_path().c_str());
    print_banner();

    Stream game_events, daemon_events;
    bool ambiguous = false, quit = false;
    Target target = find_target(pinned, ambiguous);
    g_names_pid = target.pid;
    int streamed_pid = 0;
    auto connect_streams = [&] {
        if (target.pid != streamed_pid) {
            game_events.close();
            streamed_pid = target.pid;
            if (target.valid()) {
                GameProcess p;
                if (read_process(target.pid, p)) game_events.open(agent_socket(p), {"follow"});
            }
        }
        if (!daemon_events.alive()) daemon_events.open(plat::daemon_socket(), {"events"});
    };
    connect_streams();

    linenoiseState ls;
    char buf[4096];
    std::string prompt = prompt_for(target, ambiguous);
    auto start_edit = [&] { linenoiseEditStart(&ls, -1, -1, buf, sizeof buf, prompt.c_str()); };
    start_edit();
    auto last_refresh = Clock::now();

    while (!quit) {
        std::vector<pollfd> fds = {{0, POLLIN, 0}};
        if (game_events.alive()) fds.push_back({int(game_events.socket), POLLIN, 0});
        if (daemon_events.alive()) fds.push_back({int(daemon_events.socket), POLLIN, 0});
        int ready = poll(fds.data(), nfds_t(fds.size()), 1000);

        std::vector<std::string> events;
        for (size_t i = 1; ready > 0 && i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP))) continue;
            (int(game_events.socket) == fds[i].fd ? game_events : daemon_events).pump(events);
        }
        if (!events.empty()) {
            linenoiseHide(&ls);
            for (auto& e : events) std::printf("\x1b[2m%s\x1b[0m\r\n", e.c_str());
            std::fflush(stdout);
            linenoiseShow(&ls);
        }

        if (Clock::now() - last_refresh > std::chrono::seconds(1)) {
            last_refresh = Clock::now();
            Target now = find_target(pinned, ambiguous);
            std::string wanted = prompt_for(now, ambiguous);
            if (now.pid != target.pid || wanted != prompt) {
                target = now;
                g_names_pid = target.pid;
                connect_streams();
                if (wanted != prompt && ls.len == 0) {   // only swap the prompt when nothing is half-typed
                    prompt = wanted;
                    linenoiseEditStop(&ls);
                    std::printf("\r\n");
                    start_edit();
                }
            }
            connect_streams();
        }

        if (ready > 0 && (fds[0].revents & POLLIN)) {
            char* line = linenoiseEditFeed(&ls);
            if (line == linenoiseEditMore) continue;
            linenoiseEditStop(&ls);
            if (!line) {                       // Ctrl-D leaves, Ctrl-C just clears the line
                if (errno == EAGAIN) {
                    std::printf("\r\n");
                    start_edit();
                    continue;
                }
                std::printf("\n");
                break;
            }
            std::vector<std::string> args = tokenize(line);
            if (!args.empty()) {
                linenoiseHistoryAdd(line);
                linenoiseHistorySave(history_path().c_str());
                if (!builtin(args, pinned, quit)) {
                    std::fflush(stdout);
                    dispatch(args, pinned ? pinned : target.pid);
                }
                std::cout.flush();
            }
            free(line);
            if (quit) break;
            start_edit();
        }
    }
    if (!quit) return 0;
    return 0;
}

#else  // Windows: the console already provides line editing and history

int run_shell(int pinned) {
    print_banner();
    bool ambiguous = false, quit = false;
    Stream events;
    std::thread printer;
    int streamed = 0;
    while (!quit) {
        Target target = find_target(pinned, ambiguous);
        if (target.pid != streamed) {
            events.close();
            streamed = target.pid;
            GameProcess p;
            if (target.valid() && read_process(target.pid, p)) events.open(agent_socket(p), {"follow"});
        }
        std::vector<std::string> lines;
        while (events.alive() && plat::wait_readable(events.socket, 0)) events.pump(lines);
        for (auto& l : lines) std::cout << l << "\n";
        std::cout << prompt_for(target, ambiguous) << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) break;
        std::vector<std::string> args = tokenize(line);
        if (args.empty()) continue;
        if (!builtin(args, pinned, quit)) dispatch(args, pinned ? pinned : target.pid);
    }
    return 0;
}

#endif

}  // namespace

int shell(int pid) { return run_shell(pid); }

}  // namespace umc::cli
