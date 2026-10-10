#pragma once

#include <jni.h>

namespace umc {

// Serves CLI requests on /tmp/umc-<uid>/<pid>.sock from a daemon thread.
bool start_server(JavaVM* vm);
void stop_server();

}  // namespace umc
