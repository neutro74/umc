#include "agent/core.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "agent/classfile.hpp"
#include "agent/util.hpp"
#include "bridge_class.h"
#include "common/shared.hpp"

namespace umc {

namespace {

std::string json_string(const std::string& json, const std::string& key) {
    size_t k = json.find("\"" + key + "\"");
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k);
    size_t open = json.find('"', colon);
    size_t close = json.find('"', open + 1);
    if (colon == std::string::npos || open == std::string::npos || close == std::string::npos) return "";
    return json.substr(open + 1, close - open - 1);
}

// Reads a classpath resource through a loader, or the system loader when loader is null.
std::string read_resource(JNIEnv* env, jobject loader, const char* name) {
    jclass cl = env->FindClass("java/lang/ClassLoader");
    jstring jname = env->NewStringUTF(name);
    jobject stream;
    if (loader) {
        jmethodID m = env->GetMethodID(cl, "getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;");
        stream = env->CallObjectMethod(loader, m, jname);
    } else {
        jmethodID m = env->GetStaticMethodID(cl, "getSystemResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;");
        stream = env->CallStaticObjectMethod(cl, m, jname);
    }
    std::string out;
    if (env->ExceptionCheck() || !stream) {
        env->ExceptionClear();
        return out;
    }
    jclass is = env->FindClass("java/io/InputStream");
    jmethodID read = env->GetMethodID(is, "read", "([B)I");
    jmethodID close = env->GetMethodID(is, "close", "()V");
    jbyteArray buf = env->NewByteArray(8192);
    for (;;) {
        jint n = env->CallIntMethod(stream, read, buf);
        if (env->ExceptionCheck() || n <= 0) break;
        size_t at = out.size();
        out.resize(at + size_t(n));
        env->GetByteArrayRegion(buf, 0, n, reinterpret_cast<jbyte*>(&out[at]));
    }
    env->ExceptionClear();
    env->CallVoidMethod(stream, close);
    env->ExceptionClear();
    return out;
}

// Set while proceed() re-enters a hooked method, so its own prologue lets it run.
thread_local int g_bypass_site = -1;

class CallImpl final : public Call {
public:
    CallImpl(Core& core, JNIEnv* env, int site, jobject self, jobjectArray args)
        : core_(core), env_(env), site_(site), self_(self), args_(args) {}

    JNIEnv* env() override { return env_; }
    jobject self() override { return self_; }
    int arg_count() override { return args_ ? env_->GetArrayLength(args_) : 0; }
    jobject arg(int i) override {
        return i >= 0 && i < arg_count() ? env_->GetObjectArrayElement(args_, i) : nullptr;
    }
    jint arg_int(int i) override { return core_.unbox(env_, arg(i), 'I').i; }
    jlong arg_long(int i) override { return core_.unbox(env_, arg(i), 'J').j; }
    jfloat arg_float(int i) override { return core_.unbox(env_, arg(i), 'F').f; }
    jdouble arg_double(int i) override { return core_.unbox(env_, arg(i), 'D').d; }

    jobject proceed() override {
        jobject r = core_.invoke_original(env_, site_, self_, args_);
        if (env_->ExceptionCheck()) threw = true;  // leave it pending so it propagates from the method
        finish(r);
        return r;
    }

    void cancel() override { finish(nullptr); }
    void return_object(jobject v) override { finish(v); }
    void return_int(jint v) override { jvalue j; j.i = v; finish(core_.box(env_, "java/lang/Integer", "(I)Ljava/lang/Integer;", j)); }
    void return_long(jlong v) override { jvalue j; j.j = v; finish(core_.box(env_, "java/lang/Long", "(J)Ljava/lang/Long;", j)); }
    void return_float(jfloat v) override { jvalue j; j.f = v; finish(core_.box(env_, "java/lang/Float", "(F)Ljava/lang/Float;", j)); }
    void return_double(jdouble v) override { jvalue j; j.d = v; finish(core_.box(env_, "java/lang/Double", "(D)Ljava/lang/Double;", j)); }

    bool done = false;
    bool threw = false;
    jobject result = nullptr;

private:
    void finish(jobject v) { done = true; result = v; }
    Core& core_;
    JNIEnv* env_;
    int site_;
    jobject self_;
    jobjectArray args_;
};

jobject JNICALL bridge_enter(JNIEnv* env, jclass, jint site, jobject self, jobjectArray args) {
    return Core::get().on_enter(env, site, self, args);
}

}  // namespace

