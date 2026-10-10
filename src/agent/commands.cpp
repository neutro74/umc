// Control socket commands handled inside the game.
#include <algorithm>
#include <fstream>
#include <sstream>

#include "agent/core.hpp"
#include "agent/game.hpp"
#include "agent/scripts.hpp"
#include "agent/util.hpp"
#include "common/platform.hpp"

namespace umc {

namespace {

constexpr jint ACC_STATIC = 0x0008;

std::string first_type(const std::string& sig, size_t& i) {
    size_t start = i;
    while (i < sig.size() && sig[i] == '[') ++i;
    if (i < sig.size() && sig[i] == 'L') i = sig.find(';', i);
    ++i;
    return sig.substr(start, i - start);
}

void split_sig(const std::string& sig, std::vector<std::string>& params, std::string& ret) {
    size_t i = 1;
    while (i < sig.size() && sig[i] != ')') params.push_back(first_type(sig, i));
    ++i;
    ret = first_type(sig, i);
}

std::string java_type(const std::string& t) {
    std::string base = t, dims;
    while (!base.empty() && base[0] == '[') { base = base.substr(1); dims += "[]"; }
    switch (base.empty() ? 'V' : base[0]) {
    case 'Z': return "boolean" + dims;
    case 'B': return "byte" + dims;
    case 'C': return "char" + dims;
    case 'S': return "short" + dims;
    case 'I': return "int" + dims;
    case 'J': return "long" + dims;
    case 'F': return "float" + dims;
    case 'D': return "double" + dims;
    case 'V': return "void";
    default: return dotted(base.substr(1, base.size() - 2)) + dims;
    }
}

bool parse_value(JNIEnv* env, const std::string& type, const std::string& text, jvalue& v, std::string& err) {
    try {
        switch (type[0]) {
        case 'Z': v.z = (text == "true" || text == "1") ? JNI_TRUE : JNI_FALSE; return true;
        case 'B': v.b = jbyte(std::stoi(text)); return true;
        case 'C': v.c = text.empty() ? 0 : jchar(text[0]); return true;
        case 'S': v.s = jshort(std::stoi(text)); return true;
        case 'I': v.i = std::stoi(text); return true;
        case 'J': v.j = std::stoll(text); return true;
        case 'F': v.f = std::stof(text); return true;
        case 'D': v.d = std::stod(text); return true;
        }
    } catch (const std::exception&) {
        err = "cannot parse '" + text + "' as " + java_type(type);
        return false;
    }
    if (text == "null") {
        v.l = nullptr;
        return true;
    }
    if (type == "Ljava/lang/String;" || type == "Ljava/lang/Object;" || type == "Ljava/lang/CharSequence;") {
        v.l = env->NewStringUTF(text.c_str());
        return true;
    }
    err = java_type(type) + " values can only be null from the CLI";
    return false;
}

}  // namespace

std::string Core::describe(JNIEnv* env, jobject o) {
    if (!o) return "null";
    jclass obj = env->FindClass("java/lang/Object");
    jmethodID to_string = env->GetMethodID(obj, "toString", "()Ljava/lang/String;");
    auto s = static_cast<jstring>(env->CallObjectMethod(o, to_string));
    if (env->ExceptionCheck()) return "<toString threw: " + java_exception(env) + ">";
    if (!s) return "null";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c;
    env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
    if (out.size() > 300) out = out.substr(0, 297) + "...";
    return out;
}

// Finds the deobfuscated name of a runtime member, or returns the runtime name.
std::string Core::named_member(const std::string& rt_class, const std::string& name, const std::string& sig,
                               bool method) {
    if (const ClassMapping* c = mappings_.by_named(mappings_.to_named(rt_class)))
        for (auto& m : method ? c->methods : c->fields)
            if (m.obf_name == name && m.obf_desc == sig) return m.name;
    return name;
}

std::string Core::cmd_status() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    std::ostringstream o;
    o << "pid       " << plat::current_pid() << "\n"
      << "minecraft " << version_ << " (" << side_name(side_) << ", " << loader_kind_ << ")\n"
      << "mappings  " << (mappings_.empty() ? "none (names used as-is)" : std::to_string(mappings_.size()) + " classes, " + mappings_source_) << "\n"
      << "hooks     " << hooks_.size() << " (" << traces_.size() << " traces)\n";
    size_t on = 0;
    for (auto& m : modules_) on += m.enabled;
    o << "modules   " << modules_.size() << " (" << on << " enabled), " << plugins_.size() << " plugins\n";
    return o.str();
}

