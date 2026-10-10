// Script plugins written in JavaScript, each in its own QuickJS context.
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

    // `grants` limits which of the script's declared permissions it receives; null grants everything
    // it declares (or everything, for scripts without a header, which are treated as local dev code).
    bool load(const std::string& path, std::string& error, const std::set<std::string>* grants = nullptr);
    bool unload(const std::string& name, std::string& error);
    std::vector<std::string> list();

    // Makes sure the game hook behind `event` exists. False when the event is unknown.
    bool ensure_event_hook(const std::string& event);
    // Fills the event object a handler receives. Called once per handler, in that script's context.
    using EventFill = std::function<void(JSContext*, JSValue event_object)>;

    // Runs every handler registered for `event` on the game thread. Handlers get an event object when
    // `fill` is given, and can call event.cancel(). Returns true if any handler cancelled it.
    bool emit(const std::string& event, const EventFill& fill = nullptr);
    JSValue add_java_hook(JSContext* ctx, const std::string& owner, const std::string& method,
                          const std::string& descriptor, JSValueConst callback);
    JSValue remove_java_hook(JSContext* ctx, int id);

private:
    ScriptEngine() = default;
    void ensure_runtime();

    std::recursive_mutex mu_;
    JSRuntime* rt_ = nullptr;
    std::map<std::string, std::unique_ptr<Script>> scripts_;
    std::map<std::string, std::vector<int>> event_hooks_;  // event -> core hook ids
};

}  // namespace umc
