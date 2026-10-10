// Builds mappings for Minecraft versions before 1.14.4, which have no official Mojang mappings.
// The names come from MCP (the community mappings for 1.7 to 1.12): the SRG archive links the game's
// obfuscated names to stable SRG names, and the MCP CSVs give readable names for those.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "cli/inject.hpp"
#include "common/shared.hpp"
#include "miniz.h"

namespace fs = std::filesystem;

namespace umc::cli {

namespace {

struct LegacyVersion {
    const char* version;
    const char* srg;    // mcp/<srg>/mcp-<srg>-srg.zip
    const char* stable; // mcp_stable/<stable>/mcp_stable-<stable>.zip
};

const LegacyVersion kLegacy[] = {
    {"1.7.10", "1.7.10", "12-1.7.10"}, {"1.8", "1.8", "18-1.8"},       {"1.8.8", "1.8.8", "20-1.8.8"},
    {"1.8.9", "1.8.9", "22-1.8.9"},    {"1.9", "1.9", "24-1.9"},       {"1.9.4", "1.9.4", "26-1.9.4"},
    {"1.10.2", "1.10.2", "29-1.10.2"}, {"1.11", "1.11", "32-1.11"},    {"1.11.2", "1.11.2", "32-1.11"},
    {"1.12", "1.12", "39-1.12"},       {"1.12.1", "1.12.1", "39-1.12"}, {"1.12.2", "1.12.2", "39-1.12"},
};

const char* kMaven = "https://maven.minecraftforge.net/de/oceanlabs/mcp";

bool download(const std::string& url, const std::string& path) {
    plat::make_dirs(fs::path(path).parent_path().string());
    return std::system(("curl -fsSL -o \"" + path + "\" \"" + url + "\"").c_str()) == 0;
}

bool read_entry(const std::string& zip_path, const char* name, std::string& out) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zip_path.c_str(), 0)) return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(&zip, name, &size, 0);
    bool ok = data != nullptr;
    if (ok) out.assign(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_reader_end(&zip);
    return ok;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream in(s);
    while (std::getline(in, cur, sep)) out.push_back(cur);
    return out;
}

std::string dots(std::string s) {
    for (auto& c : s) if (c == '/') c = '.';
    return s;
}

struct Member {
    std::string name, obf, desc;  // desc only for methods (named form)
    bool method;
};

}  // namespace

bool is_legacy_version(const std::string& version) {
    for (auto& v : kLegacy) if (version == v.version) return true;
    return false;
}

bool build_legacy_mappings(const std::string& version, std::string& message) {
    const LegacyVersion* info = nullptr;
    for (auto& v : kLegacy) if (version == v.version) info = &v;
    if (!info) {
        message = "no MCP mappings are known for " + version;
        return false;
    }
    std::string tmp = plat::join(plat::cache_dir(), "downloads");
    std::string srg_zip = plat::join(tmp, std::string("mcp-") + info->srg + "-srg.zip");
    std::string csv_zip = plat::join(tmp, std::string("mcp_stable-") + info->stable + ".zip");
    if (!plat::file_exists(srg_zip) &&
        !download(std::string(kMaven) + "/mcp/" + info->srg + "/mcp-" + info->srg + "-srg.zip", srg_zip)) {
        message = "could not download the SRG mappings for " + version;
        return false;
    }
    if (!plat::file_exists(csv_zip) &&
        !download(std::string(kMaven) + "/mcp_stable/" + info->stable + "/mcp_stable-" + info->stable + ".zip", csv_zip)) {
        message = "could not download the MCP names for " + version;
        return false;
    }

    std::string srg_text, fields_csv, methods_csv;
    if (!read_entry(srg_zip, "joined.srg", srg_text) || !read_entry(csv_zip, "fields.csv", fields_csv) ||
        !read_entry(csv_zip, "methods.csv", methods_csv)) {
        message = "the downloaded MCP archives are not in the expected format";
        return false;
    }

    std::map<std::string, std::string> readable;  // SRG name -> MCP name
    for (const std::string* csv : {&fields_csv, &methods_csv}) {
        std::istringstream in(*csv);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            size_t a = line.find(','), b = line.find(',', a + 1);
            if (a != std::string::npos && b != std::string::npos) readable[line.substr(0, a)] = line.substr(a + 1, b - a - 1);
        }
    }
    auto readable_name = [&](const std::string& srg) {
        auto it = readable.find(srg);
        return it == readable.end() ? srg : it->second;
    };

    std::map<std::string, std::string> class_obf;               // named class -> obf class
    std::map<std::string, std::vector<Member>> members;         // named class -> members
    std::istringstream in(srg_text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto parts = split(line, ' ');
        if (parts.size() >= 3 && parts[0] == "CL:") {
            class_obf[parts[2]] = parts[1];
        } else if (parts.size() >= 3 && parts[0] == "FD:") {
            size_t o = parts[1].rfind('/'), n = parts[2].rfind('/');
            if (o == std::string::npos || n == std::string::npos) continue;
            members[parts[2].substr(0, n)].push_back({readable_name(parts[2].substr(n + 1)), parts[1].substr(o + 1), "", false});
        } else if (parts.size() >= 5 && parts[0] == "MD:") {
            size_t o = parts[1].rfind('/'), n = parts[3].rfind('/');
            if (o == std::string::npos || n == std::string::npos) continue;
            members[parts[3].substr(0, n)].push_back({readable_name(parts[3].substr(n + 1)), parts[1].substr(o + 1), parts[4], true});
        }
    }
    if (class_obf.empty()) {
        message = "the SRG mappings contain no classes";
        return false;
    }

    std::ostringstream out;
    out << "# Generated by UMC from MCP data for Minecraft " << version << ".\n";
    for (auto& [named, obf] : class_obf) {
        out << dots(named) << " -> " << dots(obf) << ":\n";
        std::set<std::string> seen;
        for (auto& m : members[named]) {
            std::string key = std::string(m.method ? "m" : "f") + m.name + m.desc;
            if (!seen.insert(key).second) continue;
            if (m.method) out << "    !m " << m.desc << " " << m.name << " -> " << m.obf << "\n";
            else out << "    !f " << m.name << " -> " << m.obf << "\n";
        }
    }

    for (const char* side : {"client", "server"}) {
        std::string path = shared::mappings_path(version, side);
        plat::make_dirs(fs::path(path).parent_path().string());
        std::ofstream(path, std::ios::binary) << out.str();
        message += std::string(side) + " -> " + path + "\n";
    }
    return true;
}

}  // namespace umc::cli
