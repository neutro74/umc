// UMC public API. Modules (built in or loaded from plugin .so files) only depend on this header.
#pragma once

#include <jni.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace umc {

enum class Side { Unknown, Client, Server };

// One invocation of a hooked method. Valid only for the duration of the hook callback.
class Call {
public:
    virtual ~Call() = default;

    virtual JNIEnv* env() = 0;
    virtual jobject self() = 0;   // null for static methods
    virtual int arg_count() = 0;
    virtual jobject arg(int index) = 0;  // primitives arrive boxed
    virtual jint arg_int(int index) = 0;
    virtual jlong arg_long(int index) = 0;
    virtual jfloat arg_float(int index) = 0;
    virtual jdouble arg_double(int index) = 0;

    // Skip the original method body. Primitive returns become 0, objects null.
    virtual void cancel() = 0;
    // Skip the body and return this value. Primitive methods take a boxed value.
    virtual void return_object(jobject value) = 0;
    virtual void return_int(jint value) = 0;
    virtual void return_long(jlong value) = 0;
    virtual void return_float(jfloat value) = 0;
    virtual void return_double(jdouble value) = 0;
};

using HookFn = std::function<void(Call&)>;

class Api;

class Module {
public:
    virtual ~Module() = default;
    virtual std::string name() const = 0;
    virtual std::string description() const { return ""; }

    virtual void on_enable(Api&, JNIEnv*) {}
    virtual void on_disable(Api&, JNIEnv*) {}
    // Handles `umc mod <name> <args...>`. The returned text is printed by the CLI.
    virtual std::string on_command(Api&, JNIEnv*, const std::vector<std::string>& args) {
        (void)args;
        return "this module has no commands";
    }

    // Hooks added through here are removed automatically when the module is disabled.
    int hook(Api& api, const std::string& owner, const std::string& method,
             const std::string& desc, HookFn fn);
    std::vector<int>& owned_hooks() { return hooks_; }

private:
    std::vector<int> hooks_;
};

// Names passed to the API use Mojang's deobfuscated names ("net/minecraft/client/Minecraft",
// "tick", "()V") and are translated to whatever the running game actually uses.
// Dots or slashes are both accepted in class names. An empty descriptor matches by name only.
class Api {
public:
    virtual ~Api() = default;

    virtual std::string version() = 0;
    virtual Side side() = 0;
    virtual bool remapped() = 0;  // true when obfuscation mappings are active

    virtual std::string runtime_class_name(const std::string& named) = 0;
    virtual jclass find_class(JNIEnv* env, const std::string& owner) = 0;
    // Searches the owner and its superclasses. Works for static and instance members.
    virtual jmethodID find_method(JNIEnv* env, const std::string& owner, const std::string& name,
                                  const std::string& desc = "") = 0;
    virtual jfieldID find_field(JNIEnv* env, const std::string& owner, const std::string& name,
                                const std::string& desc = "") = 0;

    // Runs fn at the start of the method. Works before or after the class is loaded.
    // Returns a hook id, or -1 if the method cannot be resolved.
    virtual int hook(const std::string& owner, const std::string& method, const std::string& desc,
                     HookFn fn) = 0;
    virtual void unhook(int id) = 0;

    virtual void add_module(std::unique_ptr<Module> module) = 0;
    virtual void log(const std::string& message) = 0;
};

inline int Module::hook(Api& api, const std::string& owner, const std::string& method,
                        const std::string& desc, HookFn fn) {
    int id = api.hook(owner, method, desc, std::move(fn));
    if (id >= 0) hooks_.push_back(id);
    return id;
}

}  // namespace umc

// Plugins export this symbol and register their modules from it.
extern "C" void umc_register(umc::Api* api);
#define UMC_PLUGIN extern "C" __attribute__((visibility("default"))) void umc_register(umc::Api* api)
