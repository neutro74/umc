#include "agent/scripts.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "agent/core.hpp"
#include "agent/game.hpp"
#include "agent/java.hpp"
#include "common/platform.hpp"
#include "common/script_meta.hpp"
#include "agent/util.hpp"
#include "quickjs.h"

namespace umc {

struct ScriptEngine::Script {
    std::string name, path;
    std::set<std::string> permissions;
    JSContext* ctx = nullptr;
    std::map<std::string, std::vector<JSValue>> handlers;
    std::map<int, JSValue> hooks;
};

namespace {

constexpr size_t kMemoryLimit = 64u << 20;
constexpr size_t kStackLimit = 512u << 10;
constexpr int kLoadBudgetMs = 500;
constexpr int kHandlerBudgetMs = 50;

// Scripts run one at a time under the engine lock, so a single deadline is enough.
std::chrono::steady_clock::time_point g_deadline;
bool g_timed = false;

int interrupt(JSRuntime*, void*) { return g_timed && std::chrono::steady_clock::now() > g_deadline; }

struct Budget {
    std::chrono::steady_clock::time_point previous_deadline;
    bool previous_timed;
    explicit Budget(int ms) {
        previous_deadline = g_deadline;
        previous_timed = g_timed;
        auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        g_deadline = g_timed ? std::min(g_deadline, next) : next;
        g_timed = true;
    }
    ~Budget() { g_deadline = previous_deadline; g_timed = previous_timed; }
};

std::string describe_exception(JSContext* ctx) {
    JSValue ex = JS_GetException(ctx);
    std::string out = "unknown error";
    if (const char* msg = JS_ToCString(ctx, ex)) {
        out = msg;
        JS_FreeCString(ctx, msg);
    }
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (!JS_IsUndefined(stack)) {
        if (const char* st = JS_ToCString(ctx, stack)) {
            if (*st) out += std::string("\n") + st;
            JS_FreeCString(ctx, st);
        }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
    return out;
}

ScriptEngine::Script* script_of(JSContext* ctx) {
    return static_cast<ScriptEngine::Script*>(JS_GetContextOpaque(ctx));
}

JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string line;
    for (int i = 0; i < argc; ++i) {
        if (const char* s = JS_ToCString(ctx, argv[i])) {
            line += (i ? " " : "") + std::string(s);
            JS_FreeCString(ctx, s);
        }
    }
    Core::get().log("[" + script_of(ctx)->name + "] " + line);
    return JS_UNDEFINED;
}

JSValue js_on(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "umc.on(event, function)");
    const char* name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    std::string event = name;
    JS_FreeCString(ctx, name);
    static const std::map<std::string, std::string> needs = {{"render", "render"}, {"chat", "chat"}, {"send", "chat"}, {"attack", "combat"}};
    if (auto need = needs.find(event); need != needs.end() && !script_of(ctx)->permissions.count(need->second))
        return JS_ThrowRangeError(ctx, "the '%s' event needs the '%s' permission", event.c_str(), need->second.c_str());
    if (!ScriptEngine::get().ensure_event_hook(event)) return JS_ThrowRangeError(ctx, "unknown event '%s'", event.c_str());
    script_of(ctx)->handlers[event].push_back(JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}

JNIEnv* current_env() {
    JNIEnv* env = nullptr;
    Core::get().vm()->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    return env;
}

JSValue entity_to_js(JSContext* ctx, const EntityInfo& e) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, e.name.c_str()));
    JS_SetPropertyStr(ctx, o, "uuid", JS_NewString(ctx, e.uuid.c_str()));
    JS_SetPropertyStr(ctx, o, "id", JS_NewInt32(ctx, e.id));
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, e.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, e.y));
    JS_SetPropertyStr(ctx, o, "z", JS_NewFloat64(ctx, e.z));
    JS_SetPropertyStr(ctx, o, "yaw", JS_NewFloat64(ctx, e.yaw));
    JS_SetPropertyStr(ctx, o, "pitch", JS_NewFloat64(ctx, e.pitch));
    JS_SetPropertyStr(ctx, o, "health", JS_NewFloat64(ctx, e.health));
    JS_SetPropertyStr(ctx, o, "onGround", JS_NewBool(ctx, e.on_ground));
    return o;
}

