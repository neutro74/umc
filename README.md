<p align="center">
  <img src="assets/banner.png" alt="UMC, Universal Minecraft Connector" width="100%">
</p>

<p align="center">
  <a href="https://github.com/neutro74/umc/actions/workflows/build.yml"><img src="https://github.com/neutro74/umc/actions/workflows/build.yml/badge.svg" alt="build"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C" alt="C++17">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20macOS-3d7a2f" alt="platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-blue" alt="Apache 2.0"></a>
</p>

UMC injects a native agent into a running Minecraft Java Edition client or server and gives you a C++ API and a CLI to hook methods, inspect classes, read and write fields, and load your own modules. It works on any version: obfuscated releases are translated through Mojang's official mappings, and 26.1+ (which ships unobfuscated) needs nothing at all.

It is meant as a foundation for clients, utilities, mods and hacks. Nothing is installed into the game; you start Minecraft however you normally do and attach to it.

```
$ umc attach
injected into 48211: minecraft 26.3 client
26.3 is unobfuscated, no mappings needed

$ umc trace net.minecraft.world.entity.player.Player jumpFromGround
tracing net.minecraft.world.entity.player.Player.jumpFromGround as #1, watch with `log` or `follow`

$ umc log -f
trace net.minecraft.world.entity.player.Player.jumpFromGround()
```

## Features

- **Injection only.** Attach to a game that's already running, from any launcher. Files the game needs are staged where sandboxed launchers (like the Prism Flatpak) can read them.
- **Method hooks from C++.** Run code at the start of any method, read its arguments, cancel it, or replace its return value. Hooks can be added before the class has loaded.
- **Any version.** Mojang's ProGuard mappings are downloaded and applied automatically for obfuscated versions, so you always write `net.minecraft.client.Minecraft.tick`, never `flk.t`.
- **Plugins.** Modules are plain shared libraries built against one header. Load, unload and reload them while the game runs.
- **Inspection from the terminal.** List classes, fields and methods with readable names, call static methods, read and write static fields, trace calls with their arguments, dump class bytecode.
- **Cross-platform.** Linux, Windows and macOS, client and server.

## How it works

The CLI loads the agent library into the game's JVM: through HotSpot's attach mechanism on Linux and macOS, and with a `LoadLibrary` remote thread on Windows. Inside the game the agent:

1. defines a small `umc.Bridge` class in the bootstrap class loader, so every game class can call it,
2. detects the Minecraft version and side, and loads mappings if the version needs them,
3. opens a local control socket that the CLI talks to.

Hooks rewrite bytecode through JVMTI class retransformation. The hooked method gets a short prologue that calls into native code with `this` and the boxed arguments, and returns early if the hook asks it to. Stack map frames, exception tables and debug tables are rewritten to match, so the result passes the JVM verifier. The rewriter is tested by injecting a hook into every one of the 64,282 methods in the 1.21.4 client and linking all 8,857 classes with zero verify errors.

## Getting started

You need CMake 3.16+, a C++17 compiler and a JDK (for the JNI headers and to compile the bridge class).

```sh
git clone https://github.com/neutro74/umc
cd umc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

This produces `build/umc` (the CLI) and the agent library next to it (`libumc.so`, `libumc.dylib` or `umc.dll`). Keep them in the same directory.

Start Minecraft, then:

```sh
umc ps                 # find running games
umc attach             # inject into the only running game (or: umc attach <pid>)
umc status             # version, side, mappings, hooks, modules
```

When several games are running, pick one with `-p <pid>` in front of any command.

## Commands

| | |
| --- | --- |
| `ps [-a]` | list running Minecraft JVMs (`-a` shows every JVM) |
| `attach [pid] [-l plugin]... [-o opts]` | inject into a game, optionally loading plugins |
| `mappings fetch\|import\|list` | manage the mappings cache |
| `status`, `threads`, `mem`, `gc` | game and JVM state |
| `modules`, `enable <m>`, `disable <m>`, `mod <m> [args]` | control modules |
| `load <lib>`, `unload <plugin>`, `reload <plugin>` | manage plugins in the running game |
| `classes <filter>` | search loaded classes |
| `fields <class>`, `methods <class>` | members with their readable names |
| `map <class> [member]` | show how a name is mapped |
| `get <class> <field>`, `set <class> <field> <value>` | read or write a static field |
| `call <class> <method> [args]` | call a static method |
| `trace <class> <method> [desc]`, `untrace <id\|all>` | log every call with its arguments |
| `dump <class> <file>` | save a class's bytecode |
| `hooks` | list active hooks |
| `log [n]`, `log -f` | agent log, or follow it live |

Plugins in `~/.config/umc/plugins` (`%APPDATA%\umc\plugins` on Windows, `~/Library/Application Support/umc/plugins` on macOS) are loaded on every attach.

## Writing a module

A plugin is a shared library that registers one or more modules. Names are always Mojang's readable names; UMC maps them to whatever the running game uses.

```cpp
#include <atomic>

