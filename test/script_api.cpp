#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include "agent/core.hpp"
#include "agent/scripts.hpp"
#include "common/language.hpp"
#include "common/platform.hpp"
#include "common/sha256.hpp"
#include "cli/scripts.hpp"
namespace {
void JNICALL on_load(jvmtiEnv*, JNIEnv*, jclass, jobject, const char* name, jobject, jint len, const unsigned char* data, jint* new_len, unsigned char** new_data) {
    umc::Core::get().on_class_file_load(name, len, data, new_len, new_data);
}
void JNICALL on_prepare(jvmtiEnv*, JNIEnv* env, jthread, jclass cls) { umc::Core::get().on_class_prepare(env, cls); }
void require(bool ok, const std::string& message) { if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
}
namespace umc::cli {
JNIEnv* test_env = nullptr;
std::filesystem::path staged_dir;
std::vector<std::string> staged_sources;
bool agent_request(const GameProcess&, const std::vector<std::string>& args, std::string& reply) {
    auto result = Core::get().command(test_env, args);
    reply = (result.first ? "ok\n" : "error\n") + result.second;
    return true;
}
std::string stage_file(const GameProcess&, const std::string& path, std::string& error) {
    if (!std::filesystem::path(path).is_absolute() || path.rfind("~",0)==0) { error="host path was not expanded"; return ""; }
    staged_sources.push_back(path);
    std::filesystem::create_directories(staged_dir);
    auto target = staged_dir / std::filesystem::path(path).filename();
    std::error_code ec; std::filesystem::copy_file(path,target,std::filesystem::copy_options::overwrite_existing,ec);
    if (ec) { error=ec.message(); return ""; }
    return target.string();
}
}
int main(int argc, char** argv) {
    require(argc == 3, "usage: test <classes> <repository>");
    const char* test_home = std::getenv("HOME");
    if (test_home && *test_home) {
        require(umc::plat::expand_home("~/plugins/example.umc") == std::string(test_home) + "/plugins/example.umc", "home path expansion");
        require(umc::plat::expand_home("~") == test_home, "bare home expansion");
    }
    require(umc::plat::expand_home("~another/plugin.umc") == "~another/plugin.umc", "named user path preserved");
    require(umc::plat::expand_home("/tmp/~plugin.umc") == "/tmp/~plugin.umc", "embedded tilde preserved");
    require(umc::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 vector");
    require(umc::sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty SHA-256 vector");
    umc::lang::Compiled compiled; std::string error;
    for (const auto& source : {"plugin \"x\"; init { java.type(\"java.lang.Runtime\"); }", "plugin \"x\"; permissions java;", "plugin \"x\"; init { eval(\"x\"); }", "plugin \"x\"; hook \"java.lang.Runtime\" \"exec\" \"()V\" {}"})
        require(!umc::lang::compile(source, compiled, error), "forbidden capability rejected by compiler");
    require(!umc::lang::compile("plugin \"x\"; state x=" + std::string(200, '!') + "true;", compiled, error), "parser depth bound");
    require(!umc::lang::compile(std::string(umc::lang::max_source_bytes + 1, ' '), compiled, error), "source size bound");
    std::string cp = std::string("-Djava.class.path=") + argv[1]; JavaVMOption options[2]{};
    options[0].optionString=cp.data(); options[1].optionString=const_cast<char*>("-Xcheck:jni");
    JavaVMInitArgs args{}; args.version=JNI_VERSION_1_8;args.nOptions=2;args.options=options;
    JavaVM* vm=nullptr; JNIEnv* env=nullptr;
    require(JNI_CreateJavaVM(&vm,reinterpret_cast<void**>(&env),&args)==JNI_OK,"create JVM");
    jvmtiEnv* ti=nullptr;require(vm->GetEnv(reinterpret_cast<void**>(&ti),JVMTI_VERSION_1_2)==JNI_OK,"JVMTI");
    jvmtiCapabilities caps{};caps.can_retransform_classes=1;require(ti->AddCapabilities(&caps)==JVMTI_ERROR_NONE,"retransform");
    jvmtiEventCallbacks cb{};cb.ClassFileLoadHook=on_load;cb.ClassPrepare=on_prepare;ti->SetEventCallbacks(&cb,sizeof cb);
    ti->SetEventNotificationMode(JVMTI_ENABLE,JVMTI_EVENT_CLASS_FILE_LOAD_HOOK,nullptr);ti->SetEventNotificationMode(JVMTI_ENABLE,JVMTI_EVENT_CLASS_PREPARE,nullptr);
    std::string root=argv[2];require(umc::Core::get().start(vm,ti,env,"version=1.8.9,side=client,mappings="+root+"/test/fakemc18/client.txt"),"start core");
    auto& scripts=umc::ScriptEngine::get();
    auto load=[&](const std::string& name) { bool ok=scripts.load(root+"/test/scripts/"+name+".umc",error);require(ok,"load "+name+": "+error); };
    auto call=[&](const std::string& id,const std::string& command,std::vector<std::string> a=std::vector<std::string>{}) { auto r=scripts.invoke(id,command,a);require(r.first,id+"."+command+": "+r.second);return r.second; };
    umc::cli::test_env = env;
    umc::cli::staged_dir = std::filesystem::path(argv[1]).parent_path() / "staged";
    std::string local_path = root + "/test/scripts/caller.umc";
    if (test_home && local_path.rfind(std::string(test_home)+"/",0)==0) local_path = "~" + local_path.substr(std::string(test_home).size());
    require(umc::cli::load_local_plugin({}, {"script","load",local_path}) == 0, "CLI loads dependencies first through staging");
    require(umc::cli::staged_sources.size()==2 && umc::cli::staged_sources[0].find("base.umc")!=std::string::npos,"dependency staged before caller");
    require(scripts.source("caller").second == root + "/test/scripts/caller.umc", "host source retained for reload");
    require(call("caller","call")=="10","dependency call and functions");
    require(!scripts.invoke("caller","undeclared",{}).first,"undeclared dependency denied");
    require(!scripts.unload("base",error),"in-use dependency cannot unload");
    auto editable = std::filesystem::path(argv[1]).parent_path() / "reload-check.umc";
    { std::ofstream file(editable); file << "plugin \"reload-check\"; command \"value\" { return 1; }"; }
    require(umc::cli::load_local_plugin({}, {"script","load",editable.string()})==0,"load editable source");
    { std::ofstream file(editable); file << "plugin \"reload-check\"; permissions storage; command \"value\" { return 2; } command \"storage\" { return fs.read(\"x\"); }"; }
    auto restaged=umc::cli::stage_file({},editable.string(),error);
    require(scripts.reload("reload-check",error,restaged),"reload freshly staged host edits");
    require(call("reload-check","value")=="2","reload reads edits");
    require(!scripts.invoke("reload-check","storage",{}).first,"reload does not gain new permissions");
    { std::ofstream file(editable); file << "plugin \"changed-id\";"; }
    restaged=umc::cli::stage_file({},editable.string(),error);
    require(!scripts.reload("reload-check",error,restaged),"reload rejects changed ID");
    require(call("reload-check","value")=="2","failed reload preserves existing plugin");
    require(scripts.unload("reload-check",error),"editable cleanup");

    load("private");load("weak");require(!scripts.invoke("weak","read",{}).first,"cross-plugin capability escalation denied");
    require(!scripts.invoke("base","runaway",{}).first,"instruction budget");
    require(!scripts.load(root+"/test/scripts/loop.umc",error),"load-time loop bounded");
    require(!scripts.load(root+"/test/scripts/memory.umc",error),"VM memory bound");
    load("denied");require(!scripts.invoke("denied","storage",{}).first,"storage unavailable without grant");
    require(!scripts.invoke("denied","player",{}).first,"player unavailable without grant");
    load("storage");require(call("storage-check","write")=="hello world","private storage");require(call("storage-check","missing")=="null","missing file");
    require(call("storage-check","list")=="[\"notes.txt\"]","storage list");
    for (const auto& action : {"escape","absolute","hidden","large"}) require(!scripts.invoke("storage-check",action,{}).first,"storage boundary: "+std::string(action));
#ifndef _WIN32
    auto dir=std::filesystem::path(umc::plat::config_dir())/"data"/"storage-check";
    auto outside=std::filesystem::path(umc::plat::config_dir()).parent_path()/"outside.txt";std::ofstream(outside)<<"secret";
    std::filesystem::create_symlink(outside,dir/"link.txt");require(!scripts.invoke("storage-check","symlink",{}).first,"symlink escape denied");
#endif
    require(call("storage-check","remove")=="false","remove storage");
    jclass mc=env->FindClass("ave"); jobject instance=env->CallStaticObjectMethod(mc,env->GetStaticMethodID(mc,"a","()Lave;"));require(!env->ExceptionCheck(),"create game");
    jmethodID tick=env->GetMethodID(mc,"s","()V");
    load("scheduled");require(call("scheduled","count")=="0","task deferred");
    env->CallVoidMethod(instance,tick);require(!env->ExceptionCheck(),"first tick");require(call("scheduled","count")=="0","task not early");
    env->CallVoidMethod(instance,tick);require(!env->ExceptionCheck(),"second tick");require(call("scheduled","count")=="1","task on game thread");
    call("scheduled","later");require(scripts.unload("scheduled",error),"unload tasks");load("scheduled");require(call("scheduled","count")=="0","tasks cannot survive unload");
    load("property");require(!scripts.invoke("property","escape",{}).first,"prototype traversal denied");
    load("probe");jmethodID score=env->GetStaticMethodID(mc,"d","(IJLjava/lang/String;)I");
    jint v=env->CallStaticIntMethod(mc,score,1,jlong(2),env->NewStringUTF("xyz"));require(!env->ExceptionCheck()&&v==1007,"plain hook wraps original");
    call("probe","mode",{"override"});v=env->CallStaticIntMethod(mc,score,1,jlong(2),env->NewStringUTF("xyz"));require(!env->ExceptionCheck()&&v==777,"hook replaces return");
    require(scripts.unload("probe",error),"unload hook");v=env->CallStaticIntMethod(mc,score,1,jlong(2),env->NewStringUTF("xyz"));require(!env->ExceptionCheck()&&v==7,"original restored");
    load("events");
    jclass chat=env->FindClass("bao"),player=env->FindClass("bln"),controller=env->FindClass("bda"),text=env->FindClass("fa");
    jobject chat_obj=env->NewObject(chat,env->GetMethodID(chat,"<init>","()V")), player_obj=env->NewObject(player,env->GetMethodID(player,"<init>","()V")), controller_obj=env->NewObject(controller,env->GetMethodID(controller,"<init>","()V"));
    jmethodID literal=env->GetMethodID(text,"<init>","(Ljava/lang/String;)V"),show=env->GetMethodID(chat,"a","(Leu;I)V"),send=env->GetMethodID(player,"e","(Ljava/lang/String;)V"),attack=env->GetMethodID(controller,"a","(Lwn;Lpk;)V");
    for (const char* message:{"secret stuff","hello world"}) { jobject component=env->NewObject(text,literal,env->NewStringUTF(message));env->CallVoidMethod(chat_obj,show,component,0);require(!env->ExceptionCheck(),"chat"); }
    for (const char* message:{".help","normal message"}) { env->CallVoidMethod(player_obj,send,env->NewStringUTF(message));require(!env->ExceptionCheck(),"send"); }
    for (int i=0;i<2;++i) { env->CallVoidMethod(controller_obj,attack,player_obj,player_obj);require(!env->ExceptionCheck(),"attack"); }
    require(env->GetIntField(chat_obj,env->GetFieldID(chat,"shown","I"))==1,"chat cancellation");
    require(env->GetIntField(player_obj,env->GetFieldID(player,"sent","I"))==1,"send cancellation");
    require(env->GetIntField(controller_obj,env->GetFieldID(controller,"attacked","I"))==1,"attack cancellation");
    std::set<std::string> no_grants;require(scripts.unload("property",error),"unload granted plugin");
    require(scripts.load(root+"/test/scripts/property.umc",error,&no_grants),"load narrowed grants");require(!scripts.invoke("property","escape",{}).first,"narrowed permissions");
    for (const auto& old:{"modules","enable","load","mod"}) require(!umc::Core::get().command(env,{old,"anything"}).first,"native command removed");
    require(!scripts.load(root+"/README.md",error),"only .umc files accepted");
    uint64_t seq=0;for(const auto& line:umc::Core::get().log_since(seq,0)) require(line.find("method hook error:")==std::string::npos&&line.find("error in '")==std::string::npos,line);
    require(scripts.unload("caller",error)&&scripts.unload("base",error)&&scripts.unload("weak",error)&&scripts.unload("private",error),"dependency cleanup order");
    for(const auto& name:scripts.list()) require(scripts.unload(name.substr(0,name.find(' ')),error),"cleanup");
    std::cout<<"all sandboxed UMC language tests passed\n";
}
