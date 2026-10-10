// Read access to the running game, resolved through the candidate table in accessors.txt so each
// accessor finds whichever name the running Minecraft version uses.
#pragma once

#include <jni.h>

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace umc {

struct EntityInfo {
    std::string name, uuid;
    int id = 0;
    double x = 0, y = 0, z = 0;
    float yaw = 0, pitch = 0, health = 0;
    bool on_ground = false;
};

class GameApi {
public:
    static GameApi& get();

    bool local_player(JNIEnv* env, EntityInfo& out);
    std::vector<EntityInfo> players(JNIEnv* env);
    bool chat(JNIEnv* env, const std::string& text);

    // HUD drawing. `graphics` is the object the game passes to its render method.
    bool draw_rect(JNIEnv* env, jobject graphics, int x, int y, int w, int h, uint32_t argb);
    bool draw_text(JNIEnv* env, jobject graphics, const std::string& text, int x, int y, uint32_t argb, bool shadow);
    int text_width(JNIEnv* env, const std::string& text);
    bool screen_size(JNIEnv* env, jobject graphics, int& w, int& h);
    // Decides whether `first_arg` (the first argument of the game's HUD render method) is the token this
    // version draws with. `token` may stay null on 1.14-1.15, where nothing is passed.
    bool frame_token(JNIEnv* env, jobject first_arg, jobject& token);

    // One line per accessor: whether it resolved on this version.
    std::string report(JNIEnv* env);

private:
    struct Candidate {
        std::string owner, name, desc;
        bool is_method;
    };
    struct Member {
        jmethodID m = nullptr;
        jfieldID f = nullptr;
        jclass owner = nullptr;  // global ref to the candidate's owner class
        std::string owner_name, name, desc;
        explicit operator bool() const { return m || f; }
        bool returns(char c) const { return !desc.empty() && desc[desc.rfind(')') + 1] == c; }
    };
    struct Slot {
        Member member;
        std::chrono::steady_clock::time_point next_try{};
    };

    GameApi();
    // Resolves a label, retrying every couple of seconds while its classes aren't loaded yet.
    const Member& need(JNIEnv* env, const std::string& label);

    enum class Draw { None, Graphics, PoseStack, Plain };
    Draw draw_mode(JNIEnv* env);
    EntityInfo read(JNIEnv* env, jobject entity);
    jobject minecraft(JNIEnv* env);
    double dbl(JNIEnv* env, const Member& g, jobject o);
    float flt(JNIEnv* env, const Member& g, jobject o);
    int integer(JNIEnv* env, const Member& g, jobject o);
    bool boolean(JNIEnv* env, const Member& g, jobject o);
    std::string to_string(JNIEnv* env, jobject o);
    std::string component_text(JNIEnv* env, jobject component);

    std::vector<std::string> labels_;
    std::map<std::string, std::vector<Candidate>> table_;
    std::map<std::string, Slot> slots_;
};

}  // namespace umc