std::string Core::cmd_hooks() {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (hooks_.empty()) return "no hooks\n";
    std::ostringstream o;
    for (auto& [id, h] : hooks_) {
        int sites = 0;
        for (auto& s : sites_) sites += std::count(s.hooks.begin(), s.hooks.end(), id) > 0;
        o << "#" << id << (traces_.count(id) ? " trace" : "") << "  " << dotted(h.owner_named) << "."
          << h.method_named << h.desc_named << "  -> " << dotted(h.owner_rt) << "  "
          << (sites ? std::to_string(sites) + " site(s) patched" : "pending") << "\n";
    }
    return o.str();
}

std::string Core::cmd_map(const std::vector<std::string>& args) {
    if (args.size() < 2) return "usage: map <class> [member]\n";
    std::string named = internal(args[1]);
    const ClassMapping* c = mappings_.by_named(named);
    if (!c) {
        std::string back = mappings_.to_named(named);
        if (back != named) return dotted(named) + " <- " + dotted(back) + "\n";
        return dotted(named) + " is not in the mappings (used as-is)\n";
    }
    std::ostringstream o;
    o << dotted(c->named) << " -> " << dotted(c->obf) << "\n";
    for (auto* list : {&c->fields, &c->methods})
        for (auto& m : *list)
            if (args.size() < 3 || m.name == args[2])
                o << "  " << m.name << " " << m.desc << " -> " << m.obf_name << " " << m.obf_desc << "\n";
    return o.str();
}

std::string Core::cmd_classes(JNIEnv* env, const std::string& filter) {
    jint count = 0;
    jclass* classes = nullptr;
    jvmti_->GetLoadedClasses(&count, &classes);
    std::vector<std::string> lines;
    for (jint i = 0; i < count; ++i) {
        std::string rt = signature_name(classes[i]);
        env->DeleteLocalRef(classes[i]);
        std::string named = mappings_.to_named(rt);
        if (named.find(internal(filter)) == std::string::npos && rt.find(internal(filter)) == std::string::npos) continue;
        lines.push_back(named == rt ? dotted(rt) : dotted(named) + "  (" + dotted(rt) + ")");
    }
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(classes));
    std::sort(lines.begin(), lines.end());
    std::ostringstream o;
    for (size_t i = 0; i < lines.size() && i < 300; ++i) o << lines[i] << "\n";
    if (lines.size() > 300) o << "... " << lines.size() - 300 << " more, narrow the filter\n";
    o << lines.size() << " of " << count << " loaded classes\n";
    return o.str();
}

