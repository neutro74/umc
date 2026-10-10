#include "common/platform.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <afunix.h>
#include <direct.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/socket.h>
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

void remove_file(const std::string& path) { std::remove(path.c_str()); }

#if defined(_WIN32)
std::string runtime_dir() {
    char tmp[MAX_PATH + 1] = {};
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + "umc";
}
std::string config_dir() { return env_or("APPDATA", "C:\\") + "\\umc"; }
std::string cache_dir() { return env_or("LOCALAPPDATA", "C:\\") + "\\umc\\cache"; }
std::string state_dir() { return env_or("LOCALAPPDATA", "C:\\") + "\\umc\\state"; }
#elif defined(__APPLE__)
// /tmp keeps socket paths short; Darwin's per-user TMPDIR is long enough to hit the sun_path limit.
std::string runtime_dir() { return "/tmp/umc-" + std::to_string(getuid()); }
std::string config_dir() { return home() + "/Library/Application Support/umc"; }
std::string cache_dir() { return home() + "/Library/Caches/umc"; }
std::string state_dir() { return home() + "/Library/Application Support/umc/state"; }
#else
// /tmp rather than XDG_RUNTIME_DIR: the game may run without that variable or inside a sandbox.
std::string runtime_dir() { return "/tmp/umc-" + std::to_string(getuid()); }
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

void* lib_open(const std::string& path, std::string& error) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) error = "LoadLibrary failed for " + path + " (error " + std::to_string(GetLastError()) + ")";
    return reinterpret_cast<void*>(h);
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) error = dlerror();
    return h;
#endif
}

void* lib_symbol(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void lib_close(void* lib) {
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

}  // namespace umc::plat