Core& Core::get() {
    static Core core;
    return core;
}

void Core::log(const std::string& message) {
    std::fprintf(stderr, "[UMC] %s\n", message.c_str());
    {
        std::lock_guard<std::mutex> lock(log_mu_);
        log_.push_back(message);
        ++log_seq_;
        if (log_.size() > 1000) log_.pop_front();
    }
    log_cv_.notify_all();
}

std::vector<std::string> Core::log_since(uint64_t& seq, int timeout_ms) {
    std::unique_lock<std::mutex> lock(log_mu_);
    log_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return log_seq_ > seq; });
    uint64_t missing = std::min<uint64_t>(log_seq_ - seq, log_.size());
    std::vector<std::string> out(log_.end() - long(missing), log_.end());
    seq = log_seq_;
    return out;
}

bool Core::start(JavaVM* vm, jvmtiEnv* jvmti, JNIEnv* env, const std::string& options) {
    vm_ = vm;
    jvmti_ = jvmti;

    std::stringstream ss(options);
    std::string kv;
    while (std::getline(ss, kv, ',')) {
        size_t eq = kv.find('=');
        if (eq != std::string::npos) options_[kv.substr(0, eq)] = kv.substr(eq + 1);
        else if (!kv.empty()) options_[kv] = "";
    }

    if (!define_bridge(env)) return false;
    detect_game(env);
    if (options_.count("mappings")) {
        std::string err;
        if (!load_mappings(options_["mappings"], err)) log("mappings: " + err);
    }
    started_ = true;
    log("injected: minecraft " + version_ + " " + side_name(side_) + " (" + loader_kind_ + ")");

    if (options_.count("plugins")) {
        std::stringstream ps(options_["plugins"]);
        std::string path, err;
        while (std::getline(ps, path, '|'))
            if (!path.empty() && !load_plugin(path, err)) log(err);
    }
    if (options_.count("enable")) {
        std::stringstream es(options_["enable"]);
        std::string name, msg;
        while (std::getline(es, name, '+')) {
            if (auto* slot = module_slot(name)) set_enabled(env, *slot, true, msg);
            else log("enable: no module named " + name);
        }
    }
    return true;
}

bool Core::define_bridge(JNIEnv* env) {
    jclass cls = env->FindClass("umc/Bridge");
    if (!cls) {
        env->ExceptionClear();
        // A null loader puts the class in the bootstrap loader, which every game class loader can reach.
        cls = env->DefineClass("umc/Bridge", nullptr, reinterpret_cast<const jbyte*>(kBridgeClass),
                               jsize(sizeof kBridgeClass));
        if (!cls) {
            log("failed to define umc.Bridge: " + java_exception(env));
            return false;
        }
    }
    JNINativeMethod natives[] = {
        {const_cast<char*>("enter"),
         const_cast<char*>("(ILjava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"),
         reinterpret_cast<void*>(&bridge_enter)}};
    if (env->RegisterNatives(cls, natives, 1) != 0) {
        log("failed to register bridge natives: " + java_exception(env));
        return false;
    }
    bridge_ = static_cast<jclass>(env->NewGlobalRef(cls));
    jfieldID f = env->GetStaticFieldID(bridge_, "CONTINUE", "Ljava/lang/Object;");
    continue_ = env->NewGlobalRef(env->GetStaticObjectField(bridge_, f));
    return true;
}