// Every game call runs inside its own JNI frame so scripts can't leak local references.
struct JniFrame {
    JNIEnv* env;
    explicit JniFrame(JNIEnv* e) : env(e) { env->PushLocalFrame(64); }
    ~JniFrame() { env->PopLocalFrame(nullptr); }
};

JSValue js_game_player(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JNIEnv* env = current_env();
    if (!env) return JS_NULL;
    JniFrame frame(env);
    EntityInfo info;
    return GameApi::get().local_player(env, info) ? entity_to_js(ctx, info) : JS_NULL;
}

JSValue js_game_players(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue array = JS_NewArray(ctx);
    JNIEnv* env = current_env();
    if (!env) return array;
    JniFrame frame(env);
    uint32_t i = 0;
    for (auto& e : GameApi::get().players(env)) JS_SetPropertyUint32(ctx, array, i++, entity_to_js(ctx, e));
    return array;
}

// Graphics object of the frame being drawn; only set while "render" handlers run.
thread_local jobject g_graphics = nullptr;
thread_local bool g_in_frame = false;

uint32_t color_arg(JSContext* ctx, JSValueConst v) {
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return uint32_t(int64_t(d));
}

JSValue js_ui_rect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "ui.rect(x, y, width, height, 0xAARRGGBB)");
    int v[4];
    for (int i = 0; i < 4; ++i) JS_ToInt32(ctx, &v[i], argv[i]);
    JNIEnv* env = current_env();
    if (!env || !g_in_frame) return JS_FALSE;
    return JS_NewBool(ctx, GameApi::get().draw_rect(env, g_graphics, v[0], v[1], v[2], v[3], color_arg(ctx, argv[4])));
}

JSValue js_ui_text(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "ui.text(text, x, y, 0xAARRGGBB, shadow = true)");
    const char* t = JS_ToCString(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    std::string text = t;
    JS_FreeCString(ctx, t);
    int x = 0, y = 0;
    JS_ToInt32(ctx, &x, argv[1]);
    JS_ToInt32(ctx, &y, argv[2]);
    bool shadow = argc < 5 || JS_ToBool(ctx, argv[4]);
    JNIEnv* env = current_env();
    if (!env || !g_in_frame) return JS_FALSE;
    return JS_NewBool(ctx, GameApi::get().draw_text(env, g_graphics, text, x, y, color_arg(ctx, argv[3]), shadow));
}

JSValue js_ui_width(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    const char* t = JS_ToCString(ctx, argv[0]);
    if (!t) return JS_EXCEPTION;
    std::string text = t;
    JS_FreeCString(ctx, t);
    JNIEnv* env = current_env();
    return JS_NewInt32(ctx, env ? GameApi::get().text_width(env, text) : 0);
}

JSValue js_ui_screen(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue o = JS_NewObject(ctx);
    int w = 0, h = 0;
    JNIEnv* env = current_env();
    if (env && g_in_frame) GameApi::get().screen_size(env, g_graphics, w, h);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    return o;
}

// Per-script private storage: one flat folder per script, with size limits.
constexpr size_t kMaxFileBytes = 1u << 20;
constexpr size_t kMaxFiles = 100;
constexpr size_t kMaxTotalBytes = 10u << 20;

namespace fsys = std::filesystem;

fsys::path storage_dir(const ScriptEngine::Script* script) {
    return fsys::path(plat::config_dir()) / "data" / sanitize_id(script->name);
}

