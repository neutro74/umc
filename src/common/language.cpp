#include "common/language.hpp"
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace umc::lang {
std::string quote(const std::string& s) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32 || c == 127) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    return out + '"';
}
namespace {
struct Token { std::string text; int kind, line; }; // 0 punctuation, 1 identifier, 2 number, 3 string
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
std::vector<Token> lex(const std::string& s) {
    if (s.size() > max_source_bytes) throw Failure("source exceeds 256 KB");
    std::vector<Token> out; size_t i = 0; int line = 1;
    auto add = [&](std::string text, int kind) { if (out.size() >= 32768) throw Failure("too many tokens"); out.push_back({text, kind, line}); };
    while (i < s.size()) {
        unsigned char c = s[i];
        if (std::isspace(c)) { if (c == '\n') ++line; ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') ++i; continue; }
        if (c == '"') {
            ++i; std::string v; bool closed = false;
            while (i < s.size()) {
                char ch = s[i++];
                if (ch == '"') { closed = true; break; }
                if (ch == '\n' || ch == '\r' || !ch) throw Failure("line " + std::to_string(line) + ": invalid string");
                if (ch == '\\') {
                    if (i == s.size()) break;
                    ch = s[i++];
                    switch (ch) { case 'n': ch = '\n'; break; case 'r': ch = '\r'; break; case 't': ch = '\t'; break;
                    case '0': ch = 0; break; case '\\': case '"': break; default: throw Failure("unsupported string escape"); }
                }
                v += ch;
                if (v.size() > 65536) throw Failure("string exceeds 64 KB");
            }
            if (!closed) throw Failure("unterminated string");
            add(v, 3); continue;
        }
        if (std::isalpha(c) || c == '_') {
            size_t start = i++;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            std::string name = s.substr(start, i - start);
            if (name.size() > 64 || name.rfind("__", 0) == 0) throw Failure("reserved or oversized identifier");
            add(name, 1); continue;
        }
        if (std::isdigit(c)) {
            size_t start = i++;
            if (c == '0' && i < s.size() && s[i] == 'x') {
                ++i; size_t digits = i;
                while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
                if (i == digits) throw Failure("invalid hex number");
            } else {
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                if (i < s.size() && s[i] == '.') { ++i; while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i; }
            }
            std::string number = s.substr(start, i - start);
            double value;
            try { value = number.rfind("0x", 0) == 0 ? double(std::stoull(number, nullptr, 16)) : std::stod(number); }
            catch (...) { throw Failure("number out of range"); }
            if (!std::isfinite(value) || number.size() > 32) throw Failure("number out of range");
            if (number.rfind("0x", 0) != 0) {
                while (number.size() > 1 && number[0] == '0' && std::isdigit(static_cast<unsigned char>(number[1]))) number.erase(number.begin());
            }
            add(number, 2); continue;
        }
        std::string two = s.substr(i, 2);
        if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||") { add(two, 0); i += 2; continue; }
        if (std::string("{}()[];,.=+-*/%!<>").find(c) == std::string::npos)
            throw Failure("line " + std::to_string(line) + ": unexpected character");
        add(std::string(1, char(c)), 0); ++i;
    }
    out.push_back({"<end>", 0, line}); return out;
}
bool method_descriptor(const std::string& desc) {
    if (desc.size() > 4096 || desc.empty() || desc[0] != '(') return false;
    size_t at = 1;
    auto type = [&]() {
        size_t arrays = 0;
        while (at < desc.size() && desc[at] == '[') { ++at; if (++arrays > 64) return false; }
        if (at >= desc.size()) return false;
        char kind = desc[at++];
        if (std::string("ZBCSIJFD").find(kind) != std::string::npos) return true;
        if (kind != 'L') return false;
        size_t start = at;
        while (at < desc.size() && desc[at] != ';') {
            unsigned char ch = desc[at++];
            if (!std::isalnum(ch) && ch != '/' && ch != '$' && ch != '_') return false;
        }
        if (at == start || at >= desc.size()) return false;
        ++at; return true;
    };
    size_t count = 0;
    while (at < desc.size() && desc[at] != ')') if (++count > 255 || !type()) return false;
    if (at >= desc.size()) return false;
    ++at;
    if (at < desc.size() && desc[at] == 'V') ++at;
    else if (!type()) return false;
    return at == desc.size();
}
struct Expr { std::string code, path; };
class Parser {
    std::vector<Token> tokens; size_t at = 0; int depth = 0;
    std::set<std::string> functions, declarations;
    std::vector<std::set<std::string>> locals;
    std::vector<std::string> events;
    bool cancellable = false, has_hook = false;
    struct Nest { int& depth; explicit Nest(int& n) : depth(n) { if (++depth > 64) throw Failure("nesting exceeds 64 levels"); } ~Nest() { --depth; } };
    const Token& token() const { return tokens[at]; }
    bool is(const std::string& s) const { return token().text == s && token().kind != 3; }
    bool take(const std::string& s) { if (!is(s)) return false; ++at; return true; }
    [[noreturn]] void fail(const std::string& why) const { throw Failure("line " + std::to_string(token().line) + ": " + why); }
    void expect(const std::string& s) { if (!take(s)) fail("expected '" + s + "'"); }
    std::string id() { if (token().kind != 1) fail("expected identifier"); return tokens[at++].text; }
    std::string text() { if (token().kind != 3) fail("expected quoted string"); return tokens[at++].text; }
    void unique(const std::string& name) { if (!declarations.insert(name).second) fail("duplicate declaration: " + name); }
    int precedence() const {
        if (is("||")) return 1;
        if (is("&&")) return 2;
        if (is("==") || is("!=")) return 3;
        if (is("<") || is(">") || is("<=") || is(">=")) return 4;
        if (is("+") || is("-")) return 5;
        if (is("*") || is("/") || is("%")) return 6;
        return 0;
    }
    Expr expression(int min = 1) {
        Nest guard(depth); Expr left = primary();
        while (precedence() >= min) {
            int p = precedence(); std::string op = tokens[at++].text;
            if (op == "==") op = "===";
            if (op == "!=") op = "!==";
            Expr right = expression(p + 1); left = {"(" + left.code + op + right.code + ")", ""};
        }
        return left;
    }
    Expr primary() {
        Nest guard(depth);
        Expr value;
        if (take("!") || take("-")) { std::string op = tokens[at - 1].text; value = {"(" + op + primary().code + ")", ""}; }
        else if (take("(")) { value = expression(); value.path.clear(); expect(")"); }
        else if (token().kind == 3) value = {quote(text()), ""};
        else if (token().kind == 2) value = {tokens[at++].text, ""};
        else if (take("true")) value = {"true", ""};
        else if (take("false")) value = {"false", ""};
        else if (take("null")) value = {"null", ""};
        else if (take("[")) {
            value.code = "["; size_t n = 0;
            if (!take("]")) { do { if (++n > 1024) fail("array literal exceeds 1024 items"); value.code += (n == 1 ? "" : ",") + expression().code; } while (take(",")); expect("]"); }
            value.code += "]";
        } else if (token().kind == 1) { value.path = id(); value.code = "v_" + value.path; }
        else fail("expected expression");
        for (;;) {
            if (take(".")) { std::string key = id(); value.code = "__umc_get(" + value.code + "," + quote(key) + ")"; if (!value.path.empty()) value.path += "." + key; }
            else if (take("[")) { value.code = "__umc_get(" + value.code + "," + expression().code + ")"; value.path.clear(); expect("]"); }
            else if (take("(")) {
                static const std::map<std::string, std::string> builtins = {
                    {"log","umc.log"}, {"str","__umc_str"}, {"len","__umc_len"}, {"round","__umc_round"}, {"contains","__umc_contains"},
                    {"game.player","game.player"}, {"game.players","game.players"}, {"game.chat","game.chat"}, {"game.version","__umc_version"},
                    {"ui.rect","ui.rect"}, {"ui.text","ui.text"}, {"ui.width","ui.width"}, {"ui.screen","ui.screen"},
                    {"fs.read","fs.read"}, {"fs.write","fs.write"}, {"fs.append","fs.append"}, {"fs.exists","fs.exists"}, {"fs.remove","fs.remove"}, {"fs.list","fs.list"},
                    {"plugins.call","__umc_call"}, {"cancel_task","__umc_cancel_task"}, {"original","__umc_original"}
                };
                std::string callee;
                if (auto b = builtins.find(value.path); b != builtins.end()) callee = b->second;
                else if (functions.count(value.path)) callee = "f_" + value.path;
                else fail("unknown function: " + value.path);
                std::string args; size_t n = 0;
                if (!take(")")) { do { if (++n > 32) fail("too many arguments"); args += (n == 1 ? "" : ",") + expression().code; } while (take(",")); expect(")"); }
                value = {callee + "(" + args + ")", ""};
            } else break;
        }
        return value;
    }
    std::string body(const std::string& parameter = "") {
        Nest guard(depth); expect("{"); std::string code = "{";
        locals.emplace_back(); if (!parameter.empty()) locals.back().insert(parameter);
        while (!take("}")) { if (is("<end>")) fail("unterminated block"); code += "__umc_step();" + statement(); }
        locals.pop_back(); return code + "}";
    }
    std::string statement() {
        if (take("let")) { std::string name = id(); if (!locals.back().insert(name).second) fail("duplicate local: " + name); expect("="); std::string v = expression().code; expect(";"); return "let v_" + name + "=" + v + ";"; }
        if (take("if")) { std::string test = expression().code; std::string code = "if(" + test + ")" + body(); if (take("else")) code += "else" + body(); return code; }
        if (take("while")) { std::string test = expression().code; return "while((__umc_step()," + test + "))" + body(); }
        if (take("return")) { std::string v = is(";") ? "undefined" : expression().code; expect(";"); return "return " + v + ";"; }
        if (take("cancel")) { if (!cancellable) fail("cancel is only valid in an event or hook"); expect(";"); return "__umc_cancel();"; }
        if (take("after")) { std::string delay = expression().code; bool previous = cancellable; cancellable = false; std::string code = "__umc_schedule(" + delay + ",function()" + body() + ");"; cancellable = previous; return code; }
        if (take("try")) { std::string code = "try" + body(); expect("catch"); expect("("); std::string name = id(); expect(")"); return code + "catch(v_" + name + ")" + body(name); }
        if (token().kind == 1 && tokens[at + 1].text == "=") { std::string name = id(); expect("="); std::string v = expression().code; expect(";"); return "v_" + name + "=" + v + ";"; }
        std::string code = expression().code; expect(";"); return code + ";";
    }
public:
    explicit Parser(const std::string& source) : tokens(lex(source)) {
        // Only top-level fn declarations may be called. Scan first to allow forward references.
        int braces = 0;
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i].kind == 0 && tokens[i].text == "{") ++braces;
            if (tokens[i].kind == 0 && tokens[i].text == "}") --braces;
            if (!braces && tokens[i].kind == 1 && tokens[i].text == "fn" && tokens[i + 1].kind == 1) functions.insert(tokens[i + 1].text);
        }
    }
    Compiled parse() {
        Compiled out; expect("plugin"); out.meta.id = text(); expect(";");
        if (out.meta.id.empty() || out.meta.id.size() > 64 || sanitize_id(out.meta.id) != out.meta.id) fail("invalid plugin id");
        out.meta.has_header = true; out.meta.name = out.meta.id; out.meta.version = "1.0.0";
        out.code = "\"use strict\";\n";
        while (!is("<end>")) {
            if (take("name") || take("version") || take("author") || take("description")) {
                std::string key = tokens[at - 1].text; unique(key); std::string v = text(); expect(";");
                if (key == "name") out.meta.name = v; else if (key == "version") out.meta.version = v;
                else if (key == "author") out.meta.author = v; else out.meta.description = v;
            } else if (take("permissions")) {
                unique("permissions"); if (!is(";")) do { std::string p = id(); if (!is_known_permission(p)) fail("unknown permission: " + p); if (std::find(out.meta.permissions.begin(), out.meta.permissions.end(), p) != out.meta.permissions.end()) fail("duplicate permission"); out.meta.permissions.push_back(p); } while (take(",")); expect(";");
            } else if (take("depends")) {
                std::string dep = text(), version = text(); expect(";");
                if (dep.empty() || sanitize_id(dep) != dep || dep == out.meta.id || out.meta.dependencies.size() >= 32) fail("invalid dependency");
                for (const auto& d : out.meta.dependencies) if (d.first == dep) fail("duplicate dependency");
                out.meta.dependencies.push_back({dep, version});
            } else if (take("state")) {
                std::string n = id(); unique("state:" + n); expect("="); out.code += "let v_" + n + "=" + expression().code + ";\n"; expect(";");
            } else if (take("fn")) {
                std::string n = id(); unique("fn:" + n); expect("("); std::string args; size_t count = 0; std::set<std::string> parameters;
                if (!take(")")) { do { if (++count > 32) fail("too many parameters"); std::string param = id(); if (!parameters.insert(param).second) fail("duplicate parameter"); args += (count == 1 ? "" : ",") + std::string("v_") + param; } while (take(",")); expect(")"); }
                out.code += "function f_" + n + "(" + args + "){__umc_step();" + body() + "}\n";
            } else if (take("on")) {
                std::string event = id(); events.push_back(event); std::string parameter = "event";
                if (take("(")) { parameter = id(); expect(")"); }
                cancellable = true; out.code += "umc.on(" + quote(event) + ",function(v_" + parameter + ")" + body(parameter) + ");\n"; cancellable = false;
            } else if (take("command")) {
                std::string n = text(); if (n.empty() || n.size() > 64) fail("invalid command name"); unique("command:" + n); std::string param = "args";
                if (take("(")) { param = id(); expect(")"); }
                out.code += "__umc_command(" + quote(n) + ",function(v_" + param + ")" + body(param) + ");\n";
            } else if (take("hook")) {
                std::string cls = text(), method = text(), desc = text();
                if (cls.rfind("net.minecraft.", 0) != 0 || cls.size() > 512 || cls.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.$_") != std::string::npos || method.empty() || method.size() > 128 || method.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$_") != std::string::npos) fail("hooks must target Minecraft methods");
                if (!method_descriptor(desc)) fail("invalid method descriptor");
                has_hook = true;
                cancellable = true; out.code += "__umc_hook(" + quote(cls) + "," + quote(method) + "," + quote(desc) + ",function(v_args)" + body("args") + ");\n"; cancellable = false;
            } else if (take("init")) { out.code += "(function()" + body() + ")();\n"; }
            else fail("expected plugin declaration (state, fn, on, command, hook or init)");
            if (out.code.size() > max_source_bytes * 8) fail("compiled plugin is too large");
        }
        if (has_hook && std::find(out.meta.permissions.begin(), out.meta.permissions.end(), "hooks") == out.meta.permissions.end()) fail("method hooks need permission: hooks");
        for (const auto& event : events) {
            static const std::map<std::string, std::string> allowed = {{"tick", ""}, {"render", "render"}, {"chat", "chat"}, {"send", "chat"}, {"attack", "combat"}};
            if (!allowed.count(event)) fail("unknown event: " + event);
            const auto& permission = allowed.at(event);
            if (!permission.empty() && std::find(out.meta.permissions.begin(), out.meta.permissions.end(), permission) == out.meta.permissions.end()) fail("event needs permission: " + permission);
        }
        if (out.meta.version.empty() || !std::isalnum(static_cast<unsigned char>(out.meta.version[0])) || out.meta.version.find("..") != std::string::npos || out.meta.version.size() > 32 || out.meta.version.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-") != std::string::npos) fail("invalid version");
        for (const auto& dep : out.meta.dependencies) if (dep.second.empty() || !std::isalnum(static_cast<unsigned char>(dep.second[0])) || dep.second.find("..") != std::string::npos || dep.second.size() > 32 || dep.second.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ._-") != std::string::npos) fail("invalid dependency version");
        return out;
    }
};
}
bool compile(const std::string& source, Compiled& out, std::string& error) {
    try { out = Parser(source).parse(); return true; }
    catch (const std::exception& e) { error = e.what(); return false; }
}
}
