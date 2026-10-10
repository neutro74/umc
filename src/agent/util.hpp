// Small helpers shared by the agent sources.
#pragma once

#include <jni.h>

#include <string>

#include "umc/umc.hpp"

namespace umc {

inline std::string internal(std::string s) {
    for (auto& c : s) if (c == '.') c = '/';
    return s;
}

inline std::string dotted(std::string s) {
    for (auto& c : s) if (c == '/') c = '.';
    return s;
}

inline std::string side_name(Side s) {
    return s == Side::Client ? "client" : s == Side::Server ? "server" : "unknown";
}

inline std::string java_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return "";
    jthrowable t = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string text = "java exception";
    jclass obj = env->FindClass("java/lang/Object");
    jmethodID to_string = env->GetMethodID(obj, "toString", "()Ljava/lang/String;");
    if (auto s = static_cast<jstring>(env->CallObjectMethod(t, to_string))) {
        const char* c = env->GetStringUTFChars(s, nullptr);
        text = c;
        env->ReleaseStringUTFChars(s, c);
    }
    env->ExceptionClear();
    return text;
}

}  // namespace umc
