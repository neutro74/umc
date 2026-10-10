// Installing and loading script plugins. Installed scripts live in <config>/scripts/<id>.js with the
// permissions the user approved stored next to them in <id>.grants.
#include "cli/scripts.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "common/script_meta.hpp"
#include "common/shared.hpp"

namespace fs = std::filesystem;

namespace umc::cli {

namespace {

constexpr size_t kMaxScriptBytes = 1 << 20;

std::string scripts_dir() { return plat::join(plat::config_dir(), "scripts"); }

bool is_url(const std::string& s) { return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0; }

bool fetch(const std::string& source, std::string& body, std::string& error) {
    if (!is_url(source)) {
        std::ifstream in(source, std::ios::binary);
        if (!in) {
            error = "cannot read " + source;
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        body = ss.str();
    } else {
        for (char c : source)
            if (!std::isalnum(static_cast<unsigned char>(c)) && std::string(":/._-?=&%~+#@").find(c) == std::string::npos) {
                error = "that URL contains characters UMC won't pass to curl";
                return false;
            }
        FILE* f = popen(("curl -fsSL --max-filesize 1048576 \"" + source + "\"").c_str(), "r");
        if (!f) {
            error = "cannot run curl";
            return false;
        }
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() <= kMaxScriptBytes) body.append(buf, n);
        if (pclose(f) != 0) {
            error = "download failed: " + source;
            return false;
        }
    }
    if (body.size() > kMaxScriptBytes) {
        error = "script is larger than 1 MB";
        return false;
    }
    return true;
}

bool confirm(const std::string& question) {
    std::cout << question << " [y/N] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
}

std::vector<std::string> read_grants(const fs::path& file) {
    std::vector<std::string> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) out.push_back(line);
    return out;
}

std::string join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

int install(const std::vector<std::string>& args, const GameProcess* running) {
    bool yes = false;
    std::string source;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--yes" || args[i] == "-y") yes = true;
        else source = args[i];
    }
    if (source.empty()) {
        std::cerr << "usage: umc install <file.js | https://...> [--yes]\n";
        return 2;
    }
    std::string body, error;
    if (!fetch(source, body, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    ScriptMeta meta = parse_script_meta(body, fs::path(source).stem().string());
    if (!meta.has_header) {
        std::cerr << "this script has no // ==UMC== header, so it can't declare what it needs.\n"
                     "Use `umc script load` to run it as local development code instead.\n";
        return 1;
    }
    if (meta.id.empty() || meta.id != sanitize_id(meta.id)) {
        std::cerr << "the script's @id must be lowercase letters, digits and dashes\n";
        return 1;
    }
    for (auto& p : meta.permissions)
        if (!is_known_permission(p)) {
            std::cerr << "unknown permission '" << p << "'\n";
            return 1;
        }

    std::cout << "\n  " << meta.name << (meta.version.empty() ? "" : "  v" + meta.version)
              << (meta.author.empty() ? "" : "  by " + meta.author) << "\n";
    if (!meta.description.empty()) std::cout << "  " << meta.description << "\n";
    std::cout << "  from " << source << "\n\n";
    if (meta.permissions.empty()) {
        std::cout << "  It asks for no permissions: it can't read your game or draw anything.\n\n";
    } else {
        std::cout << "  It asks permission to:\n";
        for (auto& p : meta.permissions)
            for (auto& [name, text] : known_permissions())
                if (name == p) std::cout << "    - " << text << "  (" << name << ")\n";
        std::cout << "\n";
    }
    std::cout << "  Scripts run in a sandbox with no file, network or system access; these are the only\n"
                 "  parts of the game it can reach.\n\n";
    if (!yes && !confirm("Install " + meta.name + "?")) {
        std::cout << "cancelled\n";
        return 1;
    }

    plat::make_dirs(scripts_dir());
    fs::path file = fs::path(scripts_dir()) / (meta.id + ".js");
    std::ofstream(file, std::ios::binary) << body;
    std::ofstream(fs::path(scripts_dir()) / (meta.id + ".grants")) << join(meta.permissions, "\n") << "\n";
    std::cout << "installed " << meta.id << " -> " << file.string() << "\n";

    if (running) {
        std::vector<std::string> log;
        load_installed_scripts(*running, log);
        for (auto& line : log) std::cout << line << "\n";
    } else {
        std::cout << "it will load the next time you `umc attach`\n";
    }
    return 0;
}

int uninstall(const std::vector<std::string>& args, const GameProcess* running) {
    if (args.size() < 2) {
        std::cerr << "usage: umc uninstall <id>\n";
        return 2;
    }
    fs::path base = fs::path(scripts_dir()) / sanitize_id(args[1]);
    std::error_code ec;
    if (!fs::remove(base.string() + ".js", ec)) {
        std::cerr << "no installed script named " << args[1] << "\n";
        return 1;
    }
    fs::remove(base.string() + ".grants", ec);
    std::cout << "uninstalled " << args[1] << "\n";
    if (running) {
        std::string reply;
        agent_request(*running, {"script", "unload", sanitize_id(args[1])}, reply);
    }
    return 0;
}

int list_installed() {
    std::error_code ec;
    bool any = false;
    for (auto& e : fs::directory_iterator(scripts_dir(), ec)) {
        if (e.path().extension() != ".js") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        ScriptMeta meta = parse_script_meta(ss.str(), e.path().stem().string());
        auto grants = read_grants(fs::path(e.path()).replace_extension(".grants"));
        std::cout << meta.id << "  " << meta.name << (meta.version.empty() ? "" : "  v" + meta.version) << "  ["
                  << join(grants, " ") << "]\n";
        any = true;
    }
    if (!any) std::cout << "no scripts installed (try: umc install <file.js | url>)\n";
    return 0;
}

}  // namespace

void load_installed_scripts(const GameProcess& p, std::vector<std::string>& log) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(scripts_dir(), ec)) {
        if (e.path().extension() != ".js") continue;
        std::string error, staged = stage_file(p, e.path().string(), error);
        if (staged.empty()) {
            log.push_back("skipping " + e.path().filename().string() + ": " + error);
            continue;
        }
        auto grants = read_grants(fs::path(e.path()).replace_extension(".grants"));
        std::string reply;
        std::vector<std::string> args = {"script", "load", staged, grants.empty() ? "none" : join(grants, ",")};
        if (!agent_request(p, args, reply) || reply.rfind("ok\n", 0) != 0)
            log.push_back("could not load " + e.path().filename().string() + ": " +
                          (reply.size() > 4 ? reply.substr(reply.find('\n') + 1) : "no reply"));
        else
            log.push_back("loaded " + e.path().stem().string());
    }
}

int scripts_command(const std::string& cmd, const std::vector<std::string>& args, const GameProcess* running) {
    if (cmd == "install") return install(args, running);
    if (cmd == "uninstall") return uninstall(args, running);
    if (cmd == "scripts") {
        if (args.size() > 1 && args[1] == "load") {
            if (!running) {
                std::cerr << "no game with UMC in it\n";
                return 1;
            }
            std::vector<std::string> log;
            load_installed_scripts(*running, log);
            for (auto& line : log) std::cout << line << "\n";
            return 0;
        }
        return list_installed();
    }
    return 2;
}

}  // namespace umc::cli
