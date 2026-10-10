// Injection entry points. Linux and macOS load the agent through HotSpot's attach mechanism
// (Agent_OnAttach). On Windows the CLI injects the DLL with LoadLibrary and DllMain finds the JVM.
#include <jvmti.h>

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include "agent/core.hpp"
#include "agent/server.hpp"
#include "common/platform.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

void JNICALL on_class_file_load(jvmtiEnv*, JNIEnv*, jclass, jobject, const char* name, jobject, jint len,
                                const unsigned char* data, jint* new_len, unsigned char** new_data) {
    umc::Core::get().on_class_file_load(name, len, data, new_len, new_data);
}

void JNICALL on_class_prepare(jvmtiEnv*, JNIEnv* env, jthread, jclass cls) {
    umc::Core::get().on_class_prepare(env, cls);
}

jint start(JavaVM* vm, const std::string& options) {
    static std::mutex once;
    std::lock_guard<std::mutex> lock(once);
    umc::Core& core = umc::Core::get();
    if (core.started()) {
        core.log("already injected");
        return JNI_OK;
    }

    jvmtiEnv* jvmti = nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&jvmti), JVMTI_VERSION_1_2) != JNI_OK ||
        vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        std::fprintf(stderr, "[UMC] cannot get JVMTI/JNI environments\n");
        return JNI_ERR;
    }
    jvmtiCapabilities caps{};
    caps.can_retransform_classes = 1;
    if (jvmti->AddCapabilities(&caps) != JVMTI_ERROR_NONE) {
        std::fprintf(stderr, "[UMC] this JVM does not allow class retransformation\n");
        return JNI_ERR;
    }
    jvmtiEventCallbacks cb{};
    cb.ClassFileLoadHook = on_class_file_load;
    cb.ClassPrepare = on_class_prepare;
    jvmti->SetEventCallbacks(&cb, sizeof cb);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr);
    jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, nullptr);

    if (!core.start(vm, jvmti, env, options)) return JNI_ERR;
    return umc::start_server(vm) ? JNI_OK : JNI_ERR;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM* vm, char* options, void*) {
    return start(vm, options ? options : "");
}

#if defined(_WIN32)

namespace {

DWORD WINAPI inject_thread(LPVOID) {
    using GetVMs = jint(JNICALL*)(JavaVM**, jsize, jsize*);
    HMODULE jvm = GetModuleHandleW(L"jvm.dll");
    auto get_vms = jvm ? reinterpret_cast<GetVMs>(GetProcAddress(jvm, "JNI_GetCreatedJavaVMs")) : nullptr;
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (!get_vms || get_vms(&vm, 1, &count) != JNI_OK || count < 1) return 1;

    // The CLI writes options next to the control socket before injecting.
    std::string path = umc::plat::join(umc::plat::runtime_dir(), "inject-" + std::to_string(umc::plat::current_pid()) + ".opts");
    std::string options;
    std::getline(std::ifstream(path), options);
    umc::plat::remove_file(path);

    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_6, const_cast<char*>("UMC Init"), nullptr};
    if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) return 1;
    jint rc = start(vm, options);
    vm->DetachCurrentThread();
    return rc == JNI_OK ? 0 : 1;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    // JNI can't run under the loader lock, so initialization moves to its own thread.
    if (reason == DLL_PROCESS_ATTACH) {
        if (HANDLE t = CreateThread(nullptr, 0, inject_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}

#endif
