#include "agent/server.hpp"

#include <thread>

#include "agent/core.hpp"
#include "common/shared.hpp"

namespace umc {

namespace {

plat::Socket listener = plat::kNoSocket;
std::string listen_path;

// Streams agent log lines until the client goes away.
void follow(plat::Socket client) {
    uint64_t seq = 0;
    Core::get().log_since(seq, 0);  // start from now
    plat::send_all(client, "ok\n");
    for (;;) {
        // An empty keepalive line every few seconds also detects a closed client.
        std::string out;
        for (auto& line : Core::get().log_since(seq, 3000)) out += line + "\n";
        if (!plat::send_all(client, out.empty() ? "\n" : out)) break;
    }
}

void handle(JavaVM* vm, plat::Socket client) {
    JNIEnv* env = nullptr;
    JavaVMAttachArgs attach{JNI_VERSION_1_6, const_cast<char*>("UMC Request"), nullptr};
    if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &attach) != JNI_OK) {
        plat::close_socket(client);
        return;
    }
    std::vector<std::string> args = shared::read_request(client);
    if (!args.empty() && args[0] == "follow") {
        follow(client);
    } else {
        env->PushLocalFrame(256);
        auto [ok, text] = Core::get().command(env, args);
        env->PopLocalFrame(nullptr);
        env->ExceptionClear();
        plat::send_all(client, (ok ? "ok\n" : "err\n") + text);
    }
    plat::close_socket(client);
    vm->DetachCurrentThread();
}

void serve(JavaVM* vm) {
    for (;;) {
        plat::Socket client = plat::accept_local(listener);
        if (client == plat::kNoSocket) break;
        std::thread(handle, vm, client).detach();
    }
}

}  // namespace

bool start_server(JavaVM* vm) {
    std::string err;
    plat::make_dirs(plat::runtime_dir());
    listen_path = plat::join(plat::runtime_dir(), plat::agent_socket_name(plat::current_pid()));
    listener = plat::listen_local(listen_path, err);
    if (listener == plat::kNoSocket) {
        Core::get().log(err);
        return false;
    }
    std::thread(serve, vm).detach();
    std::atexit(stop_server);
    Core::get().log("control socket " + listen_path);
    return true;
}

void stop_server() {
    if (!listen_path.empty()) plat::remove_file(listen_path);
}

}  // namespace umc
