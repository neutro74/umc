// Script plugins written in JavaScript, each in its own QuickJS context.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <vector>

struct JSRuntime;

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
    // Runs every handler registered for `event`. Called on the game thread.
    void emit(const std::string& event);

private:
    ScriptEngine() = default;
    void ensure_runtime();

    std::recursive_mutex mu_;
    JSRuntime* rt_ = nullptr;
    std::map<std::string, std::unique_ptr<Script>> scripts_;
    std::map<std::string, int> event_hooks_;  // event -> core hook id
};

}  // namespace umc
