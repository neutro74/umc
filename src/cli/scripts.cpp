#include "cli/scripts.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include "common/language.hpp"
#include "common/sha256.hpp"
#include "common/shared.hpp"
namespace fs = std::filesystem;
namespace umc::cli {
namespace {
std::string scripts_dir() { return plat::join(plat::config_dir(), "scripts"); }
bool is_url(const std::string& s) { return s.rfind("https://", 0) == 0; }
bool fetch(const std::string& source, std::string& body, std::string& error) {
    if (!is_url(source)) {
        if (source.find("://") != std::string::npos) { error = "downloads require HTTPS"; return false; }
        std::ifstream in(plat::expand_home(source), std::ios::binary);
        if (!in) { error = "cannot read " + source; return false; }
        body.resize(lang::max_source_bytes + 1); in.read(body.data(), std::streamsize(body.size())); body.resize(size_t(in.gcount()));
    } else {
        for (unsigned char c : source) if (!std::isalnum(c) && std::string(":/._-?=&%~+#@").find(c) == std::string::npos) { error = "invalid download URL"; return false; }
        FILE* f = popen(("curl -fsSL --proto '=https' --proto-redir '=https' --max-time 30 --max-filesize 262144 \"" + source + "\"").c_str(), "r");
        if (!f) { error = "cannot start downloader"; return false; }
        char buf[4096]; size_t n; body.clear();
        while (body.size() <= lang::max_source_bytes && (n = fread(buf, 1, sizeof buf, f))) body.append(buf, n);
        if (pclose(f)) { error = "download failed: " + source; return false; }
    }
    if (body.size() > lang::max_source_bytes) { error = "file exceeds 256 KB"; return false; }
    return true;
}
std::string join(const std::vector<std::string>& values, const char* sep) {
    std::string out; for (size_t i = 0; i < values.size(); ++i) out += (i ? sep : "") + values[i]; return out;
}
std::vector<std::string> read_grants(const fs::path& path) {
    std::ifstream in(path); std::vector<std::string> out; std::string line;
    while (std::getline(in, line)) if (is_known_permission(line)) out.push_back(line);
    return out;
}
struct Package { std::string source, body; lang::Compiled compiled; };
struct Entry { std::string hash, url, name, description, owner; };
std::string registry_location() {
    if (const char* value = std::getenv("UMC_REGISTRY")) return plat::expand_home(value);
    return "https://raw.githubusercontent.com/neutro74/umc-registry/main/index.tsv";
}
std::string decode_field(const std::string& value) {
    if (value == "-") return "";
    std::string out;
    auto hex = [](char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char ch = value[i];
        if (ch == '%' && i + 2 < value.size() && hex(value[i+1]) >= 0 && hex(value[i+2]) >= 0) { ch = unsigned(hex(value[i+1]) * 16 + hex(value[i+2])); i += 2; }
        if (ch >= 32 && ch != 127) out += char(ch);
    }
    return out;
}
std::string lower(std::string value) { for (auto& c : value) c = char(std::tolower(static_cast<unsigned char>(c))); return value; }

using Index = std::map<std::pair<std::string, std::string>, Entry>;
bool read_index(const std::string& location, Index& index, std::string& error) {
    std::string body; if (!fetch(location, body, error)) return false;
    std::istringstream in(body); std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line); std::string id, version, hash, url, extra;
        if (!(row >> id >> version >> hash >> url) || id.empty() || id.size() > 64 || sanitize_id(id) != id || version.empty() || !std::isalnum(static_cast<unsigned char>(version[0])) || version.find("..") != std::string::npos || version.size() > 32 || version.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-") != std::string::npos || hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) { error = "invalid registry entry"; return false; }
        Entry entry{hash, url, "", "", ""};
        if (row >> extra) {
            entry.name = decode_field(extra);
            if (!(row >> extra)) { error = "invalid registry metadata"; return false; } entry.description = decode_field(extra);
            if (!(row >> extra)) { error = "invalid registry metadata"; return false; } entry.owner = decode_field(extra);
            if (row >> extra) { error = "invalid registry entry"; return false; }
        }
        if (!is_url(url)) {
            if (is_url(location) || fs::path(url).is_absolute() || url.find("..") != std::string::npos) { error = "registry entries require HTTPS"; return false; }
            url = (fs::path(plat::expand_home(location)).parent_path() / url).string();
        }
        entry.url = url;
        if (!index.emplace(std::make_pair(id, version), std::move(entry)).second) { error = "duplicate registry version"; return false; }
    }
    return true;
}
int browse_registry(const std::string& cmd, const std::vector<std::string>& args) {
    std::string location = registry_location(); std::vector<std::string> query;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--registry") {
            if (++i == args.size()) { std::cerr << "--registry needs an index URL or file\n"; return 2; }
            location = args[i];
        } else query.push_back(args[i]);
    }
    if (cmd != "search" && query.size() != 1) { std::cerr << "usage: umc " << cmd << " <id" << (cmd == "info" ? "@version" : "") << "> [--registry URL]\n"; return 2; }
    Index index; std::string error;
    if (!read_index(location, index, error)) { std::cerr << error << '\n'; return 1; }
    if (cmd == "search") {
        const std::string term = lower(join(query, " ")); size_t count = 0;
        for (const auto& [key, entry] : index) if (term.empty() || lower(key.first + " " + entry.name + " " + entry.description + " " + entry.owner).find(term) != std::string::npos) {
            std::cout << key.first << '@' << key.second;
            if (!entry.name.empty()) std::cout << "  " << entry.name;
            if (!entry.owner.empty()) std::cout << "  by " << entry.owner;
            std::cout << '\n'; if (!entry.description.empty()) std::cout << "  " << entry.description << '\n'; ++count;
        }
        if (!count) std::cout << "no matching plugins\n";
        return 0;
    }
    if (cmd == "versions") {
        size_t count = 0;
        for (const auto& [key, entry] : index) if (key.first == query[0]) { std::cout << key.first << '@' << key.second << '\n'; ++count; }
        if (!count) { std::cerr << "plugin is not in the registry\n"; return 1; }
        return 0;
    }
    const auto split = query[0].find('@'); std::string id = query[0].substr(0, split);
    std::vector<Index::const_iterator> matches;
    for (auto it = index.begin(); it != index.end(); ++it) if (it->first.first == id && (split == std::string::npos || it->first.second == query[0].substr(split + 1))) matches.push_back(it);
    if (matches.empty()) { std::cerr << "plugin version is not in the registry\n"; return 1; }
    if (matches.size() != 1) { std::cerr << "choose an exact version (umc versions " << id << ")\n"; return 2; }
    const auto& [key, entry] = *matches[0]; std::string body; lang::Compiled compiled;
    if (!fetch(entry.url, body, error) || !lang::compile(body, compiled, error)) { std::cerr << error << '\n'; return 1; }
    if (compiled.meta.id != key.first || compiled.meta.version != key.second || sha256(body) != entry.hash) { std::cerr << "registry identity or SHA-256 mismatch\n"; return 1; }
    const auto& meta = compiled.meta;
    std::cout << meta.name << " (" << meta.id << '@' << meta.version << ")\n";
    if (!meta.description.empty()) std::cout << meta.description << '\n';
    std::cout << "source: " << entry.url << "\nSHA-256: " << entry.hash << "\npermissions: " << (meta.permissions.empty() ? "none" : join(meta.permissions, ", ")) << '\n';
    for (const auto& dep : meta.dependencies) std::cout << "depends: " << dep.first << '@' << dep.second << '\n';
    std::cout << "install: umc install " << meta.id << '@' << meta.version << '\n'; return 0;
}
int install(const std::vector<std::string>& args, const GameProcess* running) {
    bool yes = false; std::string source, registry = registry_location();
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--yes" || args[i] == "-y") yes = true;
        else if (args[i] == "--registry" && i + 1 < args.size()) registry = args[++i];
        else source = args[i];
    }
    if (source.empty()) { std::cerr << "usage: umc install <file.umc|URL|id@version> [--registry URL] [--yes]\n"; return 2; }
    Index index; std::map<std::string, Package> packages; std::set<std::string> visiting; std::vector<std::string> order; std::string error;
    auto index_once = [&]() { return !index.empty() || read_index(registry, index, error); };
    std::function<bool(std::string, std::string, std::string, std::string, int)> resolve;
    resolve = [&](std::string location, std::string wanted_id, std::string version, std::string expected, int depth) {
        if (depth > 16 || packages.size() >= 32) { error = "dependency limit exceeded"; return false; }
        if (!wanted_id.empty() && visiting.count(wanted_id)) { error = "dependency cycle: " + wanted_id; return false; }
        if (!wanted_id.empty() && packages.count(wanted_id)) {
            if (packages.at(wanted_id).compiled.meta.version != version) { error = "conflicting dependency versions"; return false; }
            return true;
        }
        Package p; p.source = location;
        if (!fetch(location, p.body, error) || !lang::compile(p.body, p.compiled, error)) return false;
        auto meta = p.compiled.meta;
        if ((!wanted_id.empty() && (meta.id != wanted_id || meta.version != version)) || (!expected.empty() && sha256(p.body) != expected)) { error = "dependency identity or SHA-256 mismatch"; return false; }
        if (visiting.count(meta.id)) { error = "dependency cycle"; return false; }
        visiting.insert(meta.id); packages.emplace(meta.id, std::move(p));
        for (const auto& dep : meta.dependencies) {
            if (!index_once()) return false;
            auto entry = index.find(dep);
            if (entry == index.end()) { error = "dependency not in registry: " + dep.first + " " + dep.second; return false; }
            if (!resolve(entry->second.url, dep.first, dep.second, entry->second.hash, depth + 1)) return false;
            for (const auto& permission : packages.at(dep.first).compiled.meta.permissions)
                if (std::find(meta.permissions.begin(), meta.permissions.end(), permission) == meta.permissions.end()) { error = meta.id + " must declare dependency capability: " + permission; return false; }
        }
        visiting.erase(meta.id); order.push_back(meta.id); return true;
    };
    auto split = source.find('@');
    bool bare_id = sanitize_id(source) == source && !plat::file_exists(plat::expand_home(source));
    bool catalog = (split != std::string::npos || bare_id) && source.find('/') == std::string::npos && source.find('\\') == std::string::npos;
    bool ok;
    if (catalog) {
        if (!index_once()) { std::cerr << error << '\n'; return 1; }
        if (bare_id) {
            std::vector<std::string> versions;
            for (const auto& [key, entry] : index) if (key.first == source) versions.push_back(key.second);
            if (versions.size() != 1) { std::cerr << (versions.empty() ? "plugin is not in the registry" : "choose an exact version (umc versions " + source + ")") << '\n'; return 1; }
            source += "@" + versions[0]; split = source.find('@');
        }
        auto entry = index.find({source.substr(0, split), source.substr(split + 1)});
        if (entry == index.end()) { std::cerr << "plugin version is not in the registry\n"; return 1; }
        ok = resolve(entry->second.url, source.substr(0, split), source.substr(split + 1), entry->second.hash, 0);
    } else ok = resolve(source, "", "", "", 0);
    if (!ok) { std::cerr << error << '\n'; return 1; }
    // One installed version per ID: do not silently break another installed dependent.
    std::error_code inspect_error;
    for (const auto& file : fs::directory_iterator(scripts_dir(), inspect_error)) if (file.path().extension() == ".umc") {
        std::string body, existing_error; lang::Compiled existing;
        if (!fetch(file.path().string(), body, existing_error) || !lang::compile(body, existing, existing_error) || packages.count(existing.meta.id)) continue;
        for (const auto& dep : existing.meta.dependencies) if (packages.count(dep.first) && packages.at(dep.first).compiled.meta.version != dep.second) {
            std::cerr << "installed " << existing.meta.id << " requires " << dep.first << '@' << dep.second << "; uninstall it before changing that dependency\n"; return 1;
        }
    }
    for (const auto& id : order) {
        const auto& p = packages.at(id); const auto& meta = p.compiled.meta;
        std::cout << id << "  v" << meta.version << "  " << meta.name << "\n  source: " << p.source << "\n  SHA-256: " << sha256(p.body) << "\n";
        for (const auto& permission : meta.permissions) for (const auto& [name, text] : known_permissions()) if (name == permission) std::cout << "  - " << text << " (" << name << ")\n";
        if (meta.permissions.empty()) std::cout << "  no game permissions\n";
    }
    std::cout << "All plugins are sandboxed. Downloads happen only during installation.\n";
    if (!yes) { std::cout << "Install " << order.size() << " plugin(s)? [y/N] " << std::flush; std::string reply; std::getline(std::cin, reply); if (reply != "y" && reply != "Y") return 1; }
    std::error_code ec; fs::create_directories(scripts_dir(), ec);
    if (ec) { std::cerr << "cannot create plugin directory\n"; return 1; }
    // Validate everything before making any installed package visible.
    std::vector<std::pair<fs::path, fs::path>> staged;
    for (const auto& id : order) {
        const auto& p = packages.at(id);
        for (const auto& [ext, data] : std::vector<std::pair<std::string, std::string>>{{".umc", p.body}, {".grants", join(p.compiled.meta.permissions, "\n") + "\n"}, {".sha256", sha256(p.body)}}) {
            fs::path destination = fs::path(scripts_dir()) / (id + ext), temporary = destination.string() + ".pending";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out.write(data.data(), std::streamsize(data.size())); out.close();
            if (!out) { std::cerr << "cannot stage plugin\n"; for (const auto& [tmp, dest] : staged) fs::remove(tmp, ec); return 1; }
            staged.emplace_back(temporary, destination);
        }
    }
    for (const auto& [temporary, destination] : staged) {
        if (!plat::replace_file(temporary.string(), destination.string())) { std::cerr << "cannot publish installed plugin\n"; return 1; }
    }
    for (const auto& id : order) std::cout << "installed " << id << '\n';
    if (running) { std::vector<std::string> log; load_installed_scripts(*running, log); for (const auto& line : log) std::cout << line << '\n'; }
    return 0;
}
int uninstall(const std::vector<std::string>& args, const GameProcess* running) {
    if (args.size() != 2 || sanitize_id(args[1]) != args[1]) { std::cerr << "usage: umc uninstall <id>\n"; return 2; }
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(scripts_dir(), ec)) if (file.path().extension() == ".umc") {
        std::string body, error; lang::Compiled c;
        if (fetch(file.path().string(), body, error) && lang::compile(body, c, error)) for (const auto& dep : c.meta.dependencies)
            if (dep.first == args[1]) { std::cerr << "plugin is required by " << c.meta.id << '\n'; return 1; }
    }
    if (running) { std::string reply; agent_request(*running, {"script", "unload", args[1]}, reply); if (reply.find("required by") != std::string::npos) { std::cerr << reply; return 1; } }
    fs::path base = fs::path(scripts_dir()) / args[1];
    if (!fs::remove(base.string() + ".umc", ec)) { std::cerr << "no installed plugin named " << args[1] << '\n'; return 1; }
    fs::remove(base.string() + ".grants", ec); fs::remove(base.string() + ".sha256", ec);
    std::cout << "uninstalled " << args[1] << '\n'; return 0;
}
int list_installed() {
    std::error_code ec; bool any = false;
    for (const auto& file : fs::directory_iterator(scripts_dir(), ec)) if (file.path().extension() == ".umc") {
        std::string body, error; lang::Compiled c;
        if (!fetch(file.path().string(), body, error) || !lang::compile(body, c, error)) { std::cerr << file.path().filename() << ": " << error << '\n'; continue; }
        std::cout << c.meta.id << "  " << c.meta.name << "  v" << c.meta.version << "  [" << join(read_grants(fs::path(file.path()).replace_extension(".grants")), " ") << "]\n"; any = true;
    }
    if (!any) std::cout << "no plugins installed\n";
    return 0;
}
}
int load_local_plugin(const GameProcess& game, const std::vector<std::string>& args) {
    std::string existing, error;
    if (!agent_request(game, {"script", "list"}, existing) || existing.rfind("ok\n", 0) != 0) {
        std::cerr << "cannot list loaded plugins\n"; return 1;
    }
    std::map<std::string, std::string> loaded;
    std::istringstream rows(existing.substr(3)); std::string row;
    while (std::getline(rows, row)) {
        std::istringstream fields(row); std::string id, version; fields >> id >> version;
        if (!version.empty() && version[0] == 'v') loaded[id] = version.substr(1);
    }
    std::map<std::string, Package> packages;
    std::set<std::string> visiting; std::vector<std::string> order;
    std::function<bool(fs::path, std::string, std::string, int)> resolve;
    resolve = [&](fs::path path, std::string wanted, std::string version, int depth) {
        if (depth > 16 || packages.size() >= 32) { error = "dependency limit exceeded"; return false; }
        if (visiting.count(wanted)) { error = "dependency cycle: " + wanted; return false; }
        if (!wanted.empty() && loaded.count(wanted)) {
            if (loaded.at(wanted) == version) return true;
            error = "loaded dependency has another version: " + wanted; return false;
        }
        if (!wanted.empty() && packages.count(wanted)) {
            if (packages.at(wanted).compiled.meta.version == version) return true;
            error = "conflicting dependency versions: " + wanted; return false;
        }
        if (path.extension() != ".umc") { error = "only .umc plugins are supported"; return false; }
        Package p; p.source = path.string();
        if (!fetch(p.source, p.body, error) || !lang::compile(p.body, p.compiled, error)) return false;
        auto meta = p.compiled.meta;
        if (!wanted.empty() && (meta.id != wanted || meta.version != version)) { error = "dependency identity/version mismatch: " + wanted; return false; }
        if (loaded.count(meta.id)) { error = "plugin already loaded: " + meta.id; return false; }
        if (!visiting.insert(meta.id).second) { error = "dependency cycle: " + meta.id; return false; }
        packages.emplace(meta.id, std::move(p));
        for (const auto& dep : meta.dependencies) {
            if (!resolve(path.parent_path() / (dep.first + ".umc"), dep.first, dep.second, depth + 1)) {
                error += " (required by " + meta.id + "; use install for registry dependencies)"; return false;
            }
            if (packages.count(dep.first)) for (const auto& permission : packages.at(dep.first).compiled.meta.permissions)
                if (std::find(meta.permissions.begin(), meta.permissions.end(), permission) == meta.permissions.end()) { error = meta.id + " must declare dependency capability: " + permission; return false; }
        }
        visiting.erase(meta.id); order.push_back(meta.id); return true;
    };
    if (!resolve(fs::absolute(plat::expand_home(args[2])), "", "", 0)) { std::cerr << error << '\n'; return 1; }
    // Stage the complete, validated graph before executing any plugin.
    std::map<std::string, std::string> staged;
    for (const auto& id : order) {
        staged[id] = stage_file(game, packages.at(id).source, error);
        if (staged[id].empty()) { std::cerr << error << '\n'; return 1; }
    }
    std::vector<std::string> started;
    for (const auto& id : order) {
        std::vector<std::string> request{"script", "load", staged.at(id)};
        const auto& meta = packages.at(id).compiled.meta;
        request.push_back(args.size() > 3 ? args[3] : meta.permissions.empty() ? "none" : join(meta.permissions, ","));
        request.push_back(packages.at(id).source);
        std::string reply;
        if (!agent_request(game, request, reply) || reply.rfind("ok\n", 0) != 0) {
            std::cerr << "could not load " << id << ": " << reply << '\n';
            for (auto it = started.rbegin(); it != started.rend(); ++it) { std::string ignored; agent_request(game, {"script", "unload", *it}, ignored); }
            return 1;
        }
        started.push_back(id); std::cout << "loaded " << id << '\n';
    }
    return 0;
}
void load_installed_scripts(const GameProcess& game, std::vector<std::string>& log) {
    std::error_code ec; std::map<std::string, Package> packages;
    for (const auto& file : fs::directory_iterator(scripts_dir(), ec)) if (file.path().extension() == ".umc") {
        Package p; std::string error; p.source = file.path().string();
        if (!fetch(p.source, p.body, error) || !lang::compile(p.body, p.compiled, error)) { log.push_back(error); continue; }
        std::ifstream hashes(fs::path(file.path()).replace_extension(".sha256")); std::string hash; hashes >> hash;
        if (hash != sha256(p.body)) { log.push_back("installed hash mismatch: " + p.compiled.meta.id); continue; }
        packages[p.compiled.meta.id] = std::move(p);
    }
    std::set<std::string> loaded, visiting;
    std::string existing; if (agent_request(game, {"script", "list"}, existing) && existing.rfind("ok\n", 0) == 0) {
        std::istringstream in(existing.substr(3)); std::string line; while (std::getline(in, line)) { std::istringstream row(line); std::string id; row >> id; if (packages.count(id)) loaded.insert(id); }
    }
    std::function<bool(const std::string&)> load = [&](const std::string& id) {
        if (loaded.count(id)) return true;
        if (visiting.count(id) || !packages.count(id)) { log.push_back("missing or cyclic dependency: " + id); return false; }
        visiting.insert(id); const auto& p = packages.at(id);
        for (const auto& dep : p.compiled.meta.dependencies) {
            if (!packages.count(dep.first) || packages.at(dep.first).compiled.meta.version != dep.second || !load(dep.first)) { log.push_back("dependency version unavailable: " + dep.first); visiting.erase(id); return false; }
        }
        std::string error, staged = stage_file(game, p.source, error), reply;
        auto grants = read_grants(fs::path(p.source).replace_extension(".grants"));
        bool ok = !staged.empty() && agent_request(game, {"script", "load", staged, grants.empty() ? "none" : join(grants, ","), p.source}, reply) && reply.rfind("ok\n", 0) == 0;
        log.push_back(ok ? "loaded " + id : "could not load " + id + ": " + (staged.empty() ? error : reply));
        visiting.erase(id); if (ok) loaded.insert(id); return ok;
    };
    for (const auto& [id, p] : packages) load(id);
}
int scripts_command(const std::string& cmd, const std::vector<std::string>& args, const GameProcess* game) {
    if (cmd == "search" || cmd == "info" || cmd == "versions") return browse_registry(cmd, args);
    if (cmd == "install") return install(args, game);
    if (cmd == "uninstall") return uninstall(args, game);
    if (cmd == "scripts") {
        if (args.size() > 1 && args[1] == "load") {
            if (!game) { std::cerr << "no game with UMC\n"; return 1; }
            std::vector<std::string> log; load_installed_scripts(*game, log); for (const auto& line : log) std::cout << line << '\n'; return 0;
        }
        return list_installed();
    }
    return 2;
}
}
