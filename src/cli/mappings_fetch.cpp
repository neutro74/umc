// `umc mappings ...`: download Mojang's official mappings into the cache the CLI pushes to agents.
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "cli/inject.hpp"
#include "common/shared.hpp"

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace fs = std::filesystem;

namespace umc::cli {

namespace {

const char* kManifest = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

// Uses the system curl (bundled with Windows 10+ and macOS) to avoid a TLS dependency.
bool http_get(const std::string& url, std::string& body) {
    for (char c : url)
        if (!std::isalnum(static_cast<unsigned char>(c)) && std::string(":/._-?=&%").find(c) == std::string::npos)
            return false;
    FILE* f = popen(("curl -fsSL \"" + url + "\"").c_str(), "r");
    if (!f) return false;
    char buf[8192];
    size_t n;
    body.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n);
    return pclose(f) == 0;
}

std::string string_after(const std::string& json, size_t from, const std::string& key) {
    size_t k = json.find("\"" + key + "\"", from);
    if (k == std::string::npos) return "";
    size_t open = json.find('"', json.find(':', k));
    size_t close = json.find('"', open + 1);
    return json.substr(open + 1, close - open - 1);
}

void write_file(const std::string& path, const std::string& body) {
    plat::make_dirs(fs::path(path).parent_path().string());
    std::ofstream(path, std::ios::binary) << body;
}

}  // namespace

bool fetch_mappings(std::string version, bool& available, std::string& message) {
    available = false;
    std::string manifest;
    if (!http_get(kManifest, manifest)) {
        message = "could not download the version manifest (is curl installed and are you online?)";
        return false;
    }
    if (version == "latest" || version == "snapshot") {
        size_t latest = manifest.find("\"latest\"");
        version = string_after(manifest, latest, version == "latest" ? "release" : "snapshot");
    }
    size_t entry = manifest.find("\"id\": \"" + version + "\"");
    if (entry == std::string::npos) entry = manifest.find("\"id\":\"" + version + "\"");
    if (entry == std::string::npos) {
        message = "unknown version " + version;
        return false;
    }
    std::string meta;
    if (!http_get(string_after(manifest, entry, "url"), meta)) {
        message = "could not download metadata for " + version;
        return false;
    }

    message.clear();
    for (const char* side : {"client", "server"}) {
        std::string path = shared::mappings_path(version, side);
        size_t at = meta.find("\"" + std::string(side) + "_mappings\"");
        if (at == std::string::npos) {
            write_file(path + ".none", "");  // remember there's nothing to fetch
            continue;
        }
        std::string body;
        if (!http_get(string_after(meta, at, "url"), body)) {
            message = "download failed for " + std::string(side) + " mappings";
            return false;
        }
        write_file(path, body);
        message += std::string(side) + " -> " + path + "\n";
        available = true;
    }
    if (!available) message = version + " ships no obfuscation mappings, so UMC uses its names directly.\n";
    return true;
}

int mappings_command(const std::vector<std::string>& args) {
    if (args.size() >= 2 && args[0] == "fetch") {
        bool available = false;
        std::string message;
        bool ok = fetch_mappings(args[1], available, message);
        (ok ? std::cout : std::cerr) << message << (ok ? "" : "\n");
        return ok ? 0 : 1;
    }
    if (args.size() >= 4 && args[0] == "import") {
        std::string path = shared::mappings_path(args[1], args[2]);
        plat::make_dirs(fs::path(path).parent_path().string());
        if (!plat::copy_file(args[3], path)) {
            std::cerr << "cannot import " << args[3] << "\n";
            return 1;
        }
        std::cout << "imported " << args[3] << " -> " << path << "\n";
        return 0;
    }
    if (!args.empty() && args[0] == "list") {
        std::error_code ec;
        fs::path root = fs::path(plat::cache_dir()) / "mappings";
        bool any = false;
        for (auto& v : fs::directory_iterator(root, ec)) {
            std::string line = v.path().filename().string() + ":";
            for (const char* side : {"client", "server"}) {
                if (fs::exists(v.path() / (std::string(side) + ".txt"))) line += std::string(" ") + side;
                else if (fs::exists(v.path() / (std::string(side) + ".txt.none"))) line += std::string(" ") + side + "(unobfuscated)";
            }
            std::cout << line << "\n";
            any = true;
        }
        if (!any) std::cout << "no mappings cached\n";
        return 0;
    }
    std::cerr << "usage:\n"
                 "  umc mappings fetch <version|latest|snapshot>\n"
                 "  umc mappings import <version> <client|server> <proguard.txt>\n"
                 "  umc mappings list\n";
    return 2;
}

}  // namespace umc::cli