std::string Core::cmd_members(JNIEnv* env, const std::vector<std::string>& args, bool methods) {
    if (args.size() < 2) return std::string("usage: ") + (methods ? "methods" : "fields") + " <class> [filter]\n";
    jclass cls = find_class(env, args[1]);
    if (!cls) return "class not loaded: " + args[1] + "\n";
    const std::string rt = signature_name(cls);
    const std::string filter = args.size() > 2 ? args[2] : "";
    std::ostringstream o;
    jint count = 0;
    if (methods) {
        jmethodID* list = nullptr;
        jvmti_->GetClassMethods(cls, &count, &list);
        for (jint i = 0; i < count; ++i) {
            char *n = nullptr, *s = nullptr;
            jint mods = 0;
            jvmti_->GetMethodName(list[i], &n, &s, nullptr);
            jvmti_->GetMethodModifiers(list[i], &mods);
            std::string named = named_member(rt, n, s, true);
            std::vector<std::string> params;
            std::string ret;
            split_sig(s, params, ret);
            std::string line = std::string(mods & ACC_STATIC ? "static " : "") + java_type(ret) + " " + named + "(";
            for (size_t p = 0; p < params.size(); ++p) line += (p ? ", " : "") + java_type(params[p]);
            line += ")";
            if (named != n) line += "  [" + std::string(n) + "]";
            if (filter.empty() || line.find(filter) != std::string::npos) o << "  " << line << "\n";
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(n));
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(s));
        }
        jvmti_->Deallocate(reinterpret_cast<unsigned char*>(list));
    } else {
        jfieldID* list = nullptr;
        jvmti_->GetClassFields(cls, &count, &list);
        for (jint i = 0; i < count; ++i) {
            char *n = nullptr, *s = nullptr;
            jint mods = 0;
            jvmti_->GetFieldName(cls, list[i], &n, &s, nullptr);
            jvmti_->GetFieldModifiers(cls, list[i], &mods);
            std::string named = named_member(rt, n, s, false);
            std::string line = std::string(mods & ACC_STATIC ? "static " : "") + java_type(s) + " " + named;
            if (named != n) line += "  [" + std::string(n) + "]";
            if (filter.empty() || line.find(filter) != std::string::npos) o << "  " << line << "\n";
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(n));
            jvmti_->Deallocate(reinterpret_cast<unsigned char*>(s));
        }
        jvmti_->Deallocate(reinterpret_cast<unsigned char*>(list));
    }
    jclass super = env->GetSuperclass(cls);
    std::string header = dotted(mappings_.to_named(rt)) + (super ? " extends " + dotted(mappings_.to_named(signature_name(super))) : "");
    return header + "\n" + o.str();
}

std::pair<bool, std::string> Core::cmd_get_set(JNIEnv* env, const std::vector<std::string>& args, bool set) {
    if (args.size() < (set ? 4u : 3u)) return {false, set ? "usage: set <class> <static field> <value>\n" : "usage: get <class> <static field>\n"};
    jclass decl = nullptr;
    jfieldID f = find_field_in(env, args[1], args[2], "", &decl);
    if (!f) return {false, "no field " + args[2] + " in " + args[1] + " (is the class loaded?)\n"};
    char *n = nullptr, *sig_c = nullptr;
    jint mods = 0;
    jvmti_->GetFieldName(decl, f, &n, &sig_c, nullptr);
    jvmti_->GetFieldModifiers(decl, f, &mods);
    std::string sig = sig_c;
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(n));
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(sig_c));
    if (!(mods & ACC_STATIC)) return {false, args[2] + " is an instance field; use a module to reach instances\n"};

    if (set) {
        jvalue v{};
        std::string err;
        if (!parse_value(env, sig, args[3], v, err)) return {false, err + "\n"};
        switch (sig[0]) {
        case 'Z': env->SetStaticBooleanField(decl, f, v.z); break;
        case 'B': env->SetStaticByteField(decl, f, v.b); break;
        case 'C': env->SetStaticCharField(decl, f, v.c); break;
        case 'S': env->SetStaticShortField(decl, f, v.s); break;
        case 'I': env->SetStaticIntField(decl, f, v.i); break;
        case 'J': env->SetStaticLongField(decl, f, v.j); break;
        case 'F': env->SetStaticFloatField(decl, f, v.f); break;
        case 'D': env->SetStaticDoubleField(decl, f, v.d); break;
        default: env->SetStaticObjectField(decl, f, v.l);
        }
    }
    std::string value;
    switch (sig[0]) {
    case 'Z': value = env->GetStaticBooleanField(decl, f) ? "true" : "false"; break;
    case 'B': value = std::to_string(env->GetStaticByteField(decl, f)); break;
    case 'C': value = std::string(1, char(env->GetStaticCharField(decl, f))); break;
    case 'S': value = std::to_string(env->GetStaticShortField(decl, f)); break;
    case 'I': value = std::to_string(env->GetStaticIntField(decl, f)); break;
    case 'J': value = std::to_string(env->GetStaticLongField(decl, f)); break;
    case 'F': value = std::to_string(env->GetStaticFloatField(decl, f)); break;
    case 'D': value = std::to_string(env->GetStaticDoubleField(decl, f)); break;
    default: value = describe(env, env->GetStaticObjectField(decl, f));
    }
    return {true, java_type(sig) + " " + args[2] + " = " + value + "\n"};
}

