#include "agent/game.hpp"

#include <sstream>

#include "accessors_table.h"
#include "agent/core.hpp"
#include "agent/util.hpp"

namespace umc {

namespace {

bool cleared(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

}  // namespace

GameApi& GameApi::get() {
    static GameApi api;
    return api;
}

GameApi::GameApi() {
    std::istringstream in(std::string(reinterpret_cast<const char*>(kAccessorsTable), sizeof kAccessorsTable));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string label, owner, alt;
        if (!(ls >> label) || label[0] == '#' || !(ls >> owner)) continue;
        if (!table_.count(label)) labels_.push_back(label);
        while (ls >> alt) {
            size_t a = alt.find(':'), b = alt.find(':', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            table_[label].push_back({owner, alt.substr(a + 1, b - a - 1), alt.substr(b + 1), alt[0] == 'm'});
        }
    }
}

const GameApi::Member& GameApi::need(JNIEnv* env, const std::string& label) {
    static const Member none;
    Slot& slot = slots_[label];
    if (slot.member) return slot.member;
    auto now = std::chrono::steady_clock::now();
    if (now < slot.next_try) return none;
    slot.next_try = now + std::chrono::seconds(2);

    Core& core = Core::get();
    for (const Candidate& c : table_[label]) {
        Member found;
        if (c.is_method) found.m = core.find_method(env, c.owner, c.name, c.desc);
        else found.f = core.find_field(env, c.owner, c.name, c.desc);
        if (!found) continue;
        jclass owner = core.find_class(env, c.owner);
        found.owner = owner;
        found.owner_name = c.owner;
        found.name = c.name;
        found.desc = c.desc;
        slot.member = found;
        return slot.member;
    }
    return none;
}

std::string GameApi::report(JNIEnv* env) {
    std::string out;
    for (const std::string& label : labels_) {
        const Member& m = need(env, label);
        out += std::string(m ? "ok       " : "missing  ") + label;
        if (m) out += "  " + m.name;
        out += "\n";
    }
    return out;
}

double GameApi::dbl(JNIEnv* env, const Member& g, jobject o) {
    double v = g.m ? env->CallDoubleMethod(o, g.m) : g.f ? env->GetDoubleField(o, g.f) : 0.0;
    return cleared(env) ? 0.0 : v;
}

float GameApi::flt(JNIEnv* env, const Member& g, jobject o) {
    float v = g.m ? env->CallFloatMethod(o, g.m) : g.f ? env->GetFloatField(o, g.f) : 0.0f;
    return cleared(env) ? 0.0f : v;
}

int GameApi::integer(JNIEnv* env, const Member& g, jobject o) {
    int v = g.m ? env->CallIntMethod(o, g.m) : g.f ? env->GetIntField(o, g.f) : 0;
    return cleared(env) ? 0 : v;
}

bool GameApi::boolean(JNIEnv* env, const Member& g, jobject o) {
    bool v = g.m ? env->CallBooleanMethod(o, g.m) : g.f ? env->GetBooleanField(o, g.f) : false;
    return cleared(env) ? false : v;
}

std::string GameApi::to_string(JNIEnv* env, jobject o) {
    if (!o) return "";
    jclass object = env->FindClass("java/lang/Object");
    jmethodID m = env->GetMethodID(object, "toString", "()Ljava/lang/String;");
    env->DeleteLocalRef(object);
    auto s = static_cast<jstring>(env->CallObjectMethod(o, m));
    if (cleared(env) || !s) return "";
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars;
    env->ReleaseStringUTFChars(s, chars);
    env->DeleteLocalRef(s);
    return out;
}

std::string GameApi::component_text(JNIEnv* env, jobject component) {
    const Member& get_string = need(env, "component.string");
    if (!get_string.m) return to_string(env, component);
    auto s = static_cast<jstring>(env->CallObjectMethod(component, get_string.m));
    if (cleared(env) || !s) return "";
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars;
    env->ReleaseStringUTFChars(s, chars);
    env->DeleteLocalRef(s);
    return out;
}

EntityInfo GameApi::read(JNIEnv* env, jobject entity) {
    EntityInfo info;
    info.x = dbl(env, need(env, "entity.x"), entity);
    info.y = dbl(env, need(env, "entity.y"), entity);
    info.z = dbl(env, need(env, "entity.z"), entity);
    info.yaw = flt(env, need(env, "entity.yaw"), entity);
    info.pitch = flt(env, need(env, "entity.pitch"), entity);
    info.on_ground = boolean(env, need(env, "entity.onGround"), entity);
    info.id = integer(env, need(env, "entity.id"), entity);
    info.health = flt(env, need(env, "living.health"), entity);
    if (const Member& name = need(env, "entity.name"); name.m) {
        jobject value = env->CallObjectMethod(entity, name.m);
        if (!cleared(env) && value) {
            info.name = name.desc.find("Ljava/lang/String;") != std::string::npos && name.returns('L') &&
                                name.desc.substr(name.desc.rfind(')') + 1) == "Ljava/lang/String;"
                            ? to_string(env, value)
                            : component_text(env, value);
            env->DeleteLocalRef(value);
        }
    }
    if (const Member& uuid = need(env, "entity.uuid"); uuid.m) {
        jobject value = env->CallObjectMethod(entity, uuid.m);
        if (!cleared(env) && value) {
            info.uuid = to_string(env, value);
            env->DeleteLocalRef(value);
        }
    }
    return info;
}

jobject GameApi::minecraft(JNIEnv* env) {
    const Member& get = need(env, "minecraft.getInstance");
    if (!get.m || !get.owner) return nullptr;
    jobject mc = env->CallStaticObjectMethod(get.owner, get.m);
    return cleared(env) ? nullptr : mc;
}

bool GameApi::local_player(JNIEnv* env, EntityInfo& out) {
    const Member& field = need(env, "minecraft.player");
    jobject mc = field.f ? minecraft(env) : nullptr;
    if (!mc) return false;
    jobject player = env->GetObjectField(mc, field.f);
    env->DeleteLocalRef(mc);
    if (cleared(env) || !player) return false;
    out = read(env, player);
    env->DeleteLocalRef(player);
    return true;
}

std::vector<EntityInfo> GameApi::players(JNIEnv* env) {
    std::vector<EntityInfo> out;
    const Member& level_field = need(env, "minecraft.level");
    const Member& players = need(env, "level.players");
    jobject mc = level_field.f && players ? minecraft(env) : nullptr;
    if (!mc) return out;
    jobject level = env->GetObjectField(mc, level_field.f);
    env->DeleteLocalRef(mc);
    if (cleared(env) || !level) return out;
    jobject list = players.m ? env->CallObjectMethod(level, players.m) : env->GetObjectField(level, players.f);
    env->DeleteLocalRef(level);
    if (cleared(env) || !list) return out;

    jclass list_class = env->FindClass("java/util/List");
    jmethodID size = env->GetMethodID(list_class, "size", "()I");
    jmethodID get = env->GetMethodID(list_class, "get", "(I)Ljava/lang/Object;");
    jint count = env->CallIntMethod(list, size);
    for (jint i = 0; i < count && !cleared(env); ++i) {
        jobject entity = env->CallObjectMethod(list, get, i);
        if (cleared(env) || !entity) continue;
        out.push_back(read(env, entity));
        env->DeleteLocalRef(entity);
    }
    env->DeleteLocalRef(list_class);
    env->DeleteLocalRef(list);
    return out;
}

bool GameApi::chat(JNIEnv* env, const std::string& text) {
    const Member& send = need(env, "chat.send");
    const Member& literal = need(env, "component.literal");
    const Member& player_field = need(env, "minecraft.player");
    if (!send.m || !literal.m || !literal.owner || !player_field.f) return false;
    jobject mc = minecraft(env);
    if (!mc) return false;
    jobject player = env->GetObjectField(mc, player_field.f);
    env->DeleteLocalRef(mc);
    if (cleared(env) || !player) return false;

    jstring js = env->NewStringUTF(text.c_str());
    jobject component = literal.name == "<init>" ? env->NewObject(literal.owner, literal.m, js)
                                                  : env->CallStaticObjectMethod(literal.owner, literal.m, js);
    bool ok = !cleared(env) && component;
    if (ok) {
        if (send.desc.find("UUID") != std::string::npos) {
            jclass uuid_class = env->FindClass("java/util/UUID");
            jobject nil = env->NewObject(uuid_class, env->GetMethodID(uuid_class, "<init>", "(JJ)V"), jlong(0), jlong(0));
            env->CallVoidMethod(player, send.m, component, nil);
            env->DeleteLocalRef(uuid_class);
        } else if (send.desc.find("Z)V") != std::string::npos) {
            env->CallVoidMethod(player, send.m, component, JNI_FALSE);
        } else {
            env->CallVoidMethod(player, send.m, component);
        }
        ok = !cleared(env);
    }
    env->DeleteLocalRef(player);
    return ok;
}

GameApi::Draw GameApi::draw_mode(JNIEnv* env) {
    const Member& fill = need(env, "gfx.fill");
    if (!fill.m) return Draw::None;
    if (fill.owner_name.find("GuiGraphics") != std::string::npos) return Draw::Graphics;
    return fill.desc.find("PoseStack") != std::string::npos ? Draw::PoseStack : Draw::Plain;
}

bool GameApi::frame_token(JNIEnv* env, jobject first_arg, jobject& token) {
    token = nullptr;
    switch (draw_mode(env)) {
    case Draw::Graphics:
        token = first_arg;
        return first_arg && env->IsInstanceOf(first_arg, need(env, "gfx.fill").owner);
    case Draw::PoseStack: {
        jclass pose = Core::get().find_class(env, "com.mojang.blaze3d.vertex.PoseStack");
        token = first_arg;
        return pose && first_arg && env->IsInstanceOf(first_arg, pose);
    }
    case Draw::Plain: return true;
    default: return false;
    }
}

bool GameApi::draw_rect(JNIEnv* env, jobject token, int x, int y, int w, int h, uint32_t argb) {
    const Member& fill = need(env, "gfx.fill");
    Draw mode = draw_mode(env);
    if (mode == Draw::None) return false;
    if (mode == Draw::Graphics) {
        env->CallVoidMethod(token, fill.m, x, y, x + w, y + h, jint(argb));
    } else {
        jvalue args[6];
        int n = 0;
        if (mode == Draw::PoseStack) args[n++].l = token;
        args[n++].i = x;
        args[n++].i = y;
        args[n++].i = x + w;
        args[n++].i = y + h;
        args[n++].i = jint(argb);
        env->CallStaticVoidMethodA(fill.owner, fill.m, args);
    }
    return !cleared(env);
}

bool GameApi::draw_text(JNIEnv* env, jobject token, const std::string& text, int x, int y, uint32_t argb, bool shadow) {
    const Member& draw = need(env, "gfx.text");
    const Member& font_field = need(env, "minecraft.font");
    Draw mode = draw_mode(env);
    if (!draw.m || !font_field.f || mode == Draw::None) return false;
    jobject mc = minecraft(env);
    if (!mc) return false;
    jobject font = env->GetObjectField(mc, font_field.f);
    env->DeleteLocalRef(mc);
    jstring js = env->NewStringUTF(text.c_str());
    if (mode == Draw::Graphics) {
        if (draw.returns('I')) env->CallIntMethod(token, draw.m, font, js, x, y, jint(argb), jboolean(shadow));
        else env->CallVoidMethod(token, draw.m, font, js, x, y, jint(argb), jboolean(shadow));
    } else {
        jvalue args[5];
        int n = 0;
        if (mode == Draw::PoseStack) args[n++].l = token;
        args[n++].l = js;
        if (draw.desc.find("III") != std::string::npos) {  // 1.7 takes integer coordinates
            args[n++].i = x;
            args[n++].i = y;
        } else {
            args[n++].f = jfloat(x);
            args[n++].f = jfloat(y);
        }
        args[n++].i = jint(argb);
        env->CallIntMethodA(font, draw.m, args);
    }
    env->DeleteLocalRef(font);
    return !cleared(env);
}

int GameApi::text_width(JNIEnv* env, const std::string& text) {
    const Member& width = need(env, "font.width");
    const Member& font_field = need(env, "minecraft.font");
    if (!width.m || !font_field.f) return int(text.size()) * 6;
    jobject mc = minecraft(env);
    if (!mc) return 0;
    jobject font = env->GetObjectField(mc, font_field.f);
    env->DeleteLocalRef(mc);
    jstring js = env->NewStringUTF(text.c_str());
    int w = env->CallIntMethod(font, width.m, js);
    env->DeleteLocalRef(font);
    return cleared(env) ? 0 : w;
}

bool GameApi::screen_size(JNIEnv* env, jobject, int& w, int& h) {
    jobject mc = minecraft(env);
    if (!mc) return false;
    bool ok = false;
    const Member& window = need(env, "minecraft.window");
    const Member& gw = need(env, "window.width");
    const Member& gh = need(env, "window.height");
    if (window && gw.m && gh.m) {
        jobject win = window.m ? env->CallObjectMethod(mc, window.m) : env->GetObjectField(mc, window.f);
        if (!cleared(env) && win) {
            w = env->CallIntMethod(win, gw.m);
            h = env->CallIntMethod(win, gh.m);
            env->DeleteLocalRef(win);
            ok = !cleared(env);
        }
    } else {
        const Member& make = need(env, "scaled.new");
        const Member& sw = need(env, "scaled.width");
        const Member& sh = need(env, "scaled.height");
        if (make.m && make.owner && sw.m && sh.m && make.desc == "(Lnet/minecraft/client/Minecraft;)V") {
            jobject scaled = env->NewObject(make.owner, make.m, mc);
            if (!cleared(env) && scaled) {
                w = env->CallIntMethod(scaled, sw.m);
                h = env->CallIntMethod(scaled, sh.m);
                env->DeleteLocalRef(scaled);
                ok = !cleared(env);
            }
        }
    }
    env->DeleteLocalRef(mc);
    return ok;
}

std::vector<GameApi::HookTarget> GameApi::hook_targets(JNIEnv* env, const std::string& label) {
    std::vector<HookTarget> out;
    Core& core = Core::get();
    auto it = table_.find(label);
    if (it == table_.end()) return out;
    for (const Candidate& c : it->second) {
        bool exists = core.remapped() ? core.mapped_method(c.owner, c.name, c.desc)
                                      : core.find_method(env, c.owner, c.name, c.desc) != nullptr;
        if (exists) out.push_back({c.owner, c.name, c.desc});
    }
    return out;
}

std::string GameApi::text_of(JNIEnv* env, jobject message) {
    if (!message) return "";
    jclass string_class = env->FindClass("java/lang/String");
    bool is_string = env->IsInstanceOf(message, string_class);
    env->DeleteLocalRef(string_class);
    if (is_string) {
        const char* chars = env->GetStringUTFChars(static_cast<jstring>(message), nullptr);
        std::string out = chars;
        env->ReleaseStringUTFChars(static_cast<jstring>(message), chars);
        return out;
    }
    return component_text(env, message);
}

bool GameApi::entity_info(JNIEnv* env, jobject entity, EntityInfo& out) {
    if (!entity) return false;
    out = read(env, entity);
    return true;
}

bool GameApi::hook_target(JNIEnv* env, const std::string& label, std::string& owner, std::string& name, std::string& desc) {
    Core& core = Core::get();
    const auto it = table_.find(label);
    if (it == table_.end()) return false;
    for (const Candidate& c : it->second) {
        bool exists = core.remapped() ? core.mapped_method(c.owner, c.name, c.desc)
                                      : core.find_method(env, c.owner, c.name, c.desc) != nullptr;
        if (exists) {
            owner = c.owner, name = c.name, desc = c.desc;
            return true;
        }
    }
    if (core.remapped() || it->second.empty()) return false;
    // Unobfuscated and the class isn't loaded yet: assume the newest layout.
    const Candidate& first = it->second.front();
    owner = first.owner, name = first.name, desc = first.desc;
    return true;
}

}  // namespace umc
