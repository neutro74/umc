// Test plugin: checks argument boxing, return overrides and field lookup through mappings.
#include <umc/umc.hpp>

#include <atomic>
#include <mutex>

namespace {

class Probe : public umc::Module {
public:
    std::string name() const override { return "probe"; }

    void on_enable(umc::Api& api, JNIEnv*) override {
        hook(api, "net.minecraft.client.Minecraft", "computeScore", "(IJLjava/lang/String;)I", [this](umc::Call& c) {
            std::lock_guard<std::mutex> lock(mu_);
            seen_ = std::to_string(c.arg_int(0)) + "," + std::to_string(c.arg_long(1));
            if (override_ >= 0) c.return_int(override_);
        });
    }

    std::string on_command(umc::Api& api, JNIEnv* env, const std::vector<std::string>& args) override {
        if (!args.empty() && args[0] == "override") {
            override_ = std::stoi(args.at(1));
            return "ok";
        }
        if (!args.empty() && args[0] == "field") {
            jfieldID f = api.find_field(env, "net.minecraft.client.Minecraft", "tickCount");
            jmethodID m = api.find_method(env, "net.minecraft.client.Minecraft", "computeScore");
            jclass cls = api.find_class(env, "net.minecraft.client.Minecraft");
            jint score = m ? env->CallStaticIntMethod(cls, m, 1, jlong(2), env->NewStringUTF("xyz")) : -1;
            return std::string("field=") + (f ? "found" : "missing") + " call=" + std::to_string(score);
        }
        std::lock_guard<std::mutex> lock(mu_);
        return "args=" + seen_;
    }

private:
    std::mutex mu_;
    std::string seen_ = "none";
    std::atomic<int> override_{-1};
};

}  // namespace

UMC_PLUGIN {
    api->add_module(std::make_unique<Probe>());
}