// Only plain names like "settings.json" are allowed, so a script can never leave its folder.
bool valid_file_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (unsigned char c : name)
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') return false;
    return name.find("..") == std::string::npos;
}

bool file_arg(JSContext* ctx, JSValueConst v, std::string& name, JSValue& error) {
    const char* c = JS_ToCString(ctx, v);
    if (!c) {
        error = JS_EXCEPTION;
        return false;
    }
    name = c;
    JS_FreeCString(ctx, c);
    if (!valid_file_name(name)) {
        error = JS_ThrowTypeError(ctx, "invalid file name '%s' (use letters, digits, '.', '_' and '-')", name.c_str());
        return false;
    }
    return true;
}

size_t folder_size(const fsys::path& dir, size_t& files) {
    size_t total = 0;
    files = 0;
    std::error_code ec;
    for (auto& e : fsys::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) {
            ++files;
            total += size_t(e.file_size(ec));
        }
    return total;
}

JSValue js_fs_read(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string name;
    JSValue error;
    if (argc < 1 || !file_arg(ctx, argv[0], name, error)) return argc < 1 ? JS_ThrowTypeError(ctx, "fs.read(name)") : error;
    std::ifstream in(storage_dir(script_of(ctx)) / name, std::ios::binary);
    if (!in) return JS_NULL;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string body = ss.str();
    return JS_NewStringLen(ctx, body.data(), body.size());
}

JSValue write_file(JSContext* ctx, int argc, JSValueConst* argv, bool append) {
    std::string name;
    JSValue error;
    if (argc < 2) return JS_ThrowTypeError(ctx, append ? "fs.append(name, text)" : "fs.write(name, text)");
    if (!file_arg(ctx, argv[0], name, error)) return error;
    size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!text) return JS_EXCEPTION;
    std::string body(text, length);
    JS_FreeCString(ctx, text);

    fsys::path dir = storage_dir(script_of(ctx)), file = dir / name;
    std::error_code ec;
    fsys::create_directories(dir, ec);
    size_t files = 0, total = folder_size(dir, files);
    size_t existing = fsys::exists(file, ec) ? size_t(fsys::file_size(file, ec)) : 0;
    size_t new_size = append ? existing + body.size() : body.size();
    if (new_size > kMaxFileBytes) return JS_ThrowRangeError(ctx, "file would be larger than %zu KB", kMaxFileBytes >> 10);
    if (!existing && files >= kMaxFiles) return JS_ThrowRangeError(ctx, "storage is limited to %zu files", kMaxFiles);
    if (total - existing + new_size > kMaxTotalBytes) return JS_ThrowRangeError(ctx, "storage is limited to %zu MB", kMaxTotalBytes >> 20);

    std::string content = body;
    if (append && existing) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        content = ss.str() + body;
    }
    fsys::path temp = dir / (name + ".tmp");  // write whole, then rename, so a crash can't leave half a file
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) return JS_ThrowInternalError(ctx, "could not write %s", name.c_str());
    }
    fsys::rename(temp, file, ec);
    return ec ? JS_ThrowInternalError(ctx, "could not write %s", name.c_str()) : JS_TRUE;
}

JSValue js_fs_write(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return write_file(ctx, argc, argv, false); }
JSValue js_fs_append(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return write_file(ctx, argc, argv, true); }

JSValue js_fs_exists(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string name;
    JSValue error;
    if (argc < 1 || !file_arg(ctx, argv[0], name, error)) return argc < 1 ? JS_ThrowTypeError(ctx, "fs.exists(name)") : error;
    std::error_code ec;
    return JS_NewBool(ctx, fsys::is_regular_file(storage_dir(script_of(ctx)) / name, ec));
}

JSValue js_fs_remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string name;
    JSValue error;
    if (argc < 1 || !file_arg(ctx, argv[0], name, error)) return argc < 1 ? JS_ThrowTypeError(ctx, "fs.remove(name)") : error;
    std::error_code ec;
    return JS_NewBool(ctx, fsys::remove(storage_dir(script_of(ctx)) / name, ec));
}