#include <umc/umc.hpp>

class TickCounter : public umc::Module {
public:
    std::string name() const override { return "tick_counter"; }

    void on_enable(umc::Api& api, JNIEnv*) override {
        hook(api, "net.minecraft.client.Minecraft", "tick", "()V", [this](umc::Call& call) {
            ++ticks_;
            if (frozen_) call.cancel();  // skip the original tick
        });
    }

    std::string on_command(umc::Api&, JNIEnv*, const std::vector<std::string>& args) override {
        if (!args.empty() && args[0] == "freeze") frozen_ = !frozen_;
        return std::to_string(ticks_) + " ticks";
    }

private:
    std::atomic<long> ticks_{0};    // hooks run on the game thread, commands on another
    std::atomic<bool> frozen_{false};
};

UMC_PLUGIN {
    api->add_module(std::make_unique<TickCounter>());
}
```

```sh
umc load ./tick_counter.so
umc enable tick_counter
umc mod tick_counter freeze
```

Hooks added through `Module::hook` are removed when the module is disabled. Inside a hook, `umc::Call` gives you `self()`, `arg(i)` and its typed variants, `cancel()`, and `return_int/long/float/double/object()`. For anything else, `Api` resolves classes, methods and fields to plain JNI handles: `find_class`, `find_method`, `find_field`. The full API is in [`include/umc/umc.hpp`](include/umc/umc.hpp), and a complete example is in [`examples/tick_counter`](examples/tick_counter/tick_counter.cpp).

## Mappings

Minecraft was obfuscated until 26.1. For those versions UMC needs Mojang's mappings, which `attach` downloads automatically the first time it sees a version, into `~/.cache/umc/mappings`. You can also fetch them ahead of time with `umc mappings fetch 1.21.4`, or import your own ProGuard-format file with `umc mappings import`.

Versions older than 1.14.4 have no official mappings. Those work if you supply a ProGuard-format mapping file yourself.

Mod loaders that remap the game at runtime need their own naming scheme. NeoForge and Forge 1.20.5+ run on Mojang names and work as-is. Fabric and Quilt use intermediary names, which aren't supported yet.

## Status

| | Linux | Windows | macOS |
| --- | --- | --- | --- |
| Builds | ✓ | ✓ | ✓ |
| End-to-end tests | ✓ | not yet | not yet |
| Live Minecraft | not yet | not yet | not yet |

UMC is early. The test suite (`test/run.sh`) injects into a small fake obfuscated game and covers injection, mappings, inspection, hooks, return overrides, cancellation, tracing and plugin reloads, and the bytecode rewriter is verified against the real 1.21.4 client. It has not yet been run against a live game, and Windows and macOS support is implemented but hasn't been run on those systems.

Planned next: a background daemon that watches for game launches and injects automatically, and an interactive shell with history, completion and live events.

## Layout

```
include/umc/      public API for modules
src/agent/        the injected library: bytecode rewriter, hooks, mappings, commands
src/cli/          the umc CLI: process discovery, injection per OS, mappings download
src/common/       platform layer shared by both
src/java/         the bridge class defined into the game
examples/         example plugin
test/             end-to-end tests and the bytecode stress test
assets/           logo and banner, with the Blender scripts that render them
```

## A note on servers

Hooking a client you run on a server someone else owns can break that server's rules and get you banned. Use it on your own worlds and servers, or where it's allowed.

## License

Apache 2.0. You can use, change and redistribute UMC, including in closed-source projects, as long as you keep the [license](LICENSE) and the [NOTICE](NOTICE) file crediting UMC.
