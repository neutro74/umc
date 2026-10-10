// OS abstraction shared by the agent and the CLI: local sockets, paths, libraries, process info.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace umc::plat {

#if defined(_WIN32)
constexpr const char* kLibExt = ".dll";
constexpr char kSep = '\\';
#elif defined(__APPLE__)
constexpr const char* kLibExt = ".dylib";
constexpr char kSep = '/';
#else
constexpr const char* kLibExt = ".so";
constexpr char kSep = '/';
#endif

// Local stream sockets (AF_UNIX everywhere, including Windows 10+).
using Socket = std::intptr_t;
constexpr Socket kNoSocket = -1;

Socket listen_local(const std::string& path, std::string& error);
Socket accept_local(Socket listener);
Socket connect_local(const std::string& path);
bool send_all(Socket s, const std::string& data);
// Reads whatever is available, blocking until at least one byte or EOF. Empty means closed.
std::string recv_some(Socket s);
std::string recv_all(Socket s);
void close_socket(Socket s);
void remove_file(const std::string& path);

std::string runtime_dir();  // sockets and staged files
std::string config_dir();
std::string cache_dir();
std::string state_dir();    // logs, history
bool make_dirs(const std::string& path);
bool file_exists(const std::string& path);
bool copy_file(const std::string& from, const std::string& to);

int current_pid();
std::vector<std::string> self_args();
std::string self_exe();

void* lib_open(const std::string& path, std::string& error);
void* lib_symbol(void* lib, const char* name);
void lib_close(void* lib);

inline std::string join(const std::string& a, const std::string& b) { return a + kSep + b; }
inline std::string agent_socket_name(int pid) { return std::to_string(pid) + ".sock"; }
inline std::string daemon_socket() { return join(runtime_dir(), "daemon.sock"); }

}  // namespace umc::plat