JSValue js_fs_list(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue array = JS_NewArray(ctx);
    std::error_code ec;
    uint32_t i = 0;
    for (auto& e : fsys::directory_iterator(storage_dir(script_of(ctx)), ec))
        if (e.is_regular_file(ec) && valid_file_name(e.path().filename().string()) && e.path().extension() != ".tmp")
            JS_SetPropertyUint32(ctx, array, i++, JS_NewString(ctx, e.path().filename().string().c_str()));
    return array;
}

JSValue js_game_chat(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "game.chat(text)");
    const char* text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    std::string message = text;
    JS_FreeCString(ctx, text);
    JNIEnv* env = current_env();
    if (!env) return JS_FALSE;
    JniFrame frame(env);
    return JS_NewBool(ctx, GameApi::get().chat(env, message));
}

}  // namespace

ScriptEngine& ScriptEngine::get() {
    static ScriptEngine engine;
    return engine;
}

void ScriptEngine::ensure_runtime() {
    if (rt_) return;
    rt_ = JS_NewRuntime();
    JS_SetMemoryLimit(rt_, kMemoryLimit);
    JS_SetMaxStackSize(rt_, kStackLimit);
    JS_SetInterruptHandler(rt_, interrupt, nullptr);
}

bool ScriptEngine::ensure_event_hook(const std::string& event) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (event_hooks_.count(event)) return true;
    Core& core = Core::get();
    GameApi& game = GameApi::get();
    JNIEnv* env = current_env();
    std::vector<int> ids;
    std::string owner, method, desc;

    // Hooks every method the table lists under `label` that exists on this version.
    auto hook_all = [&](const std::string& label, const std::string& name_only_filter, HookFn fn) {
        if (!env) return;
        for (auto& t : game.hook_targets(env, label)) {
            int id = core.hook(t.owner, t.name, name_only_filter.empty() ? t.desc : "", fn);
            if (id >= 0) ids.push_back(id);
        }
    };

    if (event == "tick") {
        auto fire = [](Call&) { ScriptEngine::get().emit("tick"); };
        if (core.side() == Side::Server) {
            int id = core.hook("net.minecraft.server.MinecraftServer", "tickServer", "", fire);
            if (id >= 0) ids.push_back(id);
        } else {
            hook_all("hook.tick", "", fire);
        }
    } else if (event == "render") {
        auto draw = [](Call& call) {
            JNIEnv* e = call.env();
            jobject first = call.arg(0);
            if (!GameApi::get().frame_token(e, first, g_graphics)) return;
            g_in_frame = true;
            ScriptEngine::get().emit("render");
            g_in_frame = false;
            g_graphics = nullptr;
        };
        // Name only: the HUD method's parameters differ between versions and the name is unique in its class.
        hook_all("hook.render", "name", draw);
    } else if (event == "chat") {
        hook_all("hook.chat", "", [](Call& call) {
            JNIEnv* e = call.env();
            jobject message = call.arg(0);
            std::string text = GameApi::get().text_of(e, message);
            if (ScriptEngine::get().emit("chat", [&](JSContext* ctx, JSValue o) {
                    JS_SetPropertyStr(ctx, o, "text", JS_NewString(ctx, text.c_str()));
                }))
                call.cancel();
        });
    } else if (event == "send") {
        auto on_send = [](bool is_command) {
            return [is_command](Call& call) {
                JNIEnv* e = call.env();
                std::string text = GameApi::get().text_of(e, call.arg(0));
                if (is_command && (text.empty() || text[0] != '/')) text = "/" + text;  // 1.19+ sends commands without it
                if (ScriptEngine::get().emit("send", [&](JSContext* ctx, JSValue o) {
                        JS_SetPropertyStr(ctx, o, "text", JS_NewString(ctx, text.c_str()));
                        JS_SetPropertyStr(ctx, o, "command", JS_NewBool(ctx, text.rfind("/", 0) == 0));
                    }))
                    call.cancel();
            };
        };
        hook_all("hook.send", "", on_send(false));
        hook_all("hook.command", "", on_send(true));
    } else if (event == "attack") {
        hook_all("hook.attack", "", [](Call& call) {
            JNIEnv* e = call.env();
            EntityInfo target;
            bool known = GameApi::get().entity_info(e, call.arg(1), target);
            if (ScriptEngine::get().emit("attack", [&](JSContext* ctx, JSValue o) {
                    JS_SetPropertyStr(ctx, o, "target", known ? entity_to_js(ctx, target) : JS_NULL);
                }))
                call.cancel();
        });
    } else {
        return false;
    }
    if (ids.empty()) {
        core.log("script event '" + event + "' has no hook point on this version");
        return false;
    }
    event_hooks_[event] = ids;
    return true;
}

