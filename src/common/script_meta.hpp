// The metadata header at the top of a UMC script plugin:
//
//   // ==UMC==
//   // @name         Coords HUD
//   // @id           coords-hud
//   // @version      1.0.0
//   // @author       someone
//   // @description  Shows your position on screen.
//   // @permissions  player render
//   // ==/UMC==
#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace umc {

struct ScriptMeta {
    bool has_header = false;
    std::string id, name, version, author, description;
    std::vector<std::string> permissions;
};

// What each permission lets a script do. Anything not listed here is not available to scripts.
inline const std::vector<std::pair<std::string, std::string>>& known_permissions() {
    static const std::vector<std::pair<std::string, std::string>> list = {
        {"player", "read your position, rotation, health and name"},
        {"players", "read the position and name of other players near you"},
        {"chat", "show messages in your chat window"},
        {"render", "draw on your screen (the HUD)"},
        {"storage", "keep small private files in its own folder (it can't see anything else on your computer)"},
    };
    return list;
}

inline bool is_known_permission(const std::string& p) {
    for (auto& [name, text] : known_permissions())
        if (name == p) return true;
    return false;
}

inline std::string sanitize_id(std::string s) {
    for (auto& c : s) c = std::isalnum(static_cast<unsigned char>(c)) ? char(std::tolower(static_cast<unsigned char>(c))) : '-';
    while (!s.empty() && s.front() == '-') s.erase(s.begin());
    while (!s.empty() && s.back() == '-') s.pop_back();
    return s;
}

inline ScriptMeta parse_script_meta(const std::string& source, const std::string& fallback_name = "") {
    ScriptMeta meta;
    std::istringstream in(source);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("// ==UMC==", 0) == 0) {
            inside = meta.has_header = true;
            continue;
        }
        if (line.rfind("// ==/UMC==", 0) == 0) break;
        if (!inside) {
            if (line.empty() || line.rfind("//", 0) == 0 || line.rfind("/*", 0) == 0) continue;
            break;  // code before any header: no metadata
        }
        size_t at = line.find('@');
        if (at == std::string::npos) continue;
        size_t end = line.find_first_of(" \t", at);
        std::string key = line.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
        std::string value = end == std::string::npos ? "" : line.substr(line.find_first_not_of(" \t", end) == std::string::npos ? line.size() : line.find_first_not_of(" \t", end));
        if (key == "name") meta.name = value;
        else if (key == "id") meta.id = value;
        else if (key == "version") meta.version = value;
        else if (key == "author") meta.author = value;
        else if (key == "description") meta.description = value;
        else if (key == "permissions") {
            std::istringstream ps(value);
            std::string p;
            while (ps >> p) meta.permissions.push_back(p);
        }
    }
    if (meta.name.empty()) meta.name = fallback_name;
    if (meta.id.empty()) meta.id = sanitize_id(meta.name.empty() ? fallback_name : meta.name);
    return meta;
}

}  // namespace umc