std::pair<bool, std::string> Core::cmd_call(JNIEnv* env, const std::vector<std::string>& args) {
    if (args.size() < 3) return {false, "usage: call <class> <static method> [args...]\n"};
    jclass decl = nullptr;
    jmethodID m = find_method_in(env, args[1], args[2], "", &decl);
    if (!m) return {false, "no method " + args[2] + " in " + args[1] + " (is the class loaded?)\n"};
    char *n = nullptr, *sig_c = nullptr;
    jint mods = 0;
    jvmti_->GetMethodName(m, &n, &sig_c, nullptr);
    jvmti_->GetMethodModifiers(m, &mods);
    std::string sig = sig_c;
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(n));
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(sig_c));
    if (!(mods & ACC_STATIC)) return {false, args[2] + " is an instance method; use a module to reach instances\n"};

    std::vector<std::string> params;
    std::string ret;
    split_sig(sig, params, ret);
    if (params.size() != args.size() - 3)
        return {false, args[2] + " takes " + std::to_string(params.size()) + " argument(s): " + sig + "\n"};
    std::vector<jvalue> values(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        std::string err;
        if (!parse_value(env, params[i], args[3 + i], values[i], err)) return {false, err + "\n"};
    }
    const jvalue* a = values.data();
    std::string out;
    switch (ret[0]) {
    case 'V': env->CallStaticVoidMethodA(decl, m, a); out = "done"; break;
    case 'Z': out = env->CallStaticBooleanMethodA(decl, m, a) ? "true" : "false"; break;
    case 'B': out = std::to_string(env->CallStaticByteMethodA(decl, m, a)); break;
    case 'C': out = std::string(1, char(env->CallStaticCharMethodA(decl, m, a))); break;
    case 'S': out = std::to_string(env->CallStaticShortMethodA(decl, m, a)); break;
    case 'I': out = std::to_string(env->CallStaticIntMethodA(decl, m, a)); break;
    case 'J': out = std::to_string(env->CallStaticLongMethodA(decl, m, a)); break;
    case 'F': out = std::to_string(env->CallStaticFloatMethodA(decl, m, a)); break;
    case 'D': out = std::to_string(env->CallStaticDoubleMethodA(decl, m, a)); break;
    default: {
        jobject r = env->CallStaticObjectMethodA(decl, m, a);
        out = env->ExceptionCheck() ? "" : describe(env, r);
    }
    }
    if (env->ExceptionCheck()) return {false, "threw " + java_exception(env) + "\n"};
    return {true, out + "\n"};
}

std::pair<bool, std::string> Core::cmd_trace(const std::vector<std::string>& args) {
    if (args.size() < 3) return {false, "usage: trace <class> <method> [desc]\n"};
    const std::string label = dotted(internal(args[1])) + "." + args[2];
    int id = hook(args[1], args[2], args.size() > 3 ? args[3] : "", [this, label](Call& call) {
        std::string line = "trace " + label + "(";
        for (int i = 0; i < call.arg_count(); ++i) {
            jobject a = call.arg(i);
            line += (i ? ", " : "") + describe(call.env(), a);
            call.env()->DeleteLocalRef(a);
        }
        log(line + ")");
    });
    if (id < 0) return {false, "could not trace " + label + " (see log)\n"};
    std::lock_guard<std::recursive_mutex> lock(mu_);
    traces_.insert(id);
    return {true, "tracing " + label + " as #" + std::to_string(id) + ", watch with `log` or `follow`\n"};
}

