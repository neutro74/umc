// Read access to the running game, resolved through Mojang names with fallbacks for older versions.
#pragma once

#include <jni.h>

#include <chrono>
#include <string>
#include <utility>
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
    bool is_graphics(JNIEnv* env, jobject object);

    // One line per accessor: whether it resolved on this version.
    std::string report(JNIEnv* env);

private:
    struct Getter {
        jmethodID m = nullptr;
        jfieldID f = nullptr;
        explicit operator bool() const { return m || f; }
    };

    bool resolve(JNIEnv* env);
    Getter getter(JNIEnv* env, const char* label, const char* owner,
                  std::vector<std::pair<const char*, const char*>> methods,
                  std::vector<std::pair<const char*, const char*>> fields);
    jmethodID method(JNIEnv* env, const char* label, const char* owner, const char* name, const char* desc);
    EntityInfo read(JNIEnv* env, jobject entity);
    jobject current_player(JNIEnv* env);
    jobject current_level(JNIEnv* env);

    double dbl(JNIEnv* env, const Getter& g, jobject o);
    float flt(JNIEnv* env, const Getter& g, jobject o);
    int integer(JNIEnv* env, const Getter& g, jobject o);
    bool boolean(JNIEnv* env, const Getter& g, jobject o);
    std::string str(JNIEnv* env, jobject o);

    bool resolve_drawing(JNIEnv* env);

    bool resolved_ = false;
    bool drawing_ready_ = false;
    jclass graphics_ = nullptr;
    jmethodID fill_ = nullptr, text_ = nullptr, gui_width_ = nullptr, gui_height_ = nullptr, font_width_ = nullptr;
    bool text_returns_int_ = false;
    jfieldID font_field_ = nullptr;
    jobject font_ = nullptr;  // global ref
    std::chrono::steady_clock::time_point next_try_{};
    std::vector<std::pair<std::string, bool>> report_;

    jclass mc_ = nullptr, component_ = nullptr, text_component_ = nullptr;
    jmethodID get_instance_ = nullptr, players_ = nullptr, literal_ = nullptr, text_ctor_ = nullptr;
    jmethodID get_name_ = nullptr, component_string_ = nullptr, get_uuid_ = nullptr;
    jmethodID send_system_ = nullptr, display_ = nullptr, object_to_string_ = nullptr;
    jfieldID player_ = nullptr, level_ = nullptr;
    Getter x_, y_, z_, yaw_, pitch_, ground_, id_, health_;
};

}  // namespace umc
