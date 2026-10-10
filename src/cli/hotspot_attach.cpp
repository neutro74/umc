#if !defined(_WIN32)

#include "cli/hotspot_attach.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#include "common/platform.hpp"

namespace umc::cli {

bool hotspot_load(const AttachTarget& t, const std::string& library, const std::string& options,
                  std::string& message) {
    plat::Socket s = plat::connect_local(t.socket);
    if (s == plat::kNoSocket) {
        // The JVM starts its attach listener on SIGQUIT when a trigger file exists.
        std::string trigger;
        for (auto& candidate : t.triggers) {
            int fd = open(candidate.c_str(), O_CREAT | O_WRONLY, 0600);
            if (fd >= 0) {
                close(fd);
                trigger = candidate;
                break;
            }
        }
        if (trigger.empty()) {
            message = "cannot create the attach trigger file: " + std::string(std::strerror(errno));
            return false;
        }
        kill(t.pid, SIGQUIT);
        for (int i = 0; i < 100 && s == plat::kNoSocket; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            s = plat::connect_local(t.socket);
        }
        unlink(trigger.c_str());
        if (s == plat::kNoSocket) {
            message = "the JVM did not open its attach socket (started with -XX:+DisableAttachMechanism?)";
            return false;
        }
    }

    // Protocol v1: version, command, then exactly three NUL-terminated arguments.
    std::string req;
    for (const std::string& part : {std::string("1"), std::string("load"), library, std::string("true"), options})
        req += part + '\0';
    plat::send_all(s, req);
    std::string reply = plat::recv_all(s);
    plat::close_socket(s);

    // "0\n" means the command ran; the agent's own return code follows (bare, or "return code: N").
    if (reply.rfind("0\n", 0) != 0) {
        message = "attach failed: " + reply;
        return false;
    }
    std::string rest = reply.substr(2);
    if (rest.rfind("return code: ", 0) == 0) rest = rest.substr(13);
    if (!rest.empty() && rest[0] != '0') {
        message = "agent failed to start (code " + rest.substr(0, rest.find('\n')) + "), check the game log for [UMC]";
        return false;
    }
    message = "injected";
    return true;
}

}  // namespace umc::cli

#endif