std::pair<bool, std::string> Core::cmd_dump(JNIEnv* env, const std::vector<std::string>& args) {
    if (args.size() < 3) return {false, "usage: dump <class> <file.class>\n"};
    jclass cls = find_class(env, args[1]);
    if (!cls) return {false, "class not loaded: " + args[1] + "\n"};
    std::vector<uint8_t> bytes;
    {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        capture_class_ = signature_name(cls);
        captured_.clear();
        jvmtiError err = jvmti_->RetransformClasses(1, &cls);
        capture_class_.clear();
        bytes.swap(captured_);
        if (err != JVMTI_ERROR_NONE) return {false, "class cannot be retransformed\n"};
    }
    std::ofstream out(args[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), long(bytes.size()));
    if (!out) return {false, "cannot write " + args[2] + "\n"};
    return {true, "wrote " + std::to_string(bytes.size()) + " bytes to " + args[2] + "\n"};
}

std::string Core::cmd_threads() {
    jint count = 0;
    jthread* threads = nullptr;
    jvmti_->GetAllThreads(&count, &threads);
    JNIEnv* env = nullptr;
    vm_->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    std::ostringstream o;
    for (jint i = 0; i < count; ++i) {
        jvmtiThreadInfo info{};
        jint state = 0;
        jvmti_->GetThreadInfo(threads[i], &info);
        jvmti_->GetThreadState(threads[i], &state);
        const char* s = (state & JVMTI_THREAD_STATE_RUNNABLE) ? "running"
                        : (state & JVMTI_THREAD_STATE_BLOCKED_ON_MONITOR_ENTER) ? "blocked"
                        : (state & JVMTI_THREAD_STATE_SLEEPING) ? "sleeping"
                        : (state & JVMTI_THREAD_STATE_WAITING) ? "waiting" : "other";
        o << "  " << (info.name ? info.name : "?") << "  " << s << (info.is_daemon ? "  daemon" : "") << "\n";
        if (info.name) jvmti_->Deallocate(reinterpret_cast<unsigned char*>(info.name));
        if (env) {
            env->DeleteLocalRef(info.thread_group);
            env->DeleteLocalRef(info.context_class_loader);
            env->DeleteLocalRef(threads[i]);
        }
    }
    jvmti_->Deallocate(reinterpret_cast<unsigned char*>(threads));
    return std::to_string(count) + " threads\n" + o.str();
}

std::string Core::cmd_mem(JNIEnv* env, bool gc) {
    jclass rt_cls = env->FindClass("java/lang/Runtime");
    jobject rt = env->CallStaticObjectMethod(rt_cls, env->GetStaticMethodID(rt_cls, "getRuntime", "()Ljava/lang/Runtime;"));
    auto read = [&](const char* name) {
        return env->CallLongMethod(rt, env->GetMethodID(rt_cls, name, "()J")) / (1024 * 1024);
    };
    long before = long(read("totalMemory") - read("freeMemory"));
    if (gc) env->CallVoidMethod(rt, env->GetMethodID(rt_cls, "gc", "()V"));
    long used = long(read("totalMemory") - read("freeMemory"));
    std::ostringstream o;
    o << "used " << used << " MiB, allocated " << read("totalMemory") << " MiB, max " << read("maxMemory") << " MiB";
    if (gc) o << " (freed " << before - used << " MiB)";
    return o.str() + "\n";
}

