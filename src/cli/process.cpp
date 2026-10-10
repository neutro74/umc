#include "cli/process.hpp"

#include <algorithm>
#include <cctype>

#include "common/shared.hpp"

namespace umc::cli {

void classify(GameProcess& p) {
    std::string joined;
    for (auto& a : p.args) joined += a + " ";
    std::string low = joined;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    p.minecraft = false;
    for (const char* marker : {"minecraft", "net.fabricmc", "forge", "paper", "spigot", "purpur", "bukkit", "server.jar"})
        p.minecraft |= low.find(marker) != std::string::npos;

    bool client = joined.find("--accessToken") != std::string::npos ||
                  joined.find("net.minecraft.client.main.Main") != std::string::npos ||
                  joined.find("KnotClient") != std::string::npos;
    p.side = p.minecraft ? (client ? "client" : "server") : "-";
    for (size_t i = 0; i + 1 < p.args.size(); ++i)
        if (p.args[i] == "--version") p.version = p.args[i + 1];

    std::string reply;
    p.has_agent = agent_request(p, {"ping"}, reply);
}

bool agent_request(const GameProcess& p, const std::vector<std::string>& args, std::string& reply) {
    return shared::request(agent_socket(p), args, reply);
}

}  // namespace umc::cli
