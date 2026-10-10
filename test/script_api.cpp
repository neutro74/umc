#include <iostream>
#include <string>
#include "agent/core.hpp"
#include "agent/scripts.hpp"

namespace {
void JNICALL on_load(jvmtiEnv*, JNIEnv*, jclass, jobject, const char* name, jobject,
                     jint len, const unsigned char* data, jint* new_len, unsigned char** new_data) {
    umc::Core::get().on_class_file_load(name, len, data, new_len, new_data);
}
void JNICALL on_prepare(jvmtiEnv*, JNIEnv* env, jthread, jclass cls) {
    umc::Core::get().on_class_prepare(env, cls);
}
void require(bool ok, const std::string& message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main(int argc, char** argv) {
    require(argc == 3, "usage: umc_script_test <fixture classes> <repository>");
    std::string classpath = std::string("-Djava.class.path=") + argv[1];
    JavaVMOption options[2]{};
    options[0].optionString = classpath.data();
    options[1].optionString = const_cast<char*>("-Xcheck:jni");
    JavaVMInitArgs args{};
    args.version = JNI_VERSION_1_8; args.nOptions = 2; args.options = options;
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    require(JNI_CreateJavaVM(&vm, reinterpret_cast<void**>(&env), &args) == JNI_OK, "create JVM");
    jvmtiEnv* ti = nullptr;
    require(vm->GetEnv(reinterpret_cast<void**>(&ti), JVMTI_VERSION_1_2) == JNI_OK, "get JVMTI");
    jvmtiCapabilities caps{}; caps.can_retransform_classes = 1;
    require(ti->AddCapabilities(&caps) == JVMTI_ERROR_NONE, "enable retransformation");
    jvmtiEventCallbacks callbacks{}; callbacks.ClassFileLoadHook = on_load; callbacks.ClassPrepare = on_prepare;
    ti->SetEventCallbacks(&callbacks, sizeof callbacks);
    ti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_FILE_LOAD_HOOK, nullptr);
    ti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_CLASS_PREPARE, nullptr);
    std::string root = argv[2];
    require(umc::Core::get().start(vm, ti, env, "version=1.8.9,side=client,mappings=" + root + "/test/fakemc18/client.txt"), "start core");
    auto& scripts = umc::ScriptEngine::get();
    std::string error;
    bool loaded = scripts.load(root + "/test/scripts/java.js", error);
    require(loaded, "Java API: " + error);
    jclass fixture = env->FindClass("Interop");
    jmethodID add = env->GetStaticMethodID(fixture, "add", "(II)I");
    jint value = env->CallStaticIntMethod(fixture, add, 1, 2);
    require(!env->ExceptionCheck() && value == 123, "hook replaces return");
    jmethodID wrap = env->GetStaticMethodID(fixture, "wrapped", "(I)I");
    value = env->CallStaticIntMethod(fixture, wrap, 4);
    require(!env->ExceptionCheck() && value == 20, "hook edits arguments and wraps original");
    env->CallStaticVoidMethod(fixture, env->GetStaticMethodID(fixture, "fail", "()V"));
    require(!env->ExceptionCheck(), "scripts can catch original Java exceptions");
    require(env->GetStaticIntField(fixture, env->GetStaticFieldID(fixture, "shared", "I")) == 44, "original exception reached script");
    require(scripts.load(root + "/test/scripts/java_expired.js", error), "expired hook: " + error);
    jobject obj = env->NewObject(fixture, env->GetMethodID(fixture, "<init>", "()V"));
    jmethodID boolean_method = env->GetMethodID(fixture, "booleanValue", "(Z)Z");
    env->CallBooleanMethod(obj, boolean_method, JNI_TRUE);
    require(!env->ExceptionCheck(), "first lifetime call");
    env->CallBooleanMethod(obj, boolean_method, JNI_TRUE);
    require(!env->ExceptionCheck(), "second lifetime call");
    require(env->GetStaticIntField(fixture, env->GetStaticFieldID(fixture, "shared", "I")) == 99, "expired calls are rejected");
    require(scripts.unload("java-check", error), "unload Java hooks");
    value = env->CallStaticIntMethod(fixture, add, 1, 2);
    require(value == 3 && !env->ExceptionCheck(), "unload restores original");
    require(!scripts.load(root + "/test/scripts/java_failed.js", error), "failed load is reported");
    value = env->CallStaticIntMethod(fixture, add, 1, 2);
    require(value == 3 && !env->ExceptionCheck(), "failed load removes hooks");
    require(scripts.load(root + "/test/scripts/java_denied.js", error), "Java permission: " + error);
    std::set<std::string> grants;
    require(!scripts.load(root + "/test/scripts/java.js", error, &grants), "ungranted Java permission is denied");
    require(scripts.load(root + "/test/scripts/events.js", error), "events: " + error);
    jclass chat = env->FindClass("bao"), player = env->FindClass("bln"), controller = env->FindClass("bda"), text = env->FindClass("fa");
    jobject chat_obj = env->NewObject(chat, env->GetMethodID(chat, "<init>", "()V"));
    jobject player_obj = env->NewObject(player, env->GetMethodID(player, "<init>", "()V"));
    jobject controller_obj = env->NewObject(controller, env->GetMethodID(controller, "<init>", "()V"));
    jmethodID literal = env->GetMethodID(text, "<init>", "(Ljava/lang/String;)V");
    jmethodID show = env->GetMethodID(chat, "a", "(Leu;I)V"), send = env->GetMethodID(player, "e", "(Ljava/lang/String;)V");
    jmethodID attack = env->GetMethodID(controller, "a", "(Lwn;Lpk;)V");
    for (const char* message : {"secret stuff", "hello world"}) {
        jstring s = env->NewStringUTF(message);
        jobject component = env->NewObject(text, literal, s);
        env->CallVoidMethod(chat_obj, show, component, 0);
        require(!env->ExceptionCheck(), "chat invocation");
    }
    for (const char* message : {".help", "normal message"}) {
        env->CallVoidMethod(player_obj, send, env->NewStringUTF(message));
        require(!env->ExceptionCheck(), "send invocation");
    }
    for (int i = 0; i < 2; ++i) {
        env->CallVoidMethod(controller_obj, attack, player_obj, player_obj);
        require(!env->ExceptionCheck(), "attack invocation");
    }
    require(env->GetIntField(chat_obj, env->GetFieldID(chat, "shown", "I")) == 1, "chat cancellation");
    require(env->GetIntField(player_obj, env->GetFieldID(player, "sent", "I")) == 1, "send cancellation");
    require(env->GetIntField(controller_obj, env->GetFieldID(controller, "attacked", "I")) == 1, "attack cancellation");
    uint64_t seq = 0;
    for (const auto& line : umc::Core::get().log_since(seq, 0))
        require(line.find("hook error:") == std::string::npos && line.find("error in '") == std::string::npos, line);
    for (const auto& name : scripts.list()) {
        require(scripts.unload(name.substr(0, name.find(' ')), error), "cleanup");
    }
    std::cout << "all script API tests passed\n";
    // Core retains cached JNI references for the JVM's lifetime.
    return 0;
}