bool Core::unload_plugin(JNIEnv* env, const std::string& which, std::string& path, std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = std::find_if(plugins_.begin(), plugins_.end(), [&](const Plugin& p) {
        if (p.path == which || p.path.substr(p.path.rfind('/') + 1) == which) return true;
        for (auto& m : modules_)
            if (m.origin == p.path && m.module->name() == which) return true;
        return false;
    });
    if (it == plugins_.end()) {
        error = "no loaded plugin matches " + which;
        return false;
    }
    path = it->path;
    std::string msg;
    for (auto& m : modules_)
        if (m.origin == path && m.enabled) set_enabled(env, m, false, msg);
    modules_.erase(std::remove_if(modules_.begin(), modules_.end(), [&](const ModuleSlot& m) { return m.origin == path; }),
                   modules_.end());
    plat::lib_close(it->handle);
    plugins_.erase(it);
    log("unloaded plugin " + path);
    return true;
}

std::pair<bool, std::string> Core::command(JNIEnv* env, const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help") {
        return {true,
                "game\n"
                "  status                     version, side, mappings, counts\n"
                "  threads                    java threads and their state\n"
                "  mem | gc                   heap usage, or collect garbage first\n"
                "modules\n"
                "  modules                    list modules\n"
                "  enable|disable <name>      toggle a module\n"
                "  mod <name> [args...]       send a command to a module\n"
                "  load <plugin.so|script.js> load a native plugin or a script\n"
                "  unload|reload <plugin>     by path, file name, or one of its modules\n"
                "inspect\n"
                "  classes <filter>           search loaded classes\n"
                "  fields|methods <class> [f] list members with deobfuscated names\n"
                "  map <class> [member]       show mapping entries\n"
                "  script load|unload|list    JavaScript plugins\n"
                "  game                       which game accessors resolved on this version\n"
                "  mappings load <file>       load ProGuard mappings (the CLI does this on inject)\n"
                "  get <class> <field>        read a static field\n"
                "  set <class> <field> <v>    write a static field\n"
                "  call <class> <method> ...  call a static method\n"
                "  dump <class> <file>        save the class bytecode\n"
                "hooks\n"
                "  hooks                      list hooks\n"
                "  trace <class> <method> [d] log every call with its arguments\n"
                "  untrace <id|all>           stop tracing\n"
                "  log [n]                    recent agent log lines\n"};
    }
    const std::string& c = args[0];
    if (c == "ping") return {true, "pong\n"};
    if (c == "status") return {true, cmd_status()};
    if (c == "hooks") return {true, cmd_hooks()};
    if (c == "map") return {true, cmd_map(args)};
    if (c == "classes") return {true, cmd_classes(env, args.size() > 1 ? args[1] : "")};
    if (c == "fields" || c == "methods") return {true, cmd_members(env, args, c == "methods")};
    if (c == "get" || c == "set") return cmd_get_set(env, args, c == "set");
    if (c == "call") return cmd_call(env, args);
    if (c == "trace") return cmd_trace(args);
    if (c == "dump") return cmd_dump(env, args);
    if (c == "threads") return {true, cmd_threads()};
    if (c == "mappings") {
        if (args.size() < 3 || args[1] != "load") return {false, "usage: mappings load <proguard.txt>\n"};
        std::string err;
        if (!load_mappings(args[2], err)) return {false, err + "\n"};
        return {true, "loaded " + std::to_string(mappings_.size()) + " class mappings\n"};
    }
    if (c == "mem" || c == "gc") return {true, cmd_mem(env, c == "gc")};
    if (c == "game") return {true, GameApi::get().report(env)};
    if (c == "script") {
        std::string sub = args.size() > 1 ? args[1] : "list", err;
        if (sub == "list") {
            auto names = ScriptEngine::get().list();
            std::string out;
            for (auto& n : names) out += n + "\n";
            return {true, out.empty() ? "no scripts loaded\n" : out};
        }
        if (args.size() < 3) return {false, "usage: script load <file.js> | unload <name> | list\n"};
        if (sub == "load") {
            // Optional 4th argument: comma-separated permissions the user approved ("none" for none).
            std::set<std::string> grants;
            if (args.size() > 3) {
                std::istringstream gs(args[3]);
                std::string g;
                while (std::getline(gs, g, ',')) if (!g.empty() && g != "none") grants.insert(g);
            }
            if (!ScriptEngine::get().load(args[2], err, args.size() > 3 ? &grants : nullptr)) return {false, err + "\n"};
            return {true, "loaded " + args[2] + "\n"};
        }
        if (sub == "unload") {
            if (!ScriptEngine::get().unload(args[2], err)) return {false, err + "\n"};
            return {true, "unloaded " + args[2] + "\n"};
        }
        return {false, "usage: script load <file.js> | unload <name> | list\n"};
    }
    if (c == "untrace") {
        if (args.size() < 2) return {false, "usage: untrace <id|all>\n"};
        std::set<int> ids;
        {
            std::lock_guard<std::recursive_mutex> lock(mu_);
            if (args[1] == "all") ids = traces_;
            else if (traces_.count(std::atoi(args[1].c_str()))) ids.insert(std::atoi(args[1].c_str()));
            for (int id : ids) traces_.erase(id);
        }
        if (ids.empty()) return {false, "no such trace\n"};
        for (int id : ids) unhook(id);
        return {true, "stopped " + std::to_string(ids.size()) + " trace(s)\n"};
    }
    if (c == "log") {
        size_t n = args.size() > 1 ? size_t(std::max(1, std::atoi(args[1].c_str()))) : 40;
        std::lock_guard<std::mutex> lock(log_mu_);
        std::string out;
        for (size_t i = log_.size() > n ? log_.size() - n : 0; i < log_.size(); ++i) out += log_[i] + "\n";
        return {true, out};
    }
    if (c == "modules") {
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (modules_.empty()) return {true, "no modules loaded\n"};
        std::string out;
        for (auto& m : modules_)
            out += std::string(m.enabled ? "[on]  " : "[off] ") + m.module->name() + "  " + m.module->description() + "\n";
        return {true, out};
    }
    if (c == "load") {
        if (args.size() < 2) return {false, "usage: load <plugin.so | script.js>\n"};
        if (args[1].size() > 3 && args[1].compare(args[1].size() - 3, 3, ".js") == 0) {
            std::string err;
            if (!ScriptEngine::get().load(args[1], err)) return {false, err + "\n"};
            return {true, "loaded script " + args[1] + "\n"};
        }
        std::string err;
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (!load_plugin(args[1], err)) return {false, err + "\n"};
        return {true, "loaded " + args[1] + "\n"};
    }
    if (c == "unload" || c == "reload") {
        if (args.size() < 2) return {false, "usage: " + c + " <plugin>\n"};
        std::string path, err;
        if (!unload_plugin(env, args[1], path, err)) return {false, err + "\n"};
        if (c == "unload") return {true, "unloaded " + path + "\n"};
        std::lock_guard<std::recursive_mutex> lock(mu_);
        if (!load_plugin(path, err)) return {false, err + "\n"};
        return {true, "reloaded " + path + " (modules start disabled)\n"};
    }
    if (c == "enable" || c == "disable" || c == "mod") {
        if (args.size() < 2) return {false, "usage: " + c + " <module>\n"};
        std::lock_guard<std::recursive_mutex> lock(mu_);
        ModuleSlot* slot = module_slot(args[1]);
        if (!slot) return {false, "no module named " + args[1] + "\n"};
        if (c == "mod") {
            try {
                std::string out = slot->module->on_command(*this, env, {args.begin() + 2, args.end()});
                if (!out.empty() && out.back() != '\n') out += "\n";
                return {true, out};
            } catch (const std::exception& e) {
                return {false, std::string(e.what()) + "\n"};
            }
        }
        std::string msg;
        bool ok = set_enabled(env, *slot, c == "enable", msg);
        return {ok, msg + "\n"};
    }
    return {false, "unknown command: " + c + " (try help)\n"};
}

}  // namespace umc
