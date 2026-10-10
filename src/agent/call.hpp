#pragma once
#include <jni.h>
#include <functional>
#include <string>

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

    // Runs the original method now and makes its result the return value (primitives come back
    // boxed, void gives null). Code after proceed() runs after the original, so a hook can wrap it.
    // A return_*() call afterwards still replaces the result.
    virtual jobject proceed() = 0;

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

}
