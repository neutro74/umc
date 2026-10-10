#pragma once

#include <jvmti.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "agent/mappings.hpp"
#include "umc/umc.hpp"

namespace umc {

inline std::string internal_name(std::string s) {
    for (auto& c : s) if (c == '.') c = '/';
    return s;
}

class Core final : public Api {
public:
    static Core& get();

    // Called once after injection, on a thread attached to the JVM.
    bool start(JavaVM* vm, jvmtiEnv* jvmti, JNIEnv* env, const std::string& options);
    bool started() const { return started_; }

    // Returns (ok, text) for a control socket request.
    std::pair<bool, std::string> command(JNIEnv* env, const std::vector<std::string>& args);

    void on_class_file_load(const char* name, jint len, const unsigned char* data, jint* new_len,
                            unsigned char** new_data);
    void on_class_prepare(JNIEnv* env, jclass cls);
    jobject on_enter(JNIEnv* env, jint site, jobject self, jobjectArray args);
    // Calls the hooked method's original body for Call::proceed().
    jobject invoke_original(JNIEnv* env, int site, jobject self, jobjectArray args);
    bool set_argument(Call& call, int index, jobject value);

    // Blocks until log lines newer than `seq` exist (or the timeout passes) and returns them.
    std::vector<std::string> log_since(uint64_t& seq, int timeout_ms);

    // True when the mappings know this class. Always false without mappings.
    bool mapped_method(const std::string& owner, const std::string& name, const std::string& desc) const {
        return !mappings_.methods(internal_name(owner), name, desc).empty();
    }
    bool mapped_class(const std::string& named) const { return mappings_.by_named(internal_name(named)) != nullptr; }

    jvmtiEnv* jvmti() const { return jvmti_; }
    JavaVM* vm() const { return vm_; }

    std::string version() override { return version_; }
    Side side() override { return side_; }
    bool remapped() override { return !mappings_.empty(); }
    std::string runtime_class_name(const std::string& named) override;
    std::string runtime_descriptor(const std::string& descriptor) const { return mappings_.remap_desc(descriptor); }
    jclass find_class(JNIEnv* env, const std::string& owner) override;
    jmethodID find_method(JNIEnv* env, const std::string& owner, const std::string& name,
                          const std::string& desc) override;
    jfieldID find_field(JNIEnv* env, const std::string& owner, const std::string& name,
                        const std::string& desc) override;
    int hook(const std::string& owner, const std::string& method, const std::string& desc, HookFn fn) override;
    void unhook(int id) override;
    void add_module(std::unique_ptr<Module> module) override;
    void log(const std::string& message) override;

    // Boxing helpers used by Call implementations.
    jobject box(JNIEnv* env, const char* cls, const char* sig, jvalue v);
    jvalue unbox(JNIEnv* env, jobject o, char type);

private:
    struct HookEntry {
        int id;
        std::string owner_named, owner_rt, method_named, desc_named;
        std::vector<std::pair<std::string, std::string>> targets;  // runtime name, desc ("" = any)
        std::shared_ptr<HookFn> fn;
    };
    struct Site {
        int id;
        std::string cls, name, desc;
        std::vector<int> hooks;
        jclass owner = nullptr;      // resolved lazily for proceed()
        jmethodID method = nullptr;
        bool is_static = false;
    };
    struct ModuleSlot {
        std::unique_ptr<Module> module;
        bool enabled = false;
        std::string origin;
    };
    struct Plugin {
        std::string path;
        void* handle;
    };

    bool define_bridge(JNIEnv* env);
    void detect_game(JNIEnv* env);
    bool load_mappings(const std::string& path, std::string& error);
    bool load_plugin(const std::string& path, std::string& error);
    bool unload_plugin(JNIEnv* env, const std::string& which, std::string& path, std::string& error);
    std::string retransform(JNIEnv* env, const std::string& runtime_class);
    void add_bridge_read(JNIEnv* env, jclass cls);
    // Like find_method/find_field, also returning the class that declares the member.
    jmethodID find_method_in(JNIEnv* env, const std::string& owner, const std::string& name,
                             const std::string& desc, jclass* declaring);
    jfieldID find_field_in(JNIEnv* env, const std::string& owner, const std::string& name,
                           const std::string& desc, jclass* declaring);
    jclass scan_loaded(JNIEnv* env, const std::string& runtime_class);
    std::string signature_name(jclass cls);
    ModuleSlot* module_slot(const std::string& name);
    bool set_enabled(JNIEnv* env, ModuleSlot& slot, bool enabled, std::string& msg);

    std::string cmd_status();
    std::string cmd_hooks();
    std::string cmd_map(const std::vector<std::string>& args);
    std::string cmd_classes(JNIEnv* env, const std::string& filter);
    std::string cmd_members(JNIEnv* env, const std::vector<std::string>& args, bool methods);
    std::pair<bool, std::string> cmd_get_set(JNIEnv* env, const std::vector<std::string>& args, bool set);
    std::pair<bool, std::string> cmd_call(JNIEnv* env, const std::vector<std::string>& args);
    std::pair<bool, std::string> cmd_trace(const std::vector<std::string>& args);
    std::pair<bool, std::string> cmd_dump(JNIEnv* env, const std::vector<std::string>& args);
    std::string cmd_threads();
    std::string cmd_mem(JNIEnv* env, bool gc);
    std::string describe(JNIEnv* env, jobject o);
    std::string named_member(const std::string& rt_class, const std::string& name, const std::string& sig, bool method);

    JavaVM* vm_ = nullptr;
    jvmtiEnv* jvmti_ = nullptr;
    bool started_ = false;
    std::map<std::string, std::string> options_;

    std::string version_ = "unknown";
    Side side_ = Side::Unknown;
    std::string loader_kind_ = "vanilla";
    Mappings mappings_;
    std::string mappings_source_;

    jclass bridge_ = nullptr;
    jobject continue_ = nullptr;

    std::recursive_mutex mu_;
    std::unordered_map<std::string, jclass> class_cache_;
    std::map<int, HookEntry> hooks_;
    std::vector<Site> sites_;  // index == site id
    int next_hook_ = 1;
    std::vector<ModuleSlot> modules_;
    std::vector<Plugin> plugins_;
    std::set<int> traces_;

    // Class bytes captured by `dump` during a retransform.
    std::string capture_class_;
    std::vector<uint8_t> captured_;

    std::mutex log_mu_;
    std::condition_variable log_cv_;
    std::deque<std::string> log_;
    uint64_t log_seq_ = 0;  // sequence number of the newest line
};

}  // namespace umc