void Core::detect_game(JNIEnv* env) {
    std::vector<std::string> args = plat::self_args();
    std::string joined;
    for (auto& a : args) joined += a + " ";

    if (joined.find("net.fabricmc") != std::string::npos || joined.find("fabric-loader") != std::string::npos)
        loader_kind_ = "fabric";
    else if (joined.find("org.quiltmc") != std::string::npos) loader_kind_ = "quilt";
    else if (joined.find("neoforge") != std::string::npos) loader_kind_ = "neoforge";
    else if (joined.find("cpw.mods") != std::string::npos || joined.find("minecraftforge") != std::string::npos)
        loader_kind_ = "forge";

    if (options_.count("side")) {
        side_ = options_["side"] == "server" ? Side::Server : Side::Client;
    } else if (joined.find("--accessToken") != std::string::npos || joined.find("--assetsDir") != std::string::npos ||
               joined.find("net.minecraft.client.main.Main") != std::string::npos ||
               joined.find("KnotClient") != std::string::npos) {
        side_ = Side::Client;
    } else {
        side_ = Side::Server;
    }

    if (options_.count("version")) {
        version_ = options_["version"];
        return;
    }

    // Every jar since 1.14 ships version.json. Try the system loader, then loaders of known game classes.
    std::string json = read_resource(env, nullptr, "version.json");
    if (json.empty()) {
        static const char* entry_points[] = {
            "Lnet/minecraft/client/main/Main;", "Lnet/minecraft/server/Main;", "Lnet/minecraft/bundler/Main;",
            "Lnet/minecraft/client/Minecraft;", "Lnet/minecraft/server/MinecraftServer;"};
        jint count = 0;
        jclass* classes = nullptr;
        jvmti_->GetLoadedClasses(&count, &classes);
        for (jint i = 0; i < count; ++i) {
            char* sig = nullptr;
            if (json.empty() && jvmti_->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE) {
                for (const char* e : entry_points) {
                    if (std::strcmp(sig, e) != 0) continue;
                    jobject loader = nullptr;
                    jvmti_->GetClassLoader(classes[i], &loader);
                    if (loader) json = read_resource(env, loader, "version.json");
                    break;
                }
            }
            if (sig) jvmti_->Deallocate(reinterpret_cast<unsigned char*>(sig));
            env->DeleteLocalRef(classes[i]);
        }
        jvmti_->Deallocate(reinterpret_cast<unsigned char*>(classes));
    }
    std::string id = json_string(json, "id");
    if (!id.empty()) {
        version_ = id;
        return;
    }
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--version") version_ = args[i + 1];
}

bool Core::load_mappings(const std::string& path, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!hooks_.empty()) {
        error = "remove hooks before changing mappings";
        return false;
    }
    Mappings fresh;
    if (!fresh.load_proguard(path, error)) return false;
    mappings_ = std::move(fresh);
    mappings_source_ = path;
    class_cache_.clear();
    log("loaded " + std::to_string(mappings_.size()) + " class mappings from " + path);
    if (loader_kind_ == "fabric" || loader_kind_ == "quilt" || loader_kind_ == "forge")
        log("warning: " + loader_kind_ + " remaps the game at runtime, Mojang -> obfuscated mappings won't match");
    return true;
}

bool Core::load_plugin(const std::string& path, std::string& error) {
    void* handle = plat::lib_open(path, error);
    if (!handle) {
        error = "plugin: " + error;
        return false;
    }
    auto reg = reinterpret_cast<void (*)(Api*)>(plat::lib_symbol(handle, "umc_register"));
    if (!reg) {
        error = "plugin " + path + " does not export umc_register";
        plat::lib_close(handle);
        return false;
    }
    size_t before = modules_.size();
    reg(this);
    plugins_.push_back({path, handle});
    for (size_t i = before; i < modules_.size(); ++i) modules_[i].origin = path;
    log("loaded plugin " + path + " (" + std::to_string(modules_.size() - before) + " modules)");
    return true;
}

void Core::add_module(std::unique_ptr<Module> module) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!module) return;
    if (module_slot(module->name())) {
        log("module " + module->name() + " already registered, skipping");
        return;
    }
    modules_.push_back({std::move(module), false, "builtin"});
}

Core::ModuleSlot* Core::module_slot(const std::string& name) {
    for (auto& m : modules_)
        if (m.module->name() == name) return &m;
    return nullptr;
}

bool Core::set_enabled(JNIEnv* env, ModuleSlot& slot, bool enabled, std::string& msg) {
    const std::string name = slot.module->name();
    if (slot.enabled == enabled) {
        msg = name + " is already " + (enabled ? "enabled" : "disabled");
        return true;
    }
    try {
        if (enabled) {
            slot.module->on_enable(*this, env);
        } else {
            slot.module->on_disable(*this, env);
            for (int id : slot.module->owned_hooks()) unhook(id);
            slot.module->owned_hooks().clear();
        }
    } catch (const std::exception& e) {
        msg = name + ": " + e.what();
        log(msg);
        return false;
    }
    slot.enabled = enabled;
    msg = name + (enabled ? " enabled" : " disabled");
    log(msg);
    return true;
}

std::string Core::runtime_class_name(const std::string& named) {
    return mappings_.to_obf(internal(named));
}

