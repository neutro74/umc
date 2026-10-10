#pragma once

#include <jni.h>
#include "quickjs.h"
#include "umc/umc.hpp"

namespace umc {
void install_java_api(JSContext* ctx, JSValue global);
JSValue java_to_js(JSContext* ctx, JNIEnv* env, jobject value);
// The call object is valid only while its hook callback runs.
JSValue java_call_event(JSContext* ctx, Call& call, const std::string& descriptor);
void expire_java_call(JSValue event);
}
