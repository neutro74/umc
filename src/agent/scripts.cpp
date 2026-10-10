#include "agent/scripts.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

#include "agent/core.hpp"
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