std::string Core::signature_name(jclass cls) {
    char* sig = nullptr;
    if (jvmti_->GetClassSignature(cls, &sig, nullptr) != JVMTI_ERROR_NONE || !sig) return "";
    std::string s = sig;
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(sig));
    if (s.size() > 2 && s[0] == 'L') return s.substr(1, s.size() - 2);
    return s;
}

jclass Core::scan_loaded(JNIEnv* env, const std::string& runtime_class) {
    const std::string want = "L" + runtime_class + ";";
    jint count = 0;
    jclass* classes = nullptr;
    if (jvmti_->GetLoadedClasses(&count, &classes) != JVMTI_ERROR_NONE) return nullptr;
    jclass found = nullptr;
    for (jint i = 0; i < count; ++i) {
        char* sig = nullptr;
        if (!found && jvmti_->GetClassSignature(classes[i], &sig, nullptr) == JVMTI_ERROR_NONE && want == sig)
            found = classes[i];
        else
            env->DeleteLocalRef(classes[i]);
        if (sig) jvmti_->Deallocate(reinterpret_cast<unsigned char*>(sig));
    }
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(classes));
    return found;
}

jclass Core::find_class(JNIEnv* env, const std::string& owner) {
    const std::string rt = runtime_class_name(owner);
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = class_cache_.find(rt);
    if (it != class_cache_.end()) return it->second;
    jclass cls = scan_loaded(env, rt);
    if (!cls) return nullptr;
    jclass global = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
    class_cache_[rt] = global;
    return global;
}

jmethodID Core::find_method(JNIEnv* env, const std::string& owner, const std::string& name, const std::string& desc) {
    return find_method_in(env, owner, name, desc, nullptr);
}

jmethodID Core::find_method_in(JNIEnv* env, const std::string& owner, const std::string& name, const std::string& desc,
                          jclass* declaring) {
    jclass start = find_class(env, owner);
    if (!start) return nullptr;
    jclass cls = static_cast<jclass>(env->NewLocalRef(start));
    while (cls) {
        std::string named = mappings_.to_named(signature_name(cls));
        std::vector<std::pair<std::string, std::string>> wanted;
        if (mappings_.by_named(named)) {
            for (auto* m : mappings_.methods(named, name, desc)) wanted.push_back({m->obf_name, m->obf_desc});
        } else {
            wanted.push_back({name, mappings_.remap_desc(desc)});
        }
        jint count = 0;
        jmethodID* methods = nullptr;
        jmethodID hit = nullptr;
        if (!wanted.empty() && jvmti_->GetClassMethods(cls, &count, &methods) == JVMTI_ERROR_NONE) {
            for (jint i = 0; i < count && !hit; ++i) {
                char *mn = nullptr, *ms = nullptr;
                if (jvmti_->GetMethodName(methods[i], &mn, &ms, nullptr) != JVMTI_ERROR_NONE) continue;
                for (auto& w : wanted)
                    if (w.first == mn && (w.second.empty() || w.second == ms)) hit = methods[i];
                jvmti_->Deallocate(reinterpret_cast<unsigned char*>(mn));
                jvmti_->Deallocate(reinterpret_cast<unsigned char*>(ms));
            }
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(methods));
        }
        jclass super = env->GetSuperclass(cls);
        if (hit && declaring) *declaring = static_cast<jclass>(env->NewLocalRef(cls));
        env->DeleteLocalRef(cls);
        if (hit) {
            if (super) env->DeleteLocalRef(super);
            return hit;
        }
        cls = super;
    }
    return nullptr;
}

jfieldID Core::find_field(JNIEnv* env, const std::string& owner, const std::string& name, const std::string& desc) {
    return find_field_in(env, owner, name, desc, nullptr);
}

