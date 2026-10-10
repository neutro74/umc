#include "agent/game.hpp"

#include "agent/core.hpp"
#include "agent/util.hpp"

namespace umc {

namespace {

const char* kMinecraft = "net.minecraft.client.Minecraft";
const char* kEntity = "net.minecraft.world.entity.Entity";
const char* kLiving = "net.minecraft.world.entity.LivingEntity";
const char* kComponent = "net.minecraft.network.chat.Component";
const char* kLocalPlayer = "net.minecraft.client.player.LocalPlayer";
const char* kLevel = "net.minecraft.client.multiplayer.ClientLevel";

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

GameApi::Getter GameApi::getter(JNIEnv* env, const char* label, const char* owner,
                                std::vector<std::pair<const char*, const char*>> methods,
                                std::vector<std::pair<const char*, const char*>> fields) {
    Core& core = Core::get();
    Getter g;
    for (auto& [name, desc] : methods)
        if ((g.m = core.find_method(env, owner, name, desc))) break;
    if (!g.m)
        for (auto& [name, desc] : fields)
            if ((g.f = core.find_field(env, owner, name, desc))) break;
    report_.push_back({label, bool(g)});
    return g;
}

jmethodID GameApi::method(JNIEnv* env, const char* label, const char* owner, const char* name, const char* desc) {
    jmethodID m = Core::get().find_method(env, owner, name, desc);
    report_.push_back({label, m != nullptr});
    return m;
}

bool GameApi::resolve(JNIEnv* env) {
    if (resolved_) return true;
    auto now = std::chrono::steady_clock::now();
    if (now < next_try_) return false;
    next_try_ = now + std::chrono::seconds(2);

    Core& core = Core::get();
    report_.clear();
    jclass mc = core.find_class(env, kMinecraft);
    if (!mc) return false;  // game classes not loaded yet
    mc_ = mc;

    get_instance_ = method(env, "Minecraft.getInstance", kMinecraft, "getInstance", "()Lnet/minecraft/client/Minecraft;");
    player_ = core.find_field(env, kMinecraft, "player", "Lnet/minecraft/client/player/LocalPlayer;");
    level_ = core.find_field(env, kMinecraft, "level", "Lnet/minecraft/client/multiplayer/ClientLevel;");
    report_.push_back({"Minecraft.player", player_ != nullptr});
    report_.push_back({"Minecraft.level", level_ != nullptr});
    players_ = method(env, "ClientLevel.players", kLevel, "players", "()Ljava/util/List;");

    x_ = getter(env, "Entity.x", kEntity, {{"getX", "()D"}}, {{"x", "D"}, {"posX", "D"}});
    y_ = getter(env, "Entity.y", kEntity, {{"getY", "()D"}}, {{"y", "D"}, {"posY", "D"}});
    z_ = getter(env, "Entity.z", kEntity, {{"getZ", "()D"}}, {{"z", "D"}, {"posZ", "D"}});
    yaw_ = getter(env, "Entity.yaw", kEntity, {{"getYRot", "()F"}}, {{"yRot", "F"}, {"yaw", "F"}});
    pitch_ = getter(env, "Entity.pitch", kEntity, {{"getXRot", "()F"}}, {{"xRot", "F"}, {"pitch", "F"}});
    ground_ = getter(env, "Entity.onGround", kEntity, {{"onGround", "()Z"}}, {{"onGround", "Z"}});
    id_ = getter(env, "Entity.id", kEntity, {{"getId", "()I"}}, {{"id", "I"}});
    health_ = getter(env, "LivingEntity.health", kLiving, {{"getHealth", "()F"}}, {});
    get_name_ = method(env, "Entity.getName", kEntity, "getName", "()Lnet/minecraft/network/chat/Component;");
    get_uuid_ = method(env, "Entity.getUUID", kEntity, "getUUID", "()Ljava/util/UUID;");
    component_string_ = method(env, "Component.getString", kComponent, "getString", "()Ljava/lang/String;");

    component_ = core.find_class(env, kComponent);
    literal_ = method(env, "Component.literal", kComponent, "literal",
                      "(Ljava/lang/String;)Lnet/minecraft/network/chat/MutableComponent;");
    if (!literal_) {  // 1.14 to 1.18 build text with a constructor instead
        text_component_ = core.find_class(env, "net.minecraft.network.chat.TextComponent");
        if (text_component_) text_ctor_ = env->GetMethodID(text_component_, "<init>", "(Ljava/lang/String;)V");
        cleared(env);
        report_.push_back({"TextComponent.<init>", text_ctor_ != nullptr});
    }
    send_system_ = method(env, "LocalPlayer.sendSystemMessage", kLocalPlayer, "sendSystemMessage",
                          "(Lnet/minecraft/network/chat/Component;)V");
    if (!send_system_)
        display_ = method(env, "Player.displayClientMessage", kLocalPlayer, "displayClientMessage",
                          "(Lnet/minecraft/network/chat/Component;Z)V");

    jclass object = env->FindClass("java/lang/Object");
    object_to_string_ = env->GetMethodID(object, "toString", "()Ljava/lang/String;");
    env->DeleteLocalRef(object);

    // Without these three nothing can work, so keep retrying until the game is in a state to resolve.
    resolved_ = get_instance_ && player_ && level_;
    return resolved_;
}

std::string GameApi::report(JNIEnv* env) {
    resolve(env);
    if (report_.empty()) return "game classes are not loaded yet\n";
    std::string out;
    for (auto& [label, ok] : report_) out += std::string(ok ? "ok       " : "missing  ") + label + "\n";
    return out;
}

double GameApi::dbl(JNIEnv* env, const Getter& g, jobject o) {
    double v = g.m ? env->CallDoubleMethod(o, g.m) : g.f ? env->GetDoubleField(o, g.f) : 0.0;
    return cleared(env) ? 0.0 : v;
}

float GameApi::flt(JNIEnv* env, const Getter& g, jobject o) {
    float v = g.m ? env->CallFloatMethod(o, g.m) : g.f ? env->GetFloatField(o, g.f) : 0.0f;
    return cleared(env) ? 0.0f : v;
}

int GameApi::integer(JNIEnv* env, const Getter& g, jobject o) {
    int v = g.m ? env->CallIntMethod(o, g.m) : g.f ? env->GetIntField(o, g.f) : 0;
    return cleared(env) ? 0 : v;
}

bool GameApi::boolean(JNIEnv* env, const Getter& g, jobject o) {
    bool v = g.m ? env->CallBooleanMethod(o, g.m) : g.f ? env->GetBooleanField(o, g.f) : false;
    return cleared(env) ? false : v;
}

std::string GameApi::str(JNIEnv* env, jobject o) {
    if (!o || !object_to_string_) return "";
    auto s = static_cast<jstring>(env->CallObjectMethod(o, object_to_string_));
    if (cleared(env) || !s) return "";
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars;
    env->ReleaseStringUTFChars(s, chars);
    env->DeleteLocalRef(s);
    return out;
}

EntityInfo GameApi::read(JNIEnv* env, jobject entity) {
    EntityInfo info;
    info.x = dbl(env, x_, entity);
    info.y = dbl(env, y_, entity);
    info.z = dbl(env, z_, entity);
    info.yaw = flt(env, yaw_, entity);
    info.pitch = flt(env, pitch_, entity);
    info.on_ground = boolean(env, ground_, entity);
    info.id = integer(env, id_, entity);
    info.health = flt(env, health_, entity);
    if (get_name_) {
        jobject component = env->CallObjectMethod(entity, get_name_);
        if (!cleared(env) && component) {
            info.name = component_string_ ? [&] {
                auto s = static_cast<jstring>(env->CallObjectMethod(component, component_string_));
                if (cleared(env) || !s) return std::string();
                const char* chars = env->GetStringUTFChars(s, nullptr);
                std::string out = chars;
                env->ReleaseStringUTFChars(s, chars);
                env->DeleteLocalRef(s);
                return out;
            }() : str(env, component);
            env->DeleteLocalRef(component);
        }
    }
    if (get_uuid_) {
        jobject uuid = env->CallObjectMethod(entity, get_uuid_);
        if (!cleared(env) && uuid) {
            info.uuid = str(env, uuid);
            env->DeleteLocalRef(uuid);
        }
    }
    return info;
}

jobject GameApi::current_player(JNIEnv* env) {
    if (!resolve(env)) return nullptr;
    jobject mc = env->CallStaticObjectMethod(mc_, get_instance_);
    if (cleared(env) || !mc) return nullptr;
    jobject player = env->GetObjectField(mc, player_);
    env->DeleteLocalRef(mc);
    return cleared(env) ? nullptr : player;
}

jobject GameApi::current_level(JNIEnv* env) {
    if (!resolve(env)) return nullptr;
    jobject mc = env->CallStaticObjectMethod(mc_, get_instance_);
    if (cleared(env) || !mc) return nullptr;
    jobject level = env->GetObjectField(mc, level_);
    env->DeleteLocalRef(mc);
    return cleared(env) ? nullptr : level;
}

bool GameApi::local_player(JNIEnv* env, EntityInfo& out) {
    jobject player = current_player(env);
    if (!player) return false;
    out = read(env, player);
    env->DeleteLocalRef(player);
    return true;
}

std::vector<EntityInfo> GameApi::players(JNIEnv* env) {
    std::vector<EntityInfo> out;
    jobject level = current_level(env);
    if (!level || !players_) return out;
    jobject list = env->CallObjectMethod(level, players_);
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
    jobject player = current_player(env);
    if (!player) return false;
    jstring js = env->NewStringUTF(text.c_str());
    jobject component = nullptr;
    if (literal_ && component_) component = env->CallStaticObjectMethod(component_, literal_, js);
    else if (text_ctor_) component = env->NewObject(text_component_, text_ctor_, js);
    bool ok = !cleared(env) && component && (send_system_ || display_);
    if (ok) {
        if (send_system_) env->CallVoidMethod(player, send_system_, component);
        else env->CallVoidMethod(player, display_, component, JNI_FALSE);
        ok = !cleared(env);
    }
    env->DeleteLocalRef(player);
    return ok;
}

}  // namespace umc
