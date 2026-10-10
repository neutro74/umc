// Sandboxed UMC language plugins. QuickJS executes only compiler-generated code.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <vector>

#include "quickjs.h"


namespace umc {

class ScriptEngine {
public:
    struct Script;

    static ScriptEngine& get();

    // A plugin can receive only its declared permissions; installed grants can narrow them.
    bool load(const std::string& path, std::string& error, const std::set<std::string>* grants = nullptr, const std::string& origin = "");
    bool unload(const std::string& name, std::string& error);
    std::vector<std::string> list();

    // Makes sure the game hook behind `event` exists. False when the event is unknown.
    bool ensure_event_hook(const std::string& event);
    // Fills the event object a handler receives. Called once per handler, in that script's context.
    using EventFill = std::function<void(JSContext*, JSValue event_object)>;

    // Runs every handler registered for `event` on the game thread. Handlers get an event object when
    // `fill` is given. Returns true if any handler cancelled it.
    bool emit(const std::string& event, const EventFill& fill = nullptr);
    JSValue add_method_hook(JSContext* ctx, const std::string& owner, const std::string& method,
                          const std::string& descriptor, JSValueConst callback);
    JSValue remove_method_hook(JSContext* ctx, int id);
    JSValue call_plugin(JSContext* caller, const std::string& target, const std::string& command, JSValueConst args);
    std::pair<bool, std::string> invoke(const std::string& target, const std::string& command, const std::vector<std::string>& args);
    bool reload(const std::string& name, std::string& error, const std::string& replacement = "");
    std::pair<bool, std::string> source(const std::string& name);
    int schedule(JSContext* ctx, int ticks, JSValueConst fn);
    bool cancel_task(JSContext* ctx, int id);

private:
    ScriptEngine() = default;
    void ensure_runtime();
    void drain_tasks();
    uint64_t tick_ = 0;
    int next_task_ = 1;

    std::recursive_mutex mu_;
    JSRuntime* rt_ = nullptr;
    std::map<std::string, std::unique_ptr<Script>> scripts_;
    std::map<std::string, std::vector<int>> event_hooks_;  // event -> core hook ids
};

}  // namespace umc