jfieldID Core::find_field_in(JNIEnv* env, const std::string& owner, const std::string& name, const std::string& desc,
                          jclass* declaring) {
    jclass start = find_class(env, owner);
    if (!start) return nullptr;
    jclass cls = static_cast<jclass>(env->NewLocalRef(start));
    while (cls) {
        std::string named = mappings_.to_named(signature_name(cls));
        std::string want_name = name, want_desc = mappings_.remap_desc(desc);
        bool ok = true;
        if (mappings_.by_named(named)) {
            const MemberMapping* f = mappings_.field(named, name);
            ok = f != nullptr;
            if (f) { want_name = f->obf_name; want_desc = desc.empty() ? "" : f->obf_desc; }
        }
        jint count = 0;
        jfieldID* fields = nullptr;
        jfieldID hit = nullptr;
        if (ok && jvmti_->GetClassFields(cls, &count, &fields) == JVMTI_ERROR_NONE) {
            for (jint i = 0; i < count && !hit; ++i) {
                char *fn = nullptr, *fs = nullptr;
                if (jvmti_->GetFieldName(cls, fields[i], &fn, &fs, nullptr) != JVMTI_ERROR_NONE) continue;
                if (want_name == fn && (want_desc.empty() || want_desc == fs)) hit = fields[i];
                jvmti_->Deallocate(reinterpret_cast<unsigned char*>(fn));
                jvmti_->Deallocate(reinterpret_cast<unsigned char*>(fs));
            }
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(fields));
        }
        jclass super = env->GetSuperclass(cls);
        if (hit && declaring) *declaring = static_cast<jclass>(env->NewLocalRef(cls));
        env->DeleteLocalRef(cls);
        if (hit) {
            if (super) env->DeleteLocalRef(super);
            return hit;
        }
        cls = super;
    }
    return nullptr;
}

void Core::add_bridge_read(JNIEnv* env, jclass cls) {
    // Named modules (NeoForge, Forge) don't read the bootstrap unnamed module by default.
    jint jvmti_version = 0;
    jvmti_->GetVersionNumber(&jvmti_version);
    if (env->GetVersion() < JNI_VERSION_9 || (jvmti_version & 0x0FFF0000) < 0x00090000) return;
    jobject from = env->GetModule(cls);
    jobject to = env->GetModule(bridge_);
    if (from && to) jvmti_->AddModuleReads(from, to);
    env->ExceptionClear();
}

std::string Core::retransform(JNIEnv* env, const std::string& runtime_class) {
    jclass cls = find_class(env, runtime_class);
    if (!cls) return "pending (class not loaded yet)";
    jboolean modifiable = JNI_FALSE;
    jvmti_->IsModifiableClass(cls, &modifiable);
    if (!modifiable) return "error: class cannot be modified";
    add_bridge_read(env, cls);
    jvmtiError err = jvmti_->RetransformClasses(1, &cls);
    if (err != JVMTI_ERROR_NONE) {
        char* name = nullptr;
        jvmti_->GetErrorName(err, &name);
        std::string msg = std::string("error: retransform failed (") + (name ? name : "?") + ")";
        if (name) jvmti_->Deallocate(reinterpret_cast<unsigned char*>(name));
        env->ExceptionClear();
        return msg;
    }
    return "active";
}

int Core::hook(const std::string& owner, const std::string& method, const std::string& desc, HookFn fn) {
    const std::string named = internal(owner);
    HookEntry entry{0, named, runtime_class_name(named), method, desc, {}, std::make_shared<HookFn>(std::move(fn))};

    if (method == "<init>" || method == "<clinit>") {
        log("hook " + dotted(named) + "." + method + ": constructors can't be hooked");
        return -1;
    }
    if (mappings_.by_named(named)) {
        for (auto* m : mappings_.methods(named, method, desc)) entry.targets.push_back({m->obf_name, m->obf_desc});
        if (entry.targets.empty()) {
            log("hook " + dotted(named) + "." + method + desc + ": no such method in mappings");
            return -1;
        }
    } else {
        entry.targets.push_back({method, mappings_.remap_desc(desc)});
    }

    int id;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        id = entry.id = next_hook_++;
        hooks_[id] = entry;
    }

    JNIEnv* env = nullptr;
    vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    std::string state = env ? retransform(env, entry.owner_rt) : "pending";
    if (state.rfind("error", 0) == 0) {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        hooks_.erase(id);
        log("hook " + dotted(named) + "." + method + ": " + state);
        return -1;
    }
    log("hook #" + std::to_string(id) + " " + dotted(named) + "." + method + desc + ": " + state);
    return id;
}

void Core::unhook(int id) {
    std::string rt;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        auto it = hooks_.find(id);
        if (it == hooks_.end()) return;
        rt = it->second.owner_rt;
        hooks_.erase(it);
        for (auto& s : sites_) s.hooks.erase(std::remove(s.hooks.begin(), s.hooks.end(), id), s.hooks.end());
    }
    JNIEnv* env = nullptr;
    vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (env) retransform(env, rt);
    log("unhooked #" + std::to_string(id));
}