bool ScriptEngine::load(const std::string& path, std::string& error, const std::set<std::string>* grants) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string source = buffer.str();

    std::string name = path.substr(path.find_last_of("/\\") + 1);
    if (size_t dot = name.rfind('.'); dot != std::string::npos) name.resize(dot);

    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (scripts_.count(name)) {
        error = "a script named " + name + " is already loaded";
        return false;
    }
    ensure_runtime();
    JS_UpdateStackTop(rt_);  // this request may be on a different thread than the last one

    ScriptMeta meta = parse_script_meta(source, name);
    auto script = std::make_unique<Script>();
    script->name = meta.has_header && !meta.id.empty() ? meta.id : name;
    if (scripts_.count(script->name)) {
        error = "a script named " + script->name + " is already loaded";
        return false;
    }
    script->path = path;
    for (auto& p : meta.permissions) {
        if (!is_known_permission(p)) {
            error = "unknown permission '" + p + "'";
            return false;
        }
        if (!grants || grants->count(p)) script->permissions.insert(p);
    }
    if (!meta.has_header && !grants) {  // plain file with no header: local development code
        for (auto& [perm, text] : known_permissions()) script->permissions.insert(perm);
        Core::get().log("[" + script->name + "] no header, granting every permission (local script)");
    }
    name = script->name;
    script->ctx = JS_NewContext(rt_);
    JS_SetContextOpaque(script->ctx, script.get());

    // Capabilities are installed only for granted permissions. Trusted Java access is explicit.
    JSContext* ctx = script->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue umc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, umc, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, umc, "log", JS_NewCFunction(ctx, js_log, "log", 1));
    JS_SetPropertyStr(ctx, umc, "on", JS_NewCFunction(ctx, js_on, "on", 2));
    JS_SetPropertyStr(ctx, global, "umc", umc);

    const auto& allowed = script->permissions;
    if (allowed.count("java")) install_java_api(ctx, global);
    JSValue game = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, game, "version", JS_NewString(ctx, Core::get().version().c_str()));
    if (allowed.count("player")) JS_SetPropertyStr(ctx, game, "player", JS_NewCFunction(ctx, js_game_player, "player", 0));
    if (allowed.count("players")) JS_SetPropertyStr(ctx, game, "players", JS_NewCFunction(ctx, js_game_players, "players", 0));
    if (allowed.count("chat")) JS_SetPropertyStr(ctx, game, "chat", JS_NewCFunction(ctx, js_game_chat, "chat", 1));
    JS_SetPropertyStr(ctx, global, "game", game);

    if (allowed.count("storage")) {
        JSValue fs = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, fs, "read", JS_NewCFunction(ctx, js_fs_read, "read", 1));
        JS_SetPropertyStr(ctx, fs, "write", JS_NewCFunction(ctx, js_fs_write, "write", 2));
        JS_SetPropertyStr(ctx, fs, "append", JS_NewCFunction(ctx, js_fs_append, "append", 2));
        JS_SetPropertyStr(ctx, fs, "exists", JS_NewCFunction(ctx, js_fs_exists, "exists", 1));
        JS_SetPropertyStr(ctx, fs, "remove", JS_NewCFunction(ctx, js_fs_remove, "remove", 1));
        JS_SetPropertyStr(ctx, fs, "list", JS_NewCFunction(ctx, js_fs_list, "list", 0));
        JS_SetPropertyStr(ctx, global, "fs", fs);
    }

    if (allowed.count("render")) {
        JSValue ui = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, ui, "rect", JS_NewCFunction(ctx, js_ui_rect, "rect", 5));
        JS_SetPropertyStr(ctx, ui, "text", JS_NewCFunction(ctx, js_ui_text, "text", 5));
        JS_SetPropertyStr(ctx, ui, "width", JS_NewCFunction(ctx, js_ui_width, "width", 1));
        JS_SetPropertyStr(ctx, ui, "screen", JS_NewCFunction(ctx, js_ui_screen, "screen", 0));
        JS_SetPropertyStr(ctx, global, "ui", ui);
    }
    JS_FreeValue(ctx, global);

    JSValue result;
    {
        Budget budget(kLoadBudgetMs);
        result = JS_Eval(ctx, source.c_str(), source.size(), path.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    }
    if (JS_IsException(result)) {
        error = describe_exception(ctx);
        for (auto& [event, fns] : script->handlers)
            for (JSValue fn : fns) JS_FreeValue(ctx, fn);
        for (auto& [id, fn] : script->hooks) {
            Core::get().unhook(id);
            JS_FreeValue(ctx, fn);
        }
        JS_FreeContext(ctx);
        return false;
    }
    JS_FreeValue(ctx, result);
    std::string perms;
    for (auto& p : scripts_.count(name) ? std::set<std::string>() : script->permissions) perms += (perms.empty() ? "" : ", ") + p;
    Core::get().log("script loaded: " + name + (perms.empty() ? " (no permissions)" : " (" + perms + ")"));
    scripts_[name] = std::move(script);
    return true;
}

