#include "common/platform.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <afunix.h>
#include <direct.h>
#include <io.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif

namespace umc::plat {

namespace {

#if defined(_WIN32)
using Native = SOCKET;
#else
using Native = int;
#endif
Native nat(Socket s) { return Native(s); }

#if defined(_WIN32)
std::string utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

bool net_ready() {
    static bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
std::string home() {
    const char* h = std::getenv("HOME");
    return h && *h ? h : "/tmp";
}

std::string xdg(const char* var, const std::string& fallback) {
    const char* v = std::getenv(var);
    return v && *v ? v : fallback;
}

bool net_ready() { return true; }
#endif

bool fill_addr(const std::string& path, sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    return true;
}

}  // namespace

std::string expand_home(const std::string& path) {
    if (path != "~" && path.rfind("~/", 0) != 0 && path.rfind("~\\", 0) != 0) return path;
    const char* value = std::getenv("HOME");
#if defined(_WIN32)
    if (!value || !*value) value = std::getenv("USERPROFILE");
#endif
    if (!value || !*value) return path;
    return std::string(value) + path.substr(1);
}

Socket listen_local(const std::string& path, std::string& error) {
    sockaddr_un addr;
    if (!net_ready() || !fill_addr(path, addr)) {
        error = "socket path too long: " + path;
        return kNoSocket;
    }
    remove_file(path);
    auto s = Socket(socket(AF_UNIX, SOCK_STREAM, 0));
    if (s == kNoSocket || bind(nat(s), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(nat(s), 16) != 0) {
        error = "cannot listen on " + path + ": " + std::strerror(errno);
        if (s != kNoSocket) close_socket(s);
        return kNoSocket;
    }
#if !defined(_WIN32)
    chmod(path.c_str(), 0600);
#endif
    return s;
}

Socket accept_local(Socket listener) {
    auto c = accept(nat(listener), nullptr, nullptr);
    return Socket(c);
}

Socket connect_local(const std::string& path) {
    sockaddr_un addr;
    if (!net_ready() || !fill_addr(path, addr)) return kNoSocket;
    auto s = Socket(socket(AF_UNIX, SOCK_STREAM, 0));
    if (s == kNoSocket) return kNoSocket;
    if (connect(nat(s), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close_socket(s);
        return kNoSocket;
    }
    return s;
}

bool send_all(Socket s, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
#if defined(_WIN32)
        int n = send(SOCKET(s), data.data() + done, int(data.size() - done), 0);
#else
        ssize_t n = send(int(s), data.data() + done, data.size() - done, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        done += size_t(n);
    }
    return true;
}

std::string recv_some(Socket s) {
    char buf[4096];
#if defined(_WIN32)
    int n = recv(SOCKET(s), buf, sizeof buf, 0);
#else
    ssize_t n;
    do n = recv(int(s), buf, sizeof buf, 0); while (n < 0 && errno == EINTR);
#endif
    return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

std::string recv_all(Socket s) {
    std::string out, chunk;
    while (!(chunk = recv_some(s)).empty()) out += chunk;
    return out;
}

void close_socket(Socket s) {
    if (s == kNoSocket) return;
#if defined(_WIN32)
    closesocket(SOCKET(s));
#else
    close(int(s));
#endif
}

bool replace_file(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

void remove_file(const std::string& path) { std::remove(path.c_str()); }

namespace {
const char* runtime_override() {
    const char* v = std::getenv("UMC_RUNTIME_DIR");  // lets tests run beside a real install
    return v && *v ? v : nullptr;
}
}  // namespace

#if defined(_WIN32)
std::string runtime_dir() {
    if (const char* o = runtime_override()) return o;
    char tmp[MAX_PATH + 1] = {};
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "umc";
}
std::string config_dir() { return env_or("APPDATA", "C:\\") + "\\umc"; }
std::string cache_dir() { return env_or("LOCALAPPDATA", "C:\\") + "\\umc\\cache"; }
std::string state_dir() { return env_or("LOCALAPPDATA", "C:\\") + "\\umc\\state"; }
#elif defined(__APPLE__)
// /tmp keeps socket paths short; Darwin's per-user TMPDIR is long enough to hit the sun_path limit.
std::string runtime_dir() { return runtime_override() ? runtime_override() : "/tmp/umc-" + std::to_string(getuid()); }
std::string config_dir() { return home() + "/Library/Application Support/umc"; }
std::string cache_dir() { return home() + "/Library/Caches/umc"; }
std::string state_dir() { return home() + "/Library/Application Support/umc/state"; }
#else
// /tmp rather than XDG_RUNTIME_DIR: the game may run without that variable or inside a sandbox.
std::string runtime_dir() { return runtime_override() ? runtime_override() : "/tmp/umc-" + std::to_string(getuid()); }
std::string config_dir() { return xdg("XDG_CONFIG_HOME", home() + "/.config") + "/umc"; }
std::string cache_dir() { return xdg("XDG_CACHE_HOME", home() + "/.cache") + "/umc"; }
std::string state_dir() { return xdg("XDG_STATE_HOME", home() + "/.local/state") + "/umc"; }
#endif

bool make_dirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/' && path[i] != '\\') continue;
        std::string part = path.substr(0, i);
        if (part.size() == 2 && part[1] == ':') continue;  // drive letter
#if defined(_WIN32)
        if (_mkdir(part.c_str()) != 0 && errno != EEXIST) return false;
#else
        if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
#endif
    }
    return true;
}

bool file_exists(const std::string& path) { return std::ifstream(path).good(); }

bool copy_file(const std::string& from, const std::string& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return false;
    out << in.rdbuf();
    return bool(out);
}

bool stdin_is_tty() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(0) != 0;
#endif
}

bool wait_readable(Socket s, int timeout_ms) {
#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(SOCKET(s), &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, &set, nullptr, nullptr, &tv) > 0;
#else
    pollfd p{int(s), POLLIN, 0};
    return poll(&p, 1, timeout_ms) > 0;
#endif
}

bool spawn_detached(const std::string& exe, const std::vector<std::string>& args) {
#if defined(_WIN32)
    std::string cmd = "\"" + exe + "\"";
    for (auto& a : args) cmd += " \"" + a + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);  // the child forks again and exits at once, so this returns quickly
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (fork() != 0) _exit(0);
    setsid();
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
    }
    std::vector<char*> argv{const_cast<char*>(exe.c_str())};
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(exe.c_str(), argv.data());
    _exit(127);
#endif
}

int current_pid() {
#if defined(_WIN32)
    return int(GetCurrentProcessId());
#else
    return int(getpid());
#endif
}

std::vector<std::string> self_args() {
    std::vector<std::string> args;
#if defined(_WIN32)
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 0; i < argc; ++i) args.push_back(utf8(argv[i]));
        LocalFree(argv);
    }
#elif defined(__APPLE__)
    char** argv = *_NSGetArgv();
    for (int i = 0; i < *_NSGetArgc(); ++i) args.push_back(argv[i]);
#else
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(in, arg, '\0')) args.push_back(arg);
#endif
    return args;
}

std::string self_exe() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return utf8(buf);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    char real[4096];
    return realpath(buf, real) ? real : buf;
#else
    char buf[4096] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, size_t(n)) : "";
#endif
}

}  // namespace umc::plat
