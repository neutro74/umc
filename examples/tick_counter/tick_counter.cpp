// Example plugin: counts game ticks and can freeze them by cancelling the tick method.
//   umc load ./tick_counter.so && umc enable tick_counter
//   umc mod tick_counter count
//   umc mod tick_counter freeze on
#include <atomic>

#include <umc/umc.hpp>

namespace {

class TickCounter : public umc::Module {
public:
    std::string name() const override { return "tick_counter"; }
    std::string description() const override { return "counts ticks, can freeze the game loop"; }

    void on_enable(umc::Api& api, JNIEnv*) override {
        auto on_tick = [this](umc::Call& call) {
            ++ticks_;
            if (frozen_) call.cancel();
        };
        if (api.side() == umc::Side::Server)
            hook(api, "net.minecraft.server.MinecraftServer", "tickServer", "(Ljava/util/function/BooleanSupplier;)V", on_tick);
        else
            hook(api, "net.minecraft.client.Minecraft", "tick", "()V", on_tick);
    }

    void on_disable(umc::Api&, JNIEnv*) override { frozen_ = false; }

    std::string on_command(umc::Api&, JNIEnv*, const std::vector<std::string>& args) override {
        if (args.empty() || args[0] == "count") return std::to_string(ticks_.load()) + " ticks";
        if (args[0] == "reset") {
            ticks_ = 0;
            return "reset";
        }
        if (args[0] == "freeze" && args.size() > 1) {
            frozen_ = args[1] == "on";
            return frozen_ ? "frozen" : "running";
        }
        return "commands: count, reset, freeze on|off";
    }

private:
    std::atomic<long> ticks_{0};
    std::atomic<bool> frozen_{false};
};

}  // namespace

UMC_PLUGIN {
    api->add_module(std::make_unique<TickCounter>());
}