bool ScriptEngine::unload(const std::string& name, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = scripts_.find(name);
    if (it == scripts_.end()) {
        error = "no script named " + name;
        return false;
    }
    Script& script = *it->second;
    for (auto& [id, fn] : script.hooks) {
        Core::get().unhook(id);
        JS_FreeValue(script.ctx, fn);
    }
    for (auto& [event, fns] : script.handlers)
        for (JSValue fn : fns) JS_FreeValue(script.ctx, fn);
    JS_FreeContext(script.ctx);
    scripts_.erase(it);
    Core::get().log("script unloaded: " + name);
    return true;
}

std::vector<std::string> ScriptEngine::list() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::vector<std::string> out;
    for (auto& [name, script] : scripts_) {
        std::string perms;
        for (auto& p : script->permissions) perms += (perms.empty() ? "" : " ") + p;
        out.push_back(name + "  [" + perms + "]");
    }
    return out;
}

JSValue ScriptEngine::add_java_hook(JSContext* ctx, const std::string& owner, const std::string& method,
                                   const std::string& descriptor, JSValueConst callback) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Script* script = script_of(ctx);
    if (!script->permissions.count("java")) return JS_ThrowRangeError(ctx, "Java hooks need the 'java' permission");
    auto id = std::make_shared<int>(-1);
    std::string name = script->name;
    *id = Core::get().hook(owner, method, descriptor, [this, name, id, descriptor](Call& call) {
        // Core can retain a callback snapshot during unload. Resolve its owner under the engine lock.
        std::lock_guard<std::recursive_mutex> lock(mu_);
        auto it = scripts_.find(name);
        if (it == scripts_.end()) return;
        Script& script = *it->second;
        auto fn = script.hooks.find(*id);
        if (fn == script.hooks.end()) return;
        JS_UpdateStackTop(rt_);
        JSValue callback = JS_DupValue(script.ctx, fn->second);
        JSValue event = java_call_event(script.ctx, call, descriptor);
        JSValue result = JS_EXCEPTION;
        if (!JS_IsException(event)) {
            Budget budget(kHandlerBudgetMs);
            result = JS_Call(script.ctx, callback, JS_UNDEFINED, 1, &event);
            expire_java_call(event);
            JS_FreeValue(script.ctx, event);
        }
        if (JS_IsException(result)) {
            Core::get().log("[" + name + "] Java hook error: " + describe_exception(script.ctx));
            Core::get().unhook(*id);
            auto remaining = script.hooks.find(*id);
            if (remaining != script.hooks.end()) {
                JS_FreeValue(script.ctx, remaining->second);
                script.hooks.erase(remaining);
            }
        }
        JS_FreeValue(script.ctx, callback);
        JS_FreeValue(script.ctx, result);
    });
    if (*id < 0) return JS_ThrowReferenceError(ctx, "could not hook Java method");
    script->hooks[*id] = JS_DupValue(ctx, callback);
    return JS_NewInt32(ctx, *id);
}