void Core::on_class_file_load(const char* name, jint len, const unsigned char* data, jint* new_len,
                              unsigned char** new_data) {
    if (!name || !started_) return;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!capture_class_.empty() && capture_class_ == name) captured_.assign(data, data + len);
    std::vector<const HookEntry*> mine;
    for (auto& [id, h] : hooks_)
        if (h.owner_rt == name) mine.push_back(&h);
    if (mine.empty()) return;

    SiteResolver resolve = [&](const std::string& mname, const std::string& mdesc) -> int {
        std::vector<int> ids;
        for (auto* h : mine)
            for (auto& t : h->targets)
                if (t.first == mname && (t.second.empty() || t.second == mdesc)) { ids.push_back(h->id); break; }
        if (ids.empty()) return -1;
        for (auto& s : sites_) {
            if (s.cls == name && s.name == mname && s.desc == mdesc) {
                s.hooks = ids;
                return s.id;
            }
        }
        sites_.push_back({int(sites_.size()), name, mname, mdesc, ids});
        return sites_.back().id;
    };

    std::vector<uint8_t> out;
    std::vector<std::string> patched;
    std::string err;
    if (!inject_hooks(data, size_t(len), resolve, out, patched, err)) {
        log(std::string("cannot patch ") + name + ": " + err);
        return;
    }
    if (patched.empty()) {
        log(std::string("no hookable methods matched in ") + name);
        return;
    }
    unsigned char* mem = nullptr;
    if (jvmti_->Allocate(jlong(out.size()), &mem) != JVMTI_ERROR_NONE) return;
    std::memcpy(mem, out.data(), out.size());
    *new_len = jint(out.size());
    *new_data = mem;
    for (auto& p : patched) log(std::string("patched ") + dotted(name) + "." + p);
}

void Core::on_class_prepare(JNIEnv* env, jclass cls) {
    if (!started_) return;
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (hooks_.empty()) return;
        name = signature_name(cls);
        bool hooked = false;
        for (auto& [id, h] : hooks_) hooked |= h.owner_rt == name;
        if (!hooked) return;
    }
    add_bridge_read(env, cls);
}

jobject Core::on_enter(JNIEnv* env, jint site, jobject self, jobjectArray args) {
    if (g_bypass_site == site) {
        g_bypass_site = -1;
        return env->NewLocalRef(continue_);
    }
    std::vector<std::shared_ptr<HookFn>> fns;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (site >= 0 && size_t(site) < sites_.size())
            for (int id : sites_[size_t(site)].hooks) {
                auto it = hooks_.find(id);
                if (it != hooks_.end()) fns.push_back(it->second.fn);
            }
    }
    CallImpl call(*this, env, site, self, args);
    for (auto& fn : fns) {
        try {
            (*fn)(call);
        } catch (const std::exception& e) {
            log(std::string("hook threw: ") + e.what());
        }
        if (call.threw) return nullptr;  // the original's exception propagates out of the method
        if (env->ExceptionCheck()) log("hook left a java exception: " + java_exception(env));
        if (call.done) break;
    }
    return call.done ? call.result : env->NewLocalRef(continue_);
}

