#include "agent/scripts.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

#include "agent/core.hpp"
#include "agent/game.hpp"
#include "agent/util.hpp"
#include "quickjs.h"

namespace umc {

struct ScriptEngine::Script {
    std::string name, path;
    JSContext* ctx = nullptr;
    std::map<std::string, std::vector<JSValue>> handlers;
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
    explicit Budget(int ms) {
        g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        g_timed = true;
    }
    ~Budget() { g_timed = false; }
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
    if (!env || !g_graphics) return JS_FALSE;
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
    if (!env || !g_graphics) return JS_FALSE;
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
    if (env && g_graphics) GameApi::get().screen_size(env, g_graphics, w, h);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    return o;
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
    int id = -1;
    if (event == "tick") {
        auto fire = [](Call&) { ScriptEngine::get().emit("tick"); };
        id = core.side() == Side::Server
                 ? core.hook("net.minecraft.server.MinecraftServer", "tickServer", "", fire)
                 : core.hook("net.minecraft.client.Minecraft", "tick", "()V", fire);
        if (id < 0) core.log("script event 'tick' could not be hooked");
    } else if (event == "render") {
        // The HUD draw method: Hud.extractRenderState on 26.x, Gui.render before that.
        auto draw = [](Call& call) {
            JNIEnv* env = call.env();
            jobject g = call.arg(0);
            if (!g || !GameApi::get().is_graphics(env, g)) return;
            g_graphics = g;
            ScriptEngine::get().emit("render");
            g_graphics = nullptr;
        };
        // Hud exists from 26.x; before that Gui.render draws the HUD. The class may not have loaded yet.
        JNIEnv* env = current_env();
        const char* hud = "net.minecraft.client.gui.Hud";
        // Old versions have Gui.render; 26.x moved the drawing into Hud. If Gui isn't loaded yet, assume modern.
        bool legacy = core.remapped() ? !core.mapped_class(hud) && core.mapped_class("net.minecraft.client.gui.Gui")
                                      : env && core.find_method(env, "net.minecraft.client.gui.Gui", "render", "");
        bool modern = !legacy;
        id = modern ? core.hook(hud, "extractRenderState", "", draw)
                    : core.hook("net.minecraft.client.gui.Gui", "render", "", draw);
        if (id < 0) core.log("script event 'render' could not be hooked");
    } else {
        return false;
    }
    if (id < 0) return false;
    event_hooks_[event] = id;
    return true;
}

bool ScriptEngine::load(const std::string& path, std::string& error) {
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

    auto script = std::make_unique<Script>();
    script->name = name;
    script->path = path;
    script->ctx = JS_NewContext(rt_);
    JS_SetContextOpaque(script->ctx, script.get());

    // The only things a script can reach: a global `umc` object. There is no file, network
    // or process access in the context, and the Java side is only exposed through the curated API.
    JSContext* ctx = script->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue umc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, umc, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, umc, "log", JS_NewCFunction(ctx, js_log, "log", 1));
    JS_SetPropertyStr(ctx, umc, "on", JS_NewCFunction(ctx, js_on, "on", 2));
    JS_SetPropertyStr(ctx, global, "umc", umc);

    JSValue game = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, game, "version", JS_NewString(ctx, Core::get().version().c_str()));
    JS_SetPropertyStr(ctx, game, "player", JS_NewCFunction(ctx, js_game_player, "player", 0));
    JS_SetPropertyStr(ctx, game, "players", JS_NewCFunction(ctx, js_game_players, "players", 0));
    JS_SetPropertyStr(ctx, game, "chat", JS_NewCFunction(ctx, js_game_chat, "chat", 1));
    JS_SetPropertyStr(ctx, global, "game", game);

    JSValue ui = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ui, "rect", JS_NewCFunction(ctx, js_ui_rect, "rect", 5));
    JS_SetPropertyStr(ctx, ui, "text", JS_NewCFunction(ctx, js_ui_text, "text", 5));
    JS_SetPropertyStr(ctx, ui, "width", JS_NewCFunction(ctx, js_ui_width, "width", 1));
    JS_SetPropertyStr(ctx, ui, "screen", JS_NewCFunction(ctx, js_ui_screen, "screen", 0));
    JS_SetPropertyStr(ctx, global, "ui", ui);
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
        JS_FreeContext(ctx);
        return false;
    }
    JS_FreeValue(ctx, result);
    Core::get().log("script loaded: " + name);
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
    for (auto& [name, script] : scripts_) out.push_back(name + "  " + script->path);
    return out;
}

void ScriptEngine::emit(const std::string& event) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!rt_) return;
    JS_UpdateStackTop(rt_);
    for (auto& [name, script] : scripts_) {
        auto it = script->handlers.find(event);
        if (it == script->handlers.end()) continue;
        JSContext* ctx = script->ctx;
        bool failed = false;
        for (JSValue fn : it->second) {
            JSValue r;
            {
                Budget budget(kHandlerBudgetMs);
                r = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
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
}

}  // namespace umc