JSValue ScriptEngine::remove_java_hook(JSContext* ctx, int id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Script* script = script_of(ctx);
    auto it = script->hooks.find(id);
    if (it == script->hooks.end()) return JS_FALSE;
    Core::get().unhook(id);
    JS_FreeValue(ctx, it->second);
    script->hooks.erase(it);
    return JS_TRUE;
}

namespace {

// event.cancel() marks the event; emit() reads the flag back after each handler.
JSValue js_event_cancel(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    JS_SetPropertyStr(ctx, this_val, "cancelled", JS_TRUE);
    return JS_UNDEFINED;
}

thread_local bool g_in_event = false;

}  // namespace

bool ScriptEngine::emit(const std::string& event, const EventFill& fill) {
    // A handler that triggers the same kind of event (say, chatting from a chat handler) must not recurse.
    if (g_in_event && fill) return false;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!rt_) return false;
    JS_UpdateStackTop(rt_);
    bool cancelled = false;
    struct Guard {
        bool active;
        explicit Guard(bool a) : active(a) { if (active) g_in_event = true; }
        ~Guard() { if (active) g_in_event = false; }
    } guard(fill != nullptr);

    for (auto& [name, script] : scripts_) {
        auto it = script->handlers.find(event);
        if (it == script->handlers.end()) continue;
        JSContext* ctx = script->ctx;
        bool failed = false;
        for (JSValue fn : it->second) {
            JSValue ev = JS_UNDEFINED;
            int argc = 0;
            if (fill) {
                ev = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, ev, "cancelled", JS_FALSE);
                JS_SetPropertyStr(ctx, ev, "cancel", JS_NewCFunction(ctx, js_event_cancel, "cancel", 0));
                fill(ctx, ev);
                argc = 1;
            }
            JSValue r;
            {
                Budget budget(kHandlerBudgetMs);
                r = JS_Call(ctx, fn, JS_UNDEFINED, argc, &ev);
            }
            if (fill) {
                JSValue flag = JS_GetPropertyStr(ctx, ev, "cancelled");
                if (JS_ToBool(ctx, flag)) cancelled = true;
                JS_FreeValue(ctx, flag);
                JS_FreeValue(ctx, ev);
            }
            if (JS_IsException(r)) {
                Core::get().log("[" + name + "] error in '" + event + "' handler: " + describe_exception(ctx));
                failed = true;
                break;
            }
            JS_FreeValue(ctx, r);
        }
        if (failed) {  // a broken handler is dropped so it can't flood the log every tick
            for (JSValue fn : it->second) JS_FreeValue(ctx, fn);
            script->handlers.erase(it);
        }
    }
    return cancelled;
}

}  // namespace umc
