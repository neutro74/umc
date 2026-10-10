// Paths and the control socket protocol, shared by the CLI, the daemon and the agent.
#pragma once

#include <string>
#include <vector>

#include "common/platform.hpp"

namespace umc::shared {

inline std::string mappings_path(const std::string& version, const std::string& side) {
    return plat::join(plat::join(plat::join(plat::cache_dir(), "mappings"), version), side + ".txt");
}

// Request: one argument per line, ended by an empty line.
// Response: "ok\n" or "err\n" followed by free text, then the connection closes.
// Streaming requests (follow, subscribe) keep the connection open and send one line per event.
inline std::string encode_request(const std::vector<std::string>& args) {
    std::string s;
    for (auto& a : args) s += a + "\n";
    return s + "\n";
}

inline std::vector<std::string> decode_request(const std::string& data) {
    std::vector<std::string> args;
    size_t start = 0, end = data.find("\n\n");
    if (end == std::string::npos) end = data.size();
    while (start < end) {
        size_t nl = data.find('\n', start);
        if (nl == std::string::npos || nl > end) nl = end;
        args.push_back(data.substr(start, nl - start));
        start = nl + 1;
    }
    return args;
}

inline std::vector<std::string> read_request(plat::Socket s) {
    std::string data;
    while (data.size() < (1u << 20) && data.find("\n\n") == std::string::npos) {
        std::string chunk = plat::recv_some(s);
        if (chunk.empty()) break;
        data += chunk;
    }
    return decode_request(data);
}

// One-shot request. Returns false when nothing is listening.
inline bool request(const std::string& socket_path, const std::vector<std::string>& args, std::string& reply) {
    plat::Socket s = plat::connect_local(socket_path);
    if (s == plat::kNoSocket) return false;
    plat::send_all(s, encode_request(args));
    reply = plat::recv_all(s);
    plat::close_socket(s);
    return true;
}

}  // namespace umc::shared