jobject Core::invoke_original(JNIEnv* env, int site, jobject self, jobjectArray args) {
    jclass cls = nullptr;
    jmethodID mid = nullptr;
    bool is_static = false;
    std::string desc;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (site < 0 || size_t(site) >= sites_.size()) return nullptr;
        Site& s = sites_[size_t(site)];
        if (!s.method) {
            jclass c = find_class(env, s.cls);
            jint count = 0;
            jmethodID* methods = nullptr;
            if (c && jvmti_->GetClassMethods(c, &count, &methods) == JVMTI_ERROR_NONE) {
                for (jint i = 0; i < count && !s.method; ++i) {
                    char *n = nullptr, *sig = nullptr;
                    jvmti_->GetMethodName(methods[i], &n, &sig, nullptr);
                    if (s.name == n && s.desc == sig) {
                        jint mods = 0;
                        jvmti_->GetMethodModifiers(methods[i], &mods);
                        s.method = methods[i];
                        s.is_static = mods & 0x0008;
                        s.owner = c;
                    }
                    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(n));
                    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(sig));
                }
                jvmti_->Deallocate(reinterpret_cast<unsigned char*>(methods));
            }
            if (!s.method) {
                log("proceed: cannot resolve " + dotted(s.cls) + "." + s.name);
                return nullptr;
            }
        }
        cls = s.owner;
        mid = s.method;
        is_static = s.is_static;
        desc = s.desc;
    }

    // Unbox the Object[] the prologue built back into the method's parameter types.
    std::vector<jvalue> values;
    size_t i = 1;
    int index = 0;
    char ret = 'V';
    while (i < desc.size()) {
        if (desc[i] == ')') {
            ret = desc[i + 1];
            break;
        }
        size_t start = i;
        while (desc[i] == '[') ++i;
        if (desc[i] == 'L') i = desc.find(';', i);
        ++i;
        jobject a = env->GetObjectArrayElement(args, index++);
        jvalue v{};
        switch (i - start == 1 ? desc[start] : 'L') {
        case 'Z': v.z = unbox(env, a, 'I').i != 0; break;
        case 'B': v.b = jbyte(unbox(env, a, 'I').i); break;
        case 'C': v.c = jchar(unbox(env, a, 'I').i); break;
        case 'S': v.s = jshort(unbox(env, a, 'I').i); break;
        case 'I': v.i = unbox(env, a, 'I').i; break;
        case 'J': v.j = unbox(env, a, 'J').j; break;
        case 'F': v.f = unbox(env, a, 'F').f; break;
        case 'D': v.d = unbox(env, a, 'D').d; break;
        default: v.l = a;
        }
        values.push_back(v);
    }
    if (ret == '[') ret = 'L';

    const jvalue* a = values.data();
    jvalue r{};
    jobject boxed = nullptr;
    g_bypass_site = site;
#define UMC_CALL(Type) (is_static ? env->CallStatic##Type##MethodA(cls, mid, a) : env->CallNonvirtual##Type##MethodA(self, cls, mid, a))
    switch (ret) {
    case 'V': UMC_CALL(Void); break;
    case 'Z': r.z = UMC_CALL(Boolean); boxed = box(env, "java/lang/Boolean", "(Z)Ljava/lang/Boolean;", r); break;
    case 'B': r.i = UMC_CALL(Byte); break;
    case 'C': r.i = UMC_CALL(Char); break;
    case 'S': r.i = UMC_CALL(Short); break;
    case 'I': r.i = UMC_CALL(Int); break;
    case 'J': r.j = UMC_CALL(Long); boxed = box(env, "java/lang/Long", "(J)Ljava/lang/Long;", r); break;
    case 'F': r.f = UMC_CALL(Float); boxed = box(env, "java/lang/Float", "(F)Ljava/lang/Float;", r); break;
    case 'D': r.d = UMC_CALL(Double); boxed = box(env, "java/lang/Double", "(D)Ljava/lang/Double;", r); break;
    default: boxed = UMC_CALL(Object);
    }
#undef UMC_CALL
    g_bypass_site = -1;
    // Byte/char/short/int all come back through Bridge.asInt, so an Integer serves for each.
    if (std::string("BCSI").find(ret) != std::string::npos && !env->ExceptionCheck())
        boxed = box(env, "java/lang/Integer", "(I)Ljava/lang/Integer;", r);
    return boxed;
}

jobject Core::box(JNIEnv* env, const char* cls, const char* sig, jvalue v) {
    jclass c = env->FindClass(cls);
    jmethodID m = env->GetStaticMethodID(c, "valueOf", sig);
    jobject o = env->CallStaticObjectMethodA(c, m, &v);
    env->DeleteLocalRef(c);
    return o;
}

jvalue Core::unbox(JNIEnv* env, jobject o, char type) {
    jvalue v{};
    switch (type) {
    case 'J': v.j = env->CallStaticLongMethod(bridge_, env->GetStaticMethodID(bridge_, "asLong", "(Ljava/lang/Object;)J"), o); break;
    case 'F': v.f = env->CallStaticFloatMethod(bridge_, env->GetStaticMethodID(bridge_, "asFloat", "(Ljava/lang/Object;)F"), o); break;
    case 'D': v.d = env->CallStaticDoubleMethod(bridge_, env->GetStaticMethodID(bridge_, "asDouble", "(Ljava/lang/Object;)D"), o); break;
    default: v.i = env->CallStaticIntMethod(bridge_, env->GetStaticMethodID(bridge_, "asInt", "(Ljava/lang/Object;)I"), o);
    }
    return v;
}

}  // namespace umc
