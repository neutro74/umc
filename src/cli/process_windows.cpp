#if defined(_WIN32)

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
#include <fstream>

#include "cli/process.hpp"
#include "common/platform.hpp"

namespace umc::cli {

namespace {

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

// Reads another process's command line (Windows 8.1+).
std::vector<std::string> process_args(DWORD pid) {
    using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto query = reinterpret_cast<QueryFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    std::vector<std::string> args;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h || !query) return args;
    constexpr ULONG kProcessCommandLineInformation = 60;
    ULONG size = 0;
    query(h, kProcessCommandLineInformation, nullptr, 0, &size);
    std::vector<unsigned char> buf(size ? size : 1);
    if (size && query(h, kProcessCommandLineInformation, buf.data(), size, &size) >= 0) {
        auto* us = reinterpret_cast<UNICODE_STRING*>(buf.data());
        std::wstring cmd(us->Buffer, us->Length / sizeof(wchar_t));
        int argc = 0;
        if (wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc)) {
            for (int i = 0; i < argc; ++i) args.push_back(utf8(argv[i]));
            LocalFree(argv);
        }
    }
    CloseHandle(h);
    return args;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace

bool read_process(int pid, GameProcess& out) {
    out = GameProcess{};
    out.pid = out.ns_pid = pid;
    out.args = process_args(DWORD(pid));
    if (out.args.empty()) return false;
    std::string exe = lower(out.args[0].substr(out.args[0].find_last_of("\\/") + 1));
    if (exe.rfind("java", 0) != 0) return false;
    classify(out);
    return true;
}

std::vector<GameProcess> java_processes() {
    std::vector<GameProcess> procs;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return procs;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        std::string exe = lower(utf8(e.szExeFile));
        if (exe != "java.exe" && exe != "javaw.exe") continue;
        GameProcess p;
        if (read_process(int(e.th32ProcessID), p)) procs.push_back(std::move(p));
    }
    CloseHandle(snap);
    std::sort(procs.begin(), procs.end(), [](auto& a, auto& b) { return a.pid < b.pid; });
    return procs;
}

bool process_alive(int pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

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

// Classic LoadLibraryW remote thread. The DLL's DllMain finds the JVM itself.
bool inject_library(const GameProcess& p, const std::string& library, const std::string& options,
                    std::string& message) {
    plat::make_dirs(plat::runtime_dir());
    std::ofstream(plat::join(plat::runtime_dir(), "inject-" + std::to_string(p.pid) + ".opts")) << options << "\n";

    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
                               PROCESS_QUERY_INFORMATION, FALSE, DWORD(p.pid));
    if (!h) {
        message = "cannot open process " + std::to_string(p.pid) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    BOOL target32 = FALSE, self32 = FALSE;
    IsWow64Process(h, &target32);
    IsWow64Process(GetCurrentProcess(), &self32);
    if (target32 != self32) {
        CloseHandle(h);
        message = "architecture mismatch: use a umc build matching the game's Java (32 vs 64 bit)";
        return false;
    }

    std::wstring path = wide(library);
    SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(h, remote, path.c_str(), bytes, nullptr);
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = ok ? CreateRemoteThread(h, nullptr, 0, load, remote, 0, nullptr) : nullptr;
    DWORD code = 0;
    if (thread) {
        WaitForSingleObject(thread, 15000);
        GetExitCodeThread(thread, &code);
        CloseHandle(thread);
    }
    if (remote) VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    if (!thread || code == 0) {
        message = "LoadLibrary failed inside the game (antivirus, or a missing dependency of umc.dll)";
        return false;
    }
    message = "injected";
    return true;
}

}  // namespace umc::cli

#endif
