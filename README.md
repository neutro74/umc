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
- **Script plugins.** Write a plugin in plain JavaScript, no compiler needed. It runs in a sandbox with no file, network or system access, can only use the parts of the game you approve, and can draw on your HUD. Share it as a single `.js` file.
- **Method hooks from C++.** Run code at the start of any method, read its arguments, cancel it, replace its return value, or run the original and change what it returns.
- **Any version, 1.7 to 26.x.** Mojang's mappings (1.14.4 and later) and MCP's (1.7 to 1.12) are fetched and applied automatically, so you always write the same readable names. 26.1 and later ship unobfuscated and need nothing.
- **Inspection from the terminal.** List classes, fields and methods with readable names, call static methods, read and write static fields, trace calls with their arguments, dump class bytecode.
- **Cross-platform.** Linux, Windows and macOS, client and server. Every push injects into a real vanilla server on all three.

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
| `install <file\|url>`, `uninstall <id>`, `scripts` | install, remove and list script plugins |
| `script load <file.js>`, `script unload <id>`, `script list` | run a script without installing it |
| `load <lib\|script.js>`, `unload <plugin>`, `reload <plugin>` | manage native plugins in the running game |
| `game` | which game accessors resolved on this version |
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

## Script plugins

A plugin is one JavaScript file. A header at the top says what it needs:

```js
// ==UMC==
// @name         Coords HUD
// @id           coords-hud
// @version      1.0.0
// @author       you
// @description  Your position and the direction you're facing, on screen.
// @permissions  player render
// ==/UMC==

umc.on("render", () => {
    const me = game.player();
    if (!me) return;
    ui.rect(4, 4, 150, 16, 0x90101418);
    ui.text(`XYZ ${me.x.toFixed(1)} ${me.y.toFixed(1)} ${me.z.toFixed(1)}`, 10, 8, 0xFFE6E8EB);
});
```

Install it from a file or a link. UMC shows what it asks for, in plain words, and waits for a yes:

```
$ umc install https://example.com/coords-hud.js

  Coords HUD  v1.0.0  by you
  Your position and the direction you're facing, on screen.

  It asks permission to:
    - read your position, rotation, health and name  (player)
    - draw on your screen (the HUD)  (render)

Install Coords HUD? [y/N]
```

Installed scripts load every time you attach. `umc scripts` lists them and `umc uninstall <id>` removes one.

**What a script can do.** Each script runs in its own QuickJS sandbox. It has no file, network, process or Java access, a memory cap, and a time limit on every call, so an infinite loop can't freeze the game. The only things it can reach are the objects below, and only the ones its approved permissions unlock. A script that asks for something it wasn't given just gets `undefined`.

| Permission | Unlocks |
| --- | --- |
| *(always)* | `umc.log(...)`, `umc.on("tick", fn)`, `game.version` |
| `player` | `game.player()`: name, uuid, id, x, y, z, yaw, pitch, health, onGround |
| `players` | `game.players()`: the same for every player around you |
| `chat` | `game.chat(text)`: a message in your own chat window |
| `render` | `umc.on("render", fn)`, `ui.rect(x, y, w, h, argb)`, `ui.text(text, x, y, argb)`, `ui.width(text)`, `ui.screen()` |
| `storage` | `fs.read(name)`, `fs.write(name, text)`, `fs.append(name, text)`, `fs.exists(name)`, `fs.remove(name)`, `fs.list()` |

**Storage is a private folder, not your disk.** A script with `storage` gets one folder of its own (`umc/data/<script id>/` in UMC's config directory) and nothing else on the computer is reachable. File names are plain (`settings.json`: letters, digits, `.`, `_`, `-`), so there are no paths, no `..` and no hidden files. A script can keep at most 100 files, 1 MB each and 10 MB in total, and writes are atomic, so a crash can't leave half a file behind.

Scripts you load yourself with `umc script load <file>` are treated as local development code: they get what their header declares, or everything if they have no header. Examples are in [`examples/scripts`](examples/scripts).

## Writing a native module

Scripts are the way to share plugins with other people: they're sandboxed and a user can read them. Native modules are for your own tools and for work that needs full access to the JVM. They're ordinary shared libraries with no sandbox, so only load ones you built or trust.

A native plugin is a shared library that registers one or more modules. Names are always Mojang's readable names; UMC maps them to whatever the running game uses.

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

## Version support

UMC resolves the same readable names on every version, so a script written once keeps working.

| Versions | Where the names come from |
| --- | --- |
| 26.1 and later | the game itself, which ships unobfuscated |
| 1.14.4 to 1.21.x | Mojang's official mappings, downloaded automatically |
| 1.7.10 to 1.12.2 | MCP names, built automatically from the MCP archives (this is the "old combat" range) |
| 1.13 to 1.14.3 | not supported: no mappings were ever published for them |

`umc attach` fetches whatever the running version needs the first time, into `~/.cache/umc/mappings`. You can also run `umc mappings fetch 1.8.9` ahead of time, or `umc mappings import` to use your own mapping file.

The script API stays small and the differences between versions are handled underneath it. [`src/agent/accessors.txt`](src/agent/accessors.txt) lists, for every piece of game data UMC reads, the names it has had over the years (`getX()`, `posX`, `getName()` returning a component or a string, three different ways to draw on the HUD). The agent tries them in order and uses whichever the running version has. `test/versions/check.py` verifies the whole table against the real mappings for 23 versions from 1.7.10 to 1.21.8, and runs in CI.

This is a table of known names, not automatic discovery: when Mojang renames something new, one line gets added to the table.

Mod loaders that remap the game at runtime need their own naming scheme. NeoForge and Forge 1.20.5+ run on Mojang names and work as-is. Fabric and Quilt use intermediary names, and Forge before 1.20.5 uses SRG names; neither is supported yet.

## Status

| | Linux | Windows | macOS |
| --- | --- | --- | --- |
| Builds | ✓ | ✓ | ✓ |
| Injects into a real vanilla server | ✓ | ✓ | ✓ |
| Fake-game end-to-end suite | ✓ | not run | not run |
| Live game client | ✓ (26.3) | not yet | not yet |

UMC is early. Every push downloads real Minecraft servers (1.21.4 with mappings, 26.3 without) on all three systems, injects, and checks version and side detection, mappings, hooks, traces and scripts. A larger suite (`test/run.sh`) runs against a small fake game on Linux, including a fake 1.8-style game that uses MCP names. The client HUD, position reading and chat have been checked on a live 26.3 client on Linux; the client on Windows and macOS, and any real pre-1.14 game, have not been tried yet.

Planned next: a background daemon that watches for game launches and injects automatically, an interactive shell with history, completion and live events, and a proper catalog for sharing script plugins.

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
