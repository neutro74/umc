#include "agent/scripts.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "agent/core.hpp"
#include "agent/game.hpp"
#include "common/language.hpp"
#include <cmath>
#include <limits>
#include "common/platform.hpp"
#include "common/script_meta.hpp"
#include "agent/util.hpp"
#include "quickjs.h"

namespace umc {

struct ScriptEngine::Script {
    std::string name, path, origin;
    std::set<std::string> permissions;
    JSContext* ctx = nullptr;
    std::map<std::string, std::vector<JSValue>> handlers;
    std::map<int, JSValue> hooks;
    ScriptMeta meta;
    std::map<std::string, JSValue> commands;
    struct Task { uint64_t due; JSValue fn; };
    std::map<int, Task> tasks;
};

namespace {

constexpr size_t kMemoryLimit = 64u << 20;
constexpr size_t kStackLimit = 512u << 10;
constexpr int kLoadBudgetMs = 500;
constexpr int kHandlerBudgetMs = 50;

// Scripts run one at a time under the engine lock, so a single deadline is enough.
std::chrono::steady_clock::time_point g_deadline;
bool g_timed = false;
int g_fuel = 20000;
thread_local int g_cross_depth = 0;
thread_local JSValue g_event = JS_UNDEFINED;
thread_local Call* g_call = nullptr;
struct HandlerFrame {
    Call* call = g_call;
    JSValue event = g_event;
    HandlerFrame() { g_call = nullptr; g_event = JS_UNDEFINED; }
    ~HandlerFrame() { g_call = call; g_event = event; }
};

int interrupt(JSRuntime*, void*) { return g_timed && std::chrono::steady_clock::now() > g_deadline; }

struct Budget {
    std::chrono::steady_clock::time_point previous_deadline;
    bool previous_timed;
    explicit Budget(int ms) {
        previous_deadline = g_deadline;
        previous_timed = g_timed;
        if (!g_timed) g_fuel = 20000;
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
    std::error_code ec;
    fsys::path dir = storage_dir(script_of(ctx));
    for (const auto& path : {dir.parent_path(), dir, dir / name}) {
        if (fsys::is_symlink(fsys::symlink_status(path, ec))) {
            error = JS_ThrowTypeError(ctx, "storage symlinks are forbidden"); return false;
        }
        ec.clear();
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
    std::string body(kMaxFileBytes + 1, '\0');
    in.read(body.data(), std::streamsize(body.size())); body.resize(size_t(in.gcount()));
    if (body.size() > kMaxFileBytes) return JS_ThrowRangeError(ctx, "stored file exceeds 1 MB");
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
    bool exists = fsys::exists(file, ec);
    size_t existing = exists ? size_t(fsys::file_size(file, ec)) : 0;
    size_t new_size = append ? existing + body.size() : body.size();
    if (new_size > kMaxFileBytes) return JS_ThrowRangeError(ctx, "file would be larger than %zu KB", kMaxFileBytes >> 10);
    if (!exists && files >= kMaxFiles) return JS_ThrowRangeError(ctx, "storage is limited to %zu files", kMaxFiles);
    if (total - existing + new_size > kMaxTotalBytes) return JS_ThrowRangeError(ctx, "storage is limited to %zu MB", kMaxTotalBytes >> 20);

    std::string content = body;
    if (append && existing) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        content = ss.str() + body;
    }
    fsys::path temp = dir / (name + ".tmp");  // write whole, then rename, so a crash can't leave half a file
    if (fsys::is_symlink(fsys::symlink_status(temp, ec))) return JS_ThrowTypeError(ctx, "storage symlinks are forbidden");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << content;
        if (!out) return JS_ThrowInternalError(ctx, "could not write %s", name.c_str());
    }
    return plat::replace_file(temp.string(), file.string()) ? JS_TRUE : JS_ThrowInternalError(ctx, "could not write %s", name.c_str());
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

JSValue plain_java(JSContext* ctx, JNIEnv* env, jobject value) {
    if (!value) return JS_NULL;
    jclass string = env->FindClass("java/lang/String");
    bool text = env->IsInstanceOf(value, string);
    env->DeleteLocalRef(string);
    if (text) {
        auto s = static_cast<jstring>(value);
        const jchar* chars = env->GetStringChars(s, nullptr);
        if (!chars) return JS_ThrowInternalError(ctx, "cannot read Java string");
        JSValue result = JS_NewStringUTF16(ctx, reinterpret_cast<const uint16_t*>(chars), env->GetStringLength(s));
        env->ReleaseStringChars(s, chars);
        return result;
    }
    const char* boxes[] = {"Boolean", "Byte", "Character", "Short", "Integer", "Long", "Float", "Double"};
    const char* kinds = "ZBCSIJFD";
    for (int i = 0; i < 8; ++i) {
        std::string name = std::string("java/lang/") + boxes[i];
        jclass cls = env->FindClass(name.c_str());
        bool match = env->IsInstanceOf(value, cls); env->DeleteLocalRef(cls);
        if (!match) continue;
        jvalue v = Core::get().unbox(env, value, kinds[i]);
        if (env->ExceptionCheck()) return JS_ThrowInternalError(ctx, "%s", java_exception(env).c_str());
        if (i == 0) return JS_NewBool(ctx, v.i != 0);
        if (i < 5) return JS_NewInt32(ctx, v.i);
        if (i == 5) {
            if (v.j > 9007199254740991LL || v.j < -9007199254740991LL) return JS_ThrowRangeError(ctx, "long is outside exact numeric range");
            return JS_NewFloat64(ctx, double(v.j));
        }
        return JS_NewFloat64(ctx, i == 6 ? v.f : v.d);
    }
    // No JVM handles, reflection, class loaders or arbitrary object methods enter the language.
    return JS_NULL;
}

bool return_plain(JSContext* ctx, Call& call, const std::string& desc, JSValueConst value) {
    char kind = desc[desc.find(')') + 1];
    if (kind == 'V') { call.cancel(); return true; }
    if (kind == 'L' || kind == '[') {
        if (JS_IsNull(value)) { call.return_object(nullptr); return true; }
        if (desc.substr(desc.find(')') + 1) != "Ljava/lang/String;" || !JS_IsString(value)) {
            JS_ThrowTypeError(ctx, "reference returns accept null, or text for a String method"); return false;
        }
        size_t len = 0; const uint16_t* text = JS_ToCStringLenUTF16(ctx, &len, value);
        if (!text) return false;
        jstring result = call.env()->NewString(reinterpret_cast<const jchar*>(text), jsize(len));
        JS_FreeCStringUTF16(ctx, text); call.return_object(result); return true;
    }
    double n = 0;
    if (kind == 'Z') { call.return_int(JS_ToBool(ctx, value)); return true; }
    if (JS_ToFloat64(ctx, &n, value) || !std::isfinite(n)) { JS_ThrowTypeError(ctx, "hook result must be finite"); return false; }
    if (kind == 'D') call.return_double(n);
    else if (kind == 'F') call.return_float(float(n));
    else if (kind == 'J') {
        if (std::abs(n) > 9007199254740991.0 || std::trunc(n) != n) { JS_ThrowRangeError(ctx, "long result is not exact"); return false; }
        call.return_long(jlong(n));
    } else {
        double lo = kind == 'B' ? -128 : kind == 'S' ? -32768 : kind == 'C' ? 0 : double(INT32_MIN);
        double hi = kind == 'B' ? 127 : kind == 'S' ? 32767 : kind == 'C' ? 65535 : double(INT32_MAX);
        if (n < lo || n > hi || std::trunc(n) != n) { JS_ThrowRangeError(ctx, "integer result out of range"); return false; }
        call.return_int(jint(n));
    }
    return true;
}

JSValue language_step(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (--g_fuel < 0) return JS_ThrowRangeError(ctx, "instruction budget exceeded");
    return JS_UNDEFINED;
}
JSValue language_get(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 2) return JS_ThrowTypeError(ctx, "invalid property access");
    const char* text = JS_ToCString(ctx, argv[1]);
    if (!text) return JS_EXCEPTION;
    std::string key = text; JS_FreeCString(ctx, text);
    if (key == "constructor" || key == "prototype" || key == "__proto__" || key == "caller" || key == "callee" || key == "arguments")
        return JS_ThrowTypeError(ctx, "reserved property");
    if (JS_IsString(argv[0])) {
        if (key == "length") return JS_GetPropertyStr(ctx, argv[0], "length");
        if (key.empty() || key.size() > 9 || key.find_first_not_of("0123456789") != std::string::npos) return JS_ThrowTypeError(ctx, "invalid text index");
        return JS_GetPropertyUint32(ctx, argv[0], uint32_t(std::stoul(key)));
    }
    if (!JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "property access needs a record or array");
    JSAtom atom = JS_NewAtomLen(ctx, key.data(), key.size()); JSPropertyDescriptor property{};
    int found = JS_GetOwnProperty(ctx, &property, argv[0], atom); JS_FreeAtom(ctx, atom);
    if (found < 0) return JS_EXCEPTION;
    if (!found) return JS_NULL;
    JS_FreeValue(ctx, property.getter); JS_FreeValue(ctx, property.setter);
    if (property.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, property.value); return JS_ThrowTypeError(ctx, "accessors are unavailable"); }
    return property.value;
}
JSValue language_str(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 1) return JS_ThrowTypeError(ctx, "str(value)");
    const char* s = JS_ToCString(ctx, argv[0]); if (!s) return JS_EXCEPTION;
    JSValue result = JS_NewString(ctx, s); JS_FreeCString(ctx, s); return result;
}
JSValue language_len(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 1 || (!JS_IsString(argv[0]) && !JS_IsArray(argv[0]))) return JS_ThrowTypeError(ctx, "len needs text or an array");
    return JS_GetPropertyStr(ctx, argv[0], "length");
}
JSValue language_round(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    double value; int32_t digits = 0;
    if (argc < 1 || argc > 2 || JS_ToFloat64(ctx, &value, argv[0]) || (argc == 2 && JS_ToInt32(ctx, &digits, argv[1]))) return JS_ThrowTypeError(ctx, "round(number, digits = 0)");
    if (!std::isfinite(value) || digits < 0 || digits > 6) return JS_ThrowRangeError(ctx, "invalid rounding arguments");
    double factor = std::pow(10, digits); return JS_NewFloat64(ctx, std::round(value * factor) / factor);
}
JSValue language_contains(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_ThrowTypeError(ctx, "contains(text, text)");
    size_t n1, n2; const char* a = JS_ToCStringLen(ctx, &n1, argv[0]); if (!a) return JS_EXCEPTION;
    const char* b = JS_ToCStringLen(ctx, &n2, argv[1]); if (!b) { JS_FreeCString(ctx, a); return JS_EXCEPTION; }
    bool result = std::string(a, n1).find(std::string(b, n2)) != std::string::npos;
    JS_FreeCString(ctx, a); JS_FreeCString(ctx, b); return JS_NewBool(ctx, result);
}
JSValue language_cancel(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (g_call) { g_call->cancel(); return JS_UNDEFINED; }
    if (!JS_IsObject(g_event)) return JS_ThrowTypeError(ctx, "cancel is only available during a cancellable handler");
    JS_SetPropertyStr(ctx, g_event, "cancelled", JS_TRUE); return JS_UNDEFINED;
}
JSValue language_original(JSContext* ctx, JSValueConst, int argc, JSValueConst*) {
    if (argc || !g_call) return JS_ThrowTypeError(ctx, "original() is only available in a method hook");
    jobject result = g_call->proceed();
    if (g_call->env()->ExceptionCheck()) return JS_ThrowInternalError(ctx, "%s", java_exception(g_call->env()).c_str());
    return plain_java(ctx, g_call->env(), result);
}
JSValue language_command(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "invalid command declaration");
    const char* name = JS_ToCString(ctx, argv[0]); if (!name) return JS_EXCEPTION;
    std::string key = name; JS_FreeCString(ctx, name); auto* script = script_of(ctx);
    if (key.empty() || key.size() > 64 || script->commands.size() >= 64 || script->commands.count(key)) return JS_ThrowRangeError(ctx, "invalid or duplicate command");
    script->commands[key] = JS_DupValue(ctx, argv[1]); return JS_UNDEFINED;
}
JSValue language_schedule(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int32_t ticks;
    if (argc != 2 || !JS_IsFunction(ctx, argv[1]) || JS_ToInt32(ctx, &ticks, argv[0]) || ticks < 1 || ticks > 1000000) return JS_ThrowRangeError(ctx, "after delay must be 1..1000000 ticks");
    int id = ScriptEngine::get().schedule(ctx, ticks, argv[1]);
    return id < 0 ? JS_ThrowRangeError(ctx, "task limit reached or no tick hook") : JS_NewInt32(ctx, id);
}
JSValue language_cancel_task(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int32_t id;
    if (argc != 1 || JS_ToInt32(ctx, &id, argv[0])) return JS_ThrowTypeError(ctx, "cancel_task(id)");
    return JS_NewBool(ctx, ScriptEngine::get().cancel_task(ctx, id));
}
JSValue language_call(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_ThrowTypeError(ctx, "plugins.call(id, command, ...args)");
    const char* a = JS_ToCString(ctx, argv[0]); if (!a) return JS_EXCEPTION;
    std::string target = a; JS_FreeCString(ctx, a);
    const char* b = JS_ToCString(ctx, argv[1]); if (!b) return JS_EXCEPTION;
    std::string command = b; JS_FreeCString(ctx, b);
    JSValue args = JS_NewArray(ctx);
    for (int i = 2; i < argc; ++i) JS_SetPropertyUint32(ctx, args, i - 2, JS_DupValue(ctx, argv[i]));
    JSValue result = ScriptEngine::get().call_plugin(ctx, target, command, args); JS_FreeValue(ctx, args); return result;
}
JSValue language_hook(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 4 || !JS_IsFunction(ctx, argv[3])) return JS_ThrowTypeError(ctx, "invalid hook declaration");
    std::string values[3];
    for (int i = 0; i < 3; ++i) { const char* v = JS_ToCString(ctx, argv[i]); if (!v) return JS_EXCEPTION; values[i] = v; JS_FreeCString(ctx, v); }
    if (values[0].rfind("net.minecraft.", 0) != 0 || values[1].empty() || values[1][0] == '<' || values[2].empty() || values[2][0] != '(' || values[2].find(')') == std::string::npos)
        return JS_ThrowTypeError(ctx, "hooks must target a Minecraft method with a descriptor");
    if (script_of(ctx)->hooks.size() >= 128) return JS_ThrowRangeError(ctx, "hook limit reached");
    return ScriptEngine::get().add_method_hook(ctx, values[0], values[1], values[2], argv[3]);
}
JSValue language_version(JSContext* ctx, JSValueConst, int, JSValueConst*) { return JS_NewString(ctx, Core::get().version().c_str()); }
void install_language_api(JSContext* ctx, JSValue global) {
    struct Entry { const char* name; JSCFunction* fn; int argc; };
    const Entry entries[] = {
        {"__umc_step",language_step,0}, {"__umc_get",language_get,2}, {"__umc_str",language_str,1}, {"__umc_len",language_len,1},
        {"__umc_round",language_round,2}, {"__umc_contains",language_contains,2}, {"__umc_cancel",language_cancel,0},
        {"__umc_original",language_original,0}, {"__umc_command",language_command,2}, {"__umc_schedule",language_schedule,2},
        {"__umc_cancel_task",language_cancel_task,1}, {"__umc_call",language_call,2}, {"__umc_hook",language_hook,4}, {"__umc_version",language_version,0}
    };
    for (const auto& e : entries) JS_SetPropertyStr(ctx, global, e.name, JS_NewCFunction(ctx, e.fn, e.name, e.argc));
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

bool ScriptEngine::load(const std::string& input_path, std::string& error, const std::set<std::string>* grants, const std::string& origin) {
    const std::string path = plat::expand_home(input_path);
    if (std::filesystem::path(path).extension() != ".umc") { error = "only sandboxed .umc plugins are supported"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    std::string source(lang::max_source_bytes + 1, '\0');
    in.read(source.data(), std::streamsize(source.size()));
    source.resize(size_t(in.gcount()));
    lang::Compiled compiled;
    if (!lang::compile(source, compiled, error)) return false;
    std::string name = compiled.meta.id;

    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (scripts_.size() >= 32) { error = "plugin limit reached"; return false; }
    if (scripts_.count(name)) {
        error = "a script named " + name + " is already loaded";
        return false;
    }
    ensure_runtime();
    JS_UpdateStackTop(rt_);  // this request may be on a different thread than the last one

    ScriptMeta meta = compiled.meta;
    for (const auto& dep : meta.dependencies) {
        auto found = scripts_.find(dep.first);
        if (found == scripts_.end() || found->second->meta.version != dep.second) { error = "dependency not loaded: " + dep.first + " " + dep.second; return false; }
    }
    auto script = std::make_unique<Script>();
    script->name = meta.has_header && !meta.id.empty() ? meta.id : name;
    if (scripts_.count(script->name)) {
        error = "a script named " + script->name + " is already loaded";
        return false;
    }
    script->path = path;
    script->origin = origin.empty() ? path : origin;
    script->meta = meta;
    for (auto& p : meta.permissions) {
        if (!is_known_permission(p)) {
            error = "unknown permission '" + p + "'";
            return false;
        }
        if (!grants || grants->count(p)) script->permissions.insert(p);
    }
    name = script->name;
    script->ctx = JS_NewContext(rt_);
    JS_SetContextOpaque(script->ctx, script.get());

    // Capabilities are installed only for granted permissions. Every plugin receives only declared, granted capabilities.
    JSContext* ctx = script->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue umc = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, umc, "name", JS_NewString(ctx, name.c_str()));
    JS_SetPropertyStr(ctx, umc, "log", JS_NewCFunction(ctx, js_log, "log", 1));
    JS_SetPropertyStr(ctx, umc, "on", JS_NewCFunction(ctx, js_on, "on", 2));
    JS_SetPropertyStr(ctx, global, "umc", umc);

    const auto& allowed = script->permissions;
    install_language_api(ctx, global);
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
        result = JS_Eval(ctx, compiled.code.c_str(), compiled.code.size(), path.c_str(), JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    }
    if (JS_IsException(result)) {
        error = describe_exception(ctx);
        for (auto& [event, fns] : script->handlers)
            for (JSValue fn : fns) JS_FreeValue(ctx, fn);
        for (auto& [id, fn] : script->hooks) {
            Core::get().unhook(id);
            JS_FreeValue(ctx, fn);
        }
        for (auto& [key, fn] : script->commands) JS_FreeValue(ctx, fn);
        for (auto& [id, task] : script->tasks) JS_FreeValue(ctx, task.fn);
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
    for (const auto& [id, dependent] : scripts_) for (const auto& dep : dependent->meta.dependencies)
        if (dep.first == name) { error = "plugin is required by " + id; return false; }
    Script& script = *it->second;
    for (auto& [key, fn] : script.commands) JS_FreeValue(script.ctx, fn);
    for (auto& [id, task] : script.tasks) JS_FreeValue(script.ctx, task.fn);
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
        out.push_back(name + "  v" + script->meta.version + "  [" + perms + "]");
    }
    return out;
}

JSValue ScriptEngine::add_method_hook(JSContext* ctx, const std::string& owner, const std::string& method,
                                   const std::string& descriptor, JSValueConst callback) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Script* script = script_of(ctx);
    if (!script->permissions.count("hooks")) return JS_ThrowRangeError(ctx, "method hooks need the 'hooks' permission");
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
        HandlerFrame frame;
        JSValue callback = JS_DupValue(script.ctx, fn->second);
        JSValue event = JS_NewArray(script.ctx);
        for (int i = 0; i < call.arg_count(); ++i) {
            jobject arg = call.arg(i);
            JS_SetPropertyUint32(script.ctx, event, i, plain_java(script.ctx, call.env(), arg));
            if (arg) call.env()->DeleteLocalRef(arg);
        }
        Call* previous_call = g_call;
        g_call = &call;
        JSValue result = JS_EXCEPTION;
        if (!JS_IsException(event)) {
            Budget budget(kHandlerBudgetMs);
            result = JS_Call(script.ctx, callback, JS_UNDEFINED, 1, &event);
            if (!JS_IsException(result) && !JS_IsUndefined(result)) if (!return_plain(script.ctx, call, descriptor, result)) { JS_FreeValue(script.ctx, result); result = JS_EXCEPTION; }
            JS_FreeValue(script.ctx, event);
        }
        g_call = previous_call;
        if (JS_IsException(result)) {
            Core::get().log("[" + name + "] method hook error: " + describe_exception(script.ctx));
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
    if (*id < 0) return JS_ThrowReferenceError(ctx, "could not hook Minecraft method");
    script->hooks[*id] = JS_DupValue(ctx, callback);
    return JS_NewInt32(ctx, *id);
}

JSValue ScriptEngine::remove_method_hook(JSContext* ctx, int id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Script* script = script_of(ctx);
    auto it = script->hooks.find(id);
    if (it == script->hooks.end()) return JS_FALSE;
    Core::get().unhook(id);
    JS_FreeValue(ctx, it->second);
    script->hooks.erase(it);
    return JS_TRUE;
}

int ScriptEngine::schedule(JSContext* ctx, int ticks, JSValueConst fn) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Script* script = script_of(ctx);
    size_t total = script->tasks.size();
    for (const auto& [name, loaded] : scripts_) if (loaded.get() != script) total += loaded->tasks.size();
    if (script->tasks.size() >= 128 || total >= 1024 || next_task_ == INT32_MAX || !ensure_event_hook("tick")) return -1;
    int id = next_task_++; script->tasks.emplace(id, Script::Task{tick_ + uint64_t(ticks), JS_DupValue(ctx, fn)}); return id;
}
bool ScriptEngine::cancel_task(JSContext* ctx, int id) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto* script = script_of(ctx); auto it = script->tasks.find(id);
    if (it == script->tasks.end()) return false;
    JS_FreeValue(ctx, it->second.fn); script->tasks.erase(it); return true;
}
void ScriptEngine::drain_tasks() {
    ++tick_;
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    // Snapshot eligible ids; work scheduled inside a task waits for a later tick.
    std::vector<std::pair<std::string, int>> ready;
    for (const auto& [name, script] : scripts_) for (const auto& [id, task] : script->tasks)
        if (task.due <= tick_) ready.emplace_back(name, id);
    for (const auto& [name, id] : ready) {
        auto found = scripts_.find(name); if (found == scripts_.end()) continue;
        auto& script = *found->second; auto task = script.tasks.find(id); if (task == script.tasks.end()) continue;
        JSValue fn = task->second.fn; script.tasks.erase(task);
        Budget budget(kHandlerBudgetMs);
        JSValue r = JS_Call(script.ctx, fn, JS_UNDEFINED, 0, nullptr); JS_FreeValue(script.ctx, fn);
        if (JS_IsException(r)) Core::get().log("[" + name + "] scheduled task error: " + describe_exception(script.ctx));
        JS_FreeValue(script.ctx, r);
        if (std::chrono::steady_clock::now() >= until) break;
    }
}
JSValue ScriptEngine::call_plugin(JSContext* caller, const std::string& target, const std::string& command, JSValueConst args) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    Script* source = script_of(caller);
    bool declared = false;
    for (const auto& dep : source->meta.dependencies) if (dep.first == target) declared = true;
    if (!declared) return JS_ThrowRangeError(caller, "undeclared dependency: %s", target.c_str());
    auto it = scripts_.find(target);
    if (it == scripts_.end()) return JS_ThrowReferenceError(caller, "dependency is not loaded");
    Script& recipient = *it->second;
    for (const auto& permission : recipient.permissions) if (!source->permissions.count(permission))
        return JS_ThrowRangeError(caller, "dependency needs a capability the caller lacks: %s", permission.c_str());
    auto fn = recipient.commands.find(command);
    if (fn == recipient.commands.end()) return JS_ThrowReferenceError(caller, "dependency has no command '%s'", command.c_str());
    if (g_cross_depth >= 16) return JS_ThrowRangeError(caller, "dependency call depth exceeded");
    ++g_cross_depth;
    Call* previous_call = g_call; JSValue previous_event = g_event; g_call = nullptr; g_event = JS_UNDEFINED;
    Budget budget(kHandlerBudgetMs);
    JSValue r = JS_Call(recipient.ctx, fn->second, JS_UNDEFINED, 1, &args);
    g_call = previous_call; g_event = previous_event; --g_cross_depth;
    if (JS_IsException(r)) { std::string why = describe_exception(recipient.ctx); return JS_ThrowInternalError(caller, "%s: %s", target.c_str(), why.c_str()); }
    return r;
}
std::pair<bool, std::string> ScriptEngine::invoke(const std::string& target, const std::string& command, const std::vector<std::string>& values) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = scripts_.find(target); if (it == scripts_.end()) return {false, "plugin is not loaded"};
    Script& script = *it->second;
    auto fn = script.commands.find(command); if (fn == script.commands.end()) return {false, "unknown plugin command"};
    JS_UpdateStackTop(rt_); Budget budget(kHandlerBudgetMs);
    JSValue args = JS_NewArray(script.ctx);
    for (size_t i = 0; i < values.size(); ++i) JS_SetPropertyUint32(script.ctx, args, uint32_t(i), JS_NewStringLen(script.ctx, values[i].data(), values[i].size()));
    JSValue r = JS_Call(script.ctx, fn->second, JS_UNDEFINED, 1, &args); JS_FreeValue(script.ctx, args);
    if (JS_IsException(r)) return {false, describe_exception(script.ctx)};
    if (JS_IsUndefined(r)) { JS_FreeValue(script.ctx, r); return {true, ""}; }
    JSValue output = JS_IsString(r) ? JS_DupValue(script.ctx, r) : JS_JSONStringify(script.ctx, r, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(script.ctx, r);
    if (JS_IsException(output)) return {false, describe_exception(script.ctx)};
    size_t len; const char* text = JS_ToCStringLen(script.ctx, &len, output);
    std::string result = text ? std::string(text, std::min(len, size_t(65536))) : "";
    if (text) JS_FreeCString(script.ctx, text);
    JS_FreeValue(script.ctx, output);
    return {true, result};
}
std::pair<bool, std::string> ScriptEngine::source(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = scripts_.find(name);
    return it == scripts_.end() ? std::make_pair(false, std::string("plugin is not loaded")) : std::make_pair(true, it->second->origin);
}
bool ScriptEngine::reload(const std::string& name, std::string& error, const std::string& replacement) {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    auto it = scripts_.find(name); if (it == scripts_.end()) { error = "plugin is not loaded"; return false; }
    std::string path = replacement.empty() ? it->second->path : replacement;
    std::string origin = it->second->origin;
    std::set<std::string> grants = it->second->permissions;
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "cannot read " + path; return false; }
    std::string body(lang::max_source_bytes + 1, '\0'); in.read(body.data(), std::streamsize(body.size())); body.resize(size_t(in.gcount()));
    lang::Compiled compiled;
    if (!lang::compile(body, compiled, error)) return false;
    if (compiled.meta.id != name) { error = "reload cannot change plugin ID"; return false; }
    if (!unload(name, error)) return false;
    return load(path, error, &grants, origin);
}

namespace {

thread_local bool g_in_event = false;

}  // namespace

bool ScriptEngine::emit(const std::string& event, const EventFill& fill) {
    // A handler that triggers the same kind of event (say, chatting from a chat handler) must not recurse.
    if (g_in_event && fill) return false;
    std::lock_guard<std::recursive_mutex> lock(mu_);
    if (!rt_) return false;
    JS_UpdateStackTop(rt_);
    HandlerFrame frame;
    if (event == "tick") drain_tasks();
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
                fill(ctx, ev);
                argc = 1;
            }
            JSValue r;
            {
                Budget budget(kHandlerBudgetMs);
                JSValue previous_event = g_event;
                g_event = ev;
                r = JS_Call(ctx, fn, JS_UNDEFINED, argc, &ev);
                g_event = previous_event;
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
