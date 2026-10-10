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
    std::vector<std::pair<std::string, std::string>> dependencies;
};

// What each permission lets a script do. Anything not listed here is not available to scripts.
inline const std::vector<std::pair<std::string, std::string>>& known_permissions() {
    static const std::vector<std::pair<std::string, std::string>> list = {
        {"player", "read your position, rotation, health and name"},
        {"players", "read the position and name of other players near you"},
        {"chat", "read or cancel chat messages and display local messages"},
        {"combat", "see the things you attack, and stop attacks from happening"},
        {"render", "draw on your screen (the HUD)"},
        {"storage", "keep small private files in its own folder (it can't see anything else on your computer)"},
        {"hooks", "intercept Minecraft methods, inspect plain arguments and replace their results"},
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

}  // namespace umc
