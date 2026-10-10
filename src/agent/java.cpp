#include "agent/java.hpp"

#include <string>
#include <vector>
#include "agent/core.hpp"
#include "agent/scripts.hpp"
#include "agent/util.hpp"

namespace umc {
namespace {
JSClassID object_id = 0, call_id = 0;
struct Object { jobject ref; bool type; };
struct ActiveCall { Call* call; std::string descriptor; };

JNIEnv* env_now() {
    JNIEnv* env = nullptr;
    if (Core::get().vm()) Core::get().vm()->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    return env;
}

struct Frame {
    JNIEnv* env;
    explicit Frame(JNIEnv* e) : env(e) { env->PushLocalFrame(128); }
    ~Frame() { env->PopLocalFrame(nullptr); }
};

jclass resolve_class(JNIEnv* env, const std::string& name) {
    if (jclass loaded = Core::get().find_class(env, name)) return loaded;
    std::string runtime = Core::get().runtime_class_name(name);
    // JNI FindClass uses the system loader on an attached native thread.
    jclass cls = env->FindClass(runtime.c_str());
    if (cls) return cls;
    env->ExceptionClear();
    jclass thread = env->FindClass("java/lang/Thread");
    jobject current = env->CallStaticObjectMethod(thread, env->GetStaticMethodID(thread, "currentThread", "()Ljava/lang/Thread;"));
    if (env->ExceptionCheck()) return nullptr;
    jobject loader = env->CallObjectMethod(current, env->GetMethodID(thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;"));
    if (env->ExceptionCheck()) return nullptr;
    jclass class_cls = env->FindClass("java/lang/Class");
    jmethodID for_name = env->GetStaticMethodID(class_cls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    jstring dotted_name = env->NewStringUTF(dotted(runtime).c_str());
    return static_cast<jclass>(env->CallStaticObjectMethod(class_cls, for_name, dotted_name, JNI_FALSE, loader));
}

void finalize_object(JSRuntime*, JSValue value) {
    auto* o = static_cast<Object*>(JS_GetOpaque(value, object_id));
    if (!o) return;
    if (JNIEnv* env = env_now()) env->DeleteGlobalRef(o->ref);
    delete o;
}
void finalize_call(JSRuntime*, JSValue value) {
    delete static_cast<ActiveCall*>(JS_GetOpaque(value, call_id));
}
JSValue exception(JSContext* ctx, JNIEnv* env) {
    return JS_ThrowInternalError(ctx, "%s", java_exception(env).c_str());
}

bool string_arg(JSContext* ctx, JSValueConst v, std::string& out) {
    const char* s = JS_ToCString(ctx, v);
    if (!s) return false;
    out = s;
    JS_FreeCString(ctx, s);
    return true;
}

// Validate descriptors before handing them to JNI, which assumes a well-formed signature.
bool type_desc(const std::string& s, size_t& at, bool allow_void = false) {
    if (at >= s.size()) return false;
    char c = s[at++];
    if (c == 'V') return allow_void;
    if (std::string("ZBCSIJFD").find(c) != std::string::npos) return true;
    if (c == '[') return type_desc(s, at);
    if (c != 'L') return false;
    size_t end = s.find(';', at);
    if (end == std::string::npos || end == at) return false;
    at = end + 1;
    return true;
}
bool method_desc(const std::string& s, std::vector<std::string>& args, char& ret) {
    if (s.empty() || s[0] != '(') return false;
    size_t at = 1;
    while (at < s.size() && s[at] != ')') {
        size_t start = at;
        if (!type_desc(s, at)) return false;
        args.push_back(s.substr(start, at - start));
    }
    if (at >= s.size() || ++at >= s.size()) return false;
    ret = s[at];
    return type_desc(s, at, true) && at == s.size();
}

bool to_value(JSContext* ctx, JNIEnv* env, JSValueConst value, const std::string& desc, jvalue& out) {
    out = {};
    int32_t i = 0;
    int64_t l = 0;
    double d = 0;
    switch (desc[0]) {
    case 'Z': i = JS_ToBool(ctx, value); if (i < 0) return false; out.z = i; return true;
    case 'B': case 'C': case 'S': case 'I':
        if (JS_ToInt32(ctx, &i, value)) return false;
        if (desc[0] == 'B') out.b = i;
        else if (desc[0] == 'C') out.c = i;
        else if (desc[0] == 'S') out.s = i;
        else out.i = i;
        return true;
    case 'J':
        if (JS_IsBigInt(value)) { if (JS_ToBigInt64(ctx, &l, value)) return false; }
        else if (JS_ToInt64(ctx, &l, value)) return false;
        out.j = l; return true;
    case 'F': case 'D':
        if (JS_ToFloat64(ctx, &d, value)) return false;
        if (desc[0] == 'F') out.f = d; else out.d = d;
        return true;
    default:
        if (JS_IsNull(value)) return true;
        if (JS_IsString(value)) {
            std::string s;
            if (!string_arg(ctx, value, s)) return false;
            // JS strings are UTF-16, as are Java strings. Avoid modified-UTF-8 corruption.
            size_t len;
            const uint16_t* chars = JS_ToCStringLenUTF16(ctx, &len, value);
            if (!chars) return false;
            out.l = env->NewString(reinterpret_cast<const jchar*>(chars), jsize(len));
            JS_FreeCStringUTF16(ctx, chars);
        } else {
            auto* o = static_cast<Object*>(JS_GetOpaque(value, object_id));
            if (!o) { JS_ThrowTypeError(ctx, "expected a Java object, string or null"); return false; }
            out.l = o->ref;
        }
        std::string cls = desc[0] == 'L' ? desc.substr(1, desc.size() - 2) : desc;
        jclass expected = resolve_class(env, cls);
        if (!expected || env->ExceptionCheck()) { exception(ctx, env); return false; }
        if (out.l && !env->IsInstanceOf(out.l, expected)) {
            JS_ThrowTypeError(ctx, "Java argument does not match %s", desc.c_str()); return false;
        }
        return true;
    }
}

JSValue value_js(JSContext* ctx, JNIEnv* env, jvalue v, char kind) {
    switch (kind) {
    case 'V': return JS_UNDEFINED;
    case 'Z': return JS_NewBool(ctx, v.z);
    case 'B': return JS_NewInt32(ctx, v.b);
    case 'C': return JS_NewInt32(ctx, v.c);
    case 'S': return JS_NewInt32(ctx, v.s);
    case 'I': return JS_NewInt32(ctx, v.i);
    case 'J': return JS_NewBigInt64(ctx, v.j);
    case 'F': return JS_NewFloat64(ctx, v.f);
    case 'D': return JS_NewFloat64(ctx, v.d);
    default: return java_to_js(ctx, env, v.l);
    }
}

JSValue wrap(JSContext* ctx, JNIEnv* env, jobject ref, bool type) {
    if (!ref) return JS_NULL;
    JSValue result = JS_NewObjectClass(ctx, object_id);
    if (JS_IsException(result)) return result;
    jobject global = env->NewGlobalRef(ref);
    if (!global) { JS_FreeValue(ctx, result); return exception(ctx, env); }
    JS_SetOpaque(result, new Object{global, type});
    return result;
}

JSValue js_type(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 1) return JS_ThrowTypeError(ctx, "java.type(className)");
    std::string name;
    if (!string_arg(ctx, argv[0], name)) return JS_EXCEPTION;
    JNIEnv* env = env_now();
    if (!env) return JS_ThrowInternalError(ctx, "current thread is not attached to Java");
    Frame frame(env);
    jclass cls = resolve_class(env, name);
    if (env->ExceptionCheck()) return exception(ctx, env);
    if (!cls) return JS_ThrowReferenceError(ctx, "Java class not found: %s", name.c_str());
    return wrap(ctx, env, cls, true);
}

std::string class_name(JNIEnv*, jclass cls) {
    char* sig = nullptr;
    if (Core::get().jvmti()->GetClassSignature(cls, &sig, nullptr) != JVMTI_ERROR_NONE) return "";
    std::string name = sig;
    Core::get().jvmti()->Deallocate(reinterpret_cast<unsigned char*>(sig));
    return name[0] == 'L' ? name.substr(1, name.size() - 2) : name;
}

JSValue js_invoke(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int construct) {
    auto* object = static_cast<Object*>(JS_GetOpaque2(ctx, self, object_id));
    if (!object) return JS_EXCEPTION;
    if (argc < (construct ? 1 : 2) || (construct && !object->type))
        return JS_ThrowTypeError(ctx, "use Type.new(descriptor, ...args) or object.call(name, descriptor, ...args)");
    std::string name = "<init>", desc;
    if ((!construct && !string_arg(ctx, argv[0], name)) || !string_arg(ctx, argv[construct ? 0 : 1], desc)) return JS_EXCEPTION;
    std::vector<std::string> params;
    char ret;
    if (!method_desc(desc, params, ret) || (construct && ret != 'V') || (!construct && (name.empty() || name[0] == '<')))
        return JS_ThrowTypeError(ctx, "invalid method descriptor or name");
    int offset = construct ? 1 : 2;
    if (params.size() != size_t(argc - offset)) return JS_ThrowTypeError(ctx, "argument count does not match descriptor");
    JNIEnv* env = env_now();
    if (!env) return JS_ThrowInternalError(ctx, "current thread is not attached to Java");
    Frame frame(env);
    jclass cls = object->type ? static_cast<jclass>(object->ref) : env->GetObjectClass(object->ref);
    jmethodID method = Core::get().find_method(env, class_name(env, cls), name, desc);
    if (env->ExceptionCheck()) return exception(ctx, env);
    if (!method) return JS_ThrowReferenceError(ctx, "Java method not found: %s%s", name.c_str(), desc.c_str());
    jint mods = 0;
    if (Core::get().jvmti()->GetMethodModifiers(method, &mods) != JVMTI_ERROR_NONE)
        return JS_ThrowInternalError(ctx, "cannot inspect Java method");
    bool stat = (mods & 8) != 0;
    if (!construct && stat != object->type) return JS_ThrowTypeError(ctx, "static methods need a type; instance methods need an object");
    if (construct) {
        jclass declaring = nullptr;
        if (Core::get().jvmti()->GetMethodDeclaringClass(method, &declaring) != JVMTI_ERROR_NONE ||
            !env->IsSameObject(declaring, cls)) return JS_ThrowTypeError(ctx, "constructor must belong to the requested class");
    }
    if (stat && Core::get().jvmti()->GetMethodDeclaringClass(method, &cls) != JVMTI_ERROR_NONE)
        return JS_ThrowInternalError(ctx, "cannot find method owner");
    std::vector<jvalue> args(params.size());
    for (size_t i = 0; i < params.size(); ++i)
        if (!to_value(ctx, env, argv[offset + i], params[i], args[i])) return JS_EXCEPTION;
    if (construct) {
        jobject result = env->NewObjectA(cls, method, args.data());
        if (env->ExceptionCheck()) return exception(ctx, env);
        return wrap(ctx, env, result, false);
    }
    jvalue result{};
#define INVOKE(K, field) result.field = stat ? env->CallStatic##K##MethodA(cls, method, args.data()) : env->Call##K##MethodA(object->ref, method, args.data()); break
    switch (ret) {
    case 'V': if (stat) env->CallStaticVoidMethodA(cls, method, args.data()); else env->CallVoidMethodA(object->ref, method, args.data()); break;
    case 'Z': INVOKE(Boolean, z);
    case 'B': INVOKE(Byte, b);
    case 'C': INVOKE(Char, c);
    case 'S': INVOKE(Short, s);
    case 'I': INVOKE(Int, i);
    case 'J': INVOKE(Long, j);
    case 'F': INVOKE(Float, f);
    case 'D': INVOKE(Double, d);
    default: INVOKE(Object, l);
    }
#undef INVOKE
    if (env->ExceptionCheck()) return exception(ctx, env);
    return value_js(ctx, env, result, ret);
}

JSValue js_field(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int set) {
    auto* object = static_cast<Object*>(JS_GetOpaque2(ctx, self, object_id));
    if (!object) return JS_EXCEPTION;
    if (argc != (set ? 3 : 2)) return JS_ThrowTypeError(ctx, "get(name, descriptor) or set(name, descriptor, value)");
    std::string name, desc;
    if (!string_arg(ctx, argv[0], name) || !string_arg(ctx, argv[1], desc)) return JS_EXCEPTION;
    size_t at = 0;
    if (!type_desc(desc, at) || at != desc.size()) return JS_ThrowTypeError(ctx, "invalid field descriptor");
    JNIEnv* env = env_now();
    if (!env) return JS_ThrowInternalError(ctx, "current thread is not attached to Java");
    Frame frame(env);
    jclass cls = object->type ? static_cast<jclass>(object->ref) : env->GetObjectClass(object->ref);
    jfieldID field = Core::get().find_field(env, class_name(env, cls), name, desc);
    if (env->ExceptionCheck()) return exception(ctx, env);
    if (!field) return JS_ThrowReferenceError(ctx, "Java field not found: %s", name.c_str());
    jint mods = 0;
    // GetFieldModifiers requires the class that actually declares the field.
    jclass owner = nullptr;
    if (Core::get().jvmti()->GetFieldDeclaringClass(cls, field, &owner) != JVMTI_ERROR_NONE ||
        Core::get().jvmti()->GetFieldModifiers(owner, field, &mods) != JVMTI_ERROR_NONE)
        return JS_ThrowInternalError(ctx, "cannot inspect Java field");
    bool stat = (mods & 8) != 0;
    if (stat != object->type) return JS_ThrowTypeError(ctx, "static fields need a type; instance fields need an object");
    char* actual = nullptr;
    if (Core::get().jvmti()->GetFieldName(owner, field, nullptr, &actual, nullptr) != JVMTI_ERROR_NONE)
        return JS_ThrowInternalError(ctx, "cannot inspect Java field descriptor");
    bool matches = Core::get().runtime_descriptor(desc) == actual;
    Core::get().jvmti()->Deallocate(reinterpret_cast<unsigned char*>(actual));
    if (!matches) return JS_ThrowTypeError(ctx, "Java field descriptor does not match");
    if (set && (mods & 16)) return JS_ThrowTypeError(ctx, "cannot write a final field");
    jvalue result{};
    if (set && !to_value(ctx, env, argv[2], desc, result)) return JS_EXCEPTION;
#define FIELD(K, member) if (set) { if (stat) env->SetStatic##K##Field(owner, field, result.member); else env->Set##K##Field(object->ref, field, result.member); } else result.member = stat ? env->GetStatic##K##Field(owner, field) : env->Get##K##Field(object->ref, field); break
    switch (desc[0]) {
    case 'Z': FIELD(Boolean, z);
    case 'B': FIELD(Byte, b);
    case 'C': FIELD(Char, c);
    case 'S': FIELD(Short, s);
    case 'I': FIELD(Int, i);
    case 'J': FIELD(Long, j);
    case 'F': FIELD(Float, f);
    case 'D': FIELD(Double, d);
    default: FIELD(Object, l);
    }
#undef FIELD
    if (env->ExceptionCheck()) return exception(ctx, env);
    return set ? JS_UNDEFINED : value_js(ctx, env, result, desc[0]);
}

JSValue js_hook(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 4 || !JS_IsFunction(ctx, argv[3])) return JS_ThrowTypeError(ctx, "java.hook(className, methodName, descriptor, callback)");
    std::string owner, name, desc;
    if (!string_arg(ctx, argv[0], owner) || !string_arg(ctx, argv[1], name) || !string_arg(ctx, argv[2], desc)) return JS_EXCEPTION;
    std::vector<std::string> params;
    char ret;
    if (!method_desc(desc, params, ret) || name.empty() || name[0] == '<') return JS_ThrowTypeError(ctx, "invalid hook method or descriptor");
    return ScriptEngine::get().add_java_hook(ctx, owner, name, desc, argv[3]);
}

JSValue js_unhook(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    int32_t id;
    if (argc != 1) return JS_ThrowTypeError(ctx, "java.unhook(id)");
    if (JS_ToInt32(ctx, &id, argv[0])) return JS_EXCEPTION;
    return ScriptEngine::get().remove_java_hook(ctx, id);
}

JSValue js_array_access(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int mode) {
    auto* object = static_cast<Object*>(JS_GetOpaque2(ctx, self, object_id));
    if (!object) return JS_EXCEPTION;
    JNIEnv* env = env_now();
    if (!env) return JS_ThrowInternalError(ctx, "current thread is not attached to Java");
    Frame frame(env);
    std::string desc = class_name(env, env->GetObjectClass(object->ref));
    if (object->type || desc.empty() || desc[0] != '[') return JS_ThrowTypeError(ctx, "expected a Java array");
    jsize length = env->GetArrayLength(static_cast<jarray>(object->ref));
    if (mode == 0) return JS_NewInt32(ctx, length);
    if (argc != (mode == 2 ? 2 : 1)) return JS_ThrowTypeError(ctx, "at(index) or put(index, value)");
    int32_t index;
    if (JS_ToInt32(ctx, &index, argv[0])) return JS_EXCEPTION;
    if (index < 0 || index >= length) return JS_ThrowRangeError(ctx, "Java array index out of bounds");
    jvalue value{};
    if (mode == 2 && !to_value(ctx, env, argv[1], desc.substr(1), value)) return JS_EXCEPTION;
#define ELEMENT(K, field, array_type) if (mode == 2) env->Set##K##ArrayRegion(static_cast<array_type>(object->ref), index, 1, &value.field); else env->Get##K##ArrayRegion(static_cast<array_type>(object->ref), index, 1, &value.field); break
    switch (desc[1]) {
    case 'Z': ELEMENT(Boolean, z, jbooleanArray);
    case 'B': ELEMENT(Byte, b, jbyteArray);
    case 'C': ELEMENT(Char, c, jcharArray);
    case 'S': ELEMENT(Short, s, jshortArray);
    case 'I': ELEMENT(Int, i, jintArray);
    case 'J': ELEMENT(Long, j, jlongArray);
    case 'F': ELEMENT(Float, f, jfloatArray);
    case 'D': ELEMENT(Double, d, jdoubleArray);
    default:
        if (mode == 2) env->SetObjectArrayElement(static_cast<jobjectArray>(object->ref), index, value.l);
        else value.l = env->GetObjectArrayElement(static_cast<jobjectArray>(object->ref), index);
    }
#undef ELEMENT
    if (env->ExceptionCheck()) return exception(ctx, env);
    return mode == 2 ? JS_UNDEFINED : value_js(ctx, env, value, desc[1]);
}

JSValue js_array(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 2 || !JS_IsArray(argv[1])) return JS_ThrowTypeError(ctx, "java.array(componentDescriptor, values)");
    std::string desc;
    if (!string_arg(ctx, argv[0], desc)) return JS_EXCEPTION;
    size_t at = 0;
    if (!type_desc(desc, at) || at != desc.size()) return JS_ThrowTypeError(ctx, "invalid array component descriptor");
    JSValue length_value = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t length;
    int status = JS_ToUint32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    if (status) return JS_EXCEPTION;
    if (length > 1048576) return JS_ThrowRangeError(ctx, "Java array is too large");
    JNIEnv* env = env_now();
    if (!env) return JS_ThrowInternalError(ctx, "current thread is not attached to Java");
    Frame frame(env);
    jobject array = nullptr;
    switch (desc[0]) {
    case 'Z': array = env->NewBooleanArray(length); break;
    case 'B': array = env->NewByteArray(length); break;
    case 'C': array = env->NewCharArray(length); break;
    case 'S': array = env->NewShortArray(length); break;
    case 'I': array = env->NewIntArray(length); break;
    case 'J': array = env->NewLongArray(length); break;
    case 'F': array = env->NewFloatArray(length); break;
    case 'D': array = env->NewDoubleArray(length); break;
    default: {
        jclass component = resolve_class(env, desc[0] == 'L' ? desc.substr(1, desc.size() - 2) : desc);
        if (!component || env->ExceptionCheck()) return exception(ctx, env);
        array = env->NewObjectArray(length, component, nullptr);
    }
    }
    if (!array || env->ExceptionCheck()) return exception(ctx, env);
    JSValue result = wrap(ctx, env, array, false);
    if (JS_IsException(result)) return result;
    for (uint32_t i = 0; i < length; ++i) {
        JSValue args[] = {JS_NewUint32(ctx, i), JS_GetPropertyUint32(ctx, argv[1], i)};
        JSValue written = JS_IsException(args[1]) ? JS_EXCEPTION : js_array_access(ctx, result, 2, args, 2);
        JS_FreeValue(ctx, args[1]);
        if (JS_IsException(written)) { JS_FreeValue(ctx, result); return written; }
        JS_FreeValue(ctx, written);
    }
    return result;
}

JSValue js_call_action(JSContext* ctx, JSValueConst self, int argc, JSValueConst* argv, int action) {
    auto* active = static_cast<ActiveCall*>(JS_GetOpaque2(ctx, self, call_id));
    if (!active) return JS_EXCEPTION;
    if (!active->call) return JS_ThrowTypeError(ctx, "hook call has expired");
    Call& call = *active->call;
    JNIEnv* env = call.env();
    if (action == 0) { call.cancel(); return JS_UNDEFINED; }
    if (action == 1) {
        jobject result = call.proceed();
        if (env->ExceptionCheck()) return exception(ctx, env);
        return java_to_js(ctx, env, result);
    }
    if (action == 3) {
        if (argc != 2) return JS_ThrowTypeError(ctx, "setArg(index, value), then proceed() to apply it");
        int32_t index;
        if (JS_ToInt32(ctx, &index, argv[0])) return JS_EXCEPTION;
        std::vector<std::string> params;
        char ret;
        method_desc(active->descriptor, params, ret);
        if (index < 0 || size_t(index) >= params.size()) return JS_ThrowRangeError(ctx, "argument index out of bounds");
        jvalue value{};
        const std::string& desc = params[index];
        if (!to_value(ctx, env, argv[1], desc, value)) return JS_EXCEPTION;
        jobject boxed = value.l;
        const char* names[] = {"Boolean", "Byte", "Character", "Short", "Integer", "Long", "Float", "Double"};
        const std::string kinds = "ZBCSIJFD";
        size_t kind = kinds.find(desc[0]);
        if (kind != std::string::npos) {
            std::string cls = std::string("java/lang/") + names[kind];
            std::string sig = "(" + desc + ")L" + cls + ";";
            boxed = Core::get().box(env, cls.c_str(), sig.c_str(), value);
            if (env->ExceptionCheck()) return exception(ctx, env);
        }
        if (!Core::get().set_argument(call, index, boxed)) return exception(ctx, env);
        JSValue args = JS_GetPropertyStr(ctx, self, "args");
        JS_SetPropertyUint32(ctx, args, index, JS_DupValue(ctx, argv[1]));
        JS_FreeValue(ctx, args);
        return JS_UNDEFINED;
    }
    // Return conversion uses the exact hooked method descriptor, attached by the engine.
    if (argc != 1) return JS_ThrowTypeError(ctx, "return(value)");
    const std::string& desc = active->descriptor;
    std::string ret = desc.substr(desc.find(')') + 1);
    if (ret == "V") { call.cancel(); return JS_UNDEFINED; }
    jvalue value{};
    if (!to_value(ctx, env, argv[0], ret, value)) return JS_EXCEPTION;
    switch (ret[0]) {
    case 'Z': call.return_int(value.z); break;
    case 'B': call.return_int(value.b); break;
    case 'C': call.return_int(value.c); break;
    case 'S': call.return_int(value.s); break;
    case 'I': call.return_int(value.i); break;
    case 'J': call.return_long(value.j); break;
    case 'F': call.return_float(value.f); break;
    case 'D': call.return_double(value.d); break;
    default: call.return_object(value.l ? env->NewLocalRef(value.l) : nullptr); break;
    }
    return JS_UNDEFINED;
}
}

JSValue java_to_js(JSContext* ctx, JNIEnv* env, jobject value) {
    if (!value) return JS_NULL;
    jclass string = env->FindClass("java/lang/String");
    if (env->IsInstanceOf(value, string)) {
        auto s = static_cast<jstring>(value);
        const jchar* chars = env->GetStringChars(s, nullptr);
        if (!chars) return exception(ctx, env);
        JSValue result = JS_NewStringUTF16(ctx, reinterpret_cast<const uint16_t*>(chars), env->GetStringLength(s));
        env->ReleaseStringChars(s, chars);
        env->DeleteLocalRef(string);
        return result;
    }
    env->DeleteLocalRef(string);
    const char* boxes[] = {"java/lang/Boolean", "java/lang/Byte", "java/lang/Character", "java/lang/Short", "java/lang/Integer", "java/lang/Long", "java/lang/Float", "java/lang/Double"};
    const char* kinds = "ZBCSIJFD";
    for (size_t i = 0; i < 8; ++i) {
        jclass cls = env->FindClass(boxes[i]);
        bool match = env->IsInstanceOf(value, cls);
        env->DeleteLocalRef(cls);
        if (match) {
            jvalue unboxed = Core::get().unbox(env, value, kinds[i]);
            if (env->ExceptionCheck()) return exception(ctx, env);
            if (kinds[i] == 'Z') unboxed.z = unboxed.i != 0;
            else if (kinds[i] == 'B') unboxed.b = unboxed.i;
            else if (kinds[i] == 'C') unboxed.c = unboxed.i;
            else if (kinds[i] == 'S') unboxed.s = unboxed.i;
            return value_js(ctx, env, unboxed, kinds[i]);
        }
    }
    return wrap(ctx, env, value, false);
}

JSValue java_call_event(JSContext* ctx, Call& call, const std::string& descriptor) {
    JSValue event = JS_NewObjectClass(ctx, call_id);
    if (JS_IsException(event)) return event;
    JS_SetOpaque(event, new ActiveCall{&call, descriptor});
    JS_SetPropertyStr(ctx, event, "self", java_to_js(ctx, call.env(), call.self()));
    JSValue args = JS_NewArray(ctx);
    for (int i = 0; i < call.arg_count(); ++i) {
        jobject arg = call.arg(i);
        JS_SetPropertyUint32(ctx, args, i, java_to_js(ctx, call.env(), arg));
        if (arg) call.env()->DeleteLocalRef(arg);
    }
    JS_SetPropertyStr(ctx, event, "args", args);
    return event;
}
void expire_java_call(JSValue event) {
    if (auto* active = static_cast<ActiveCall*>(JS_GetOpaque(event, call_id))) active->call = nullptr;
}

void install_java_api(JSContext* ctx, JSValue global) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    if (!object_id) JS_NewClassID(rt, &object_id);
    if (!call_id) JS_NewClassID(rt, &call_id);
    if (!JS_IsRegisteredClass(rt, object_id)) {
        JSClassDef object{}; object.class_name = "JavaObject"; object.finalizer = finalize_object;
        JS_NewClass(rt, object_id, &object);
        JSClassDef call{}; call.class_name = "JavaCall"; call.finalizer = finalize_call;
        JS_NewClass(rt, call_id, &call);
    }
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "call", JS_NewCFunctionMagic(ctx, js_invoke, "call", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, proto, "new", JS_NewCFunctionMagic(ctx, js_invoke, "new", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, proto, "get", JS_NewCFunctionMagic(ctx, js_field, "get", 2, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, proto, "set", JS_NewCFunctionMagic(ctx, js_field, "set", 3, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, proto, "length", JS_NewCFunctionMagic(ctx, js_array_access, "length", 0, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, proto, "at", JS_NewCFunctionMagic(ctx, js_array_access, "at", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, proto, "put", JS_NewCFunctionMagic(ctx, js_array_access, "put", 2, JS_CFUNC_generic_magic, 2));
    JS_SetClassProto(ctx, object_id, proto);
    proto = JS_NewObject(ctx);
    const char* names[] = {"cancel", "proceed", "return", "setArg"};
    for (int i = 0; i < 4; ++i)
        JS_SetPropertyStr(ctx, proto, names[i], JS_NewCFunctionMagic(ctx, js_call_action, names[i], i == 3 ? 2 : i == 2 ? 1 : 0, JS_CFUNC_generic_magic, i));
    JS_SetClassProto(ctx, call_id, proto);
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "type", JS_NewCFunction(ctx, js_type, "type", 1));
    JS_SetPropertyStr(ctx, api, "hook", JS_NewCFunction(ctx, js_hook, "hook", 4));
    JS_SetPropertyStr(ctx, api, "unhook", JS_NewCFunction(ctx, js_unhook, "unhook", 1));
    JS_SetPropertyStr(ctx, api, "array", JS_NewCFunction(ctx, js_array, "array", 2));
    JS_SetPropertyStr(ctx, global, "java", api);
}
}
