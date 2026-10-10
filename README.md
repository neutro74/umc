<p align="center">
  <img src="assets/banner.png" alt="UMC, Universal Minecraft Connector" width="100%">
</p>

<p align="center">
  <a href="https://github.com/neutro74/umc/actions/workflows/build.yml"><img src="https://github.com/neutro74/umc/actions/workflows/build.yml/badge.svg" alt="build"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C" alt="C++17">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20macOS-3d7a2f" alt="platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-blue" alt="Apache 2.0"></a>
</p>

UMC is a C++ tool for attaching to Minecraft Java Edition clients and servers. Its REPL lets you inspect the game, trace methods and load JavaScript plugins while Minecraft is running.

It is built for clients, utilities, mods and hacks. Launch Minecraft normally, then inject UMC. You do not need to add a mod to the game or change its launch arguments.

## Build

You need CMake 3.16+, a C++17 compiler and a JDK for the JNI/JVMTI headers.

```sh
git clone https://github.com/neutro74/umc
cd umc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The build produces `umc` and the agent library: `libumc.so`, `libumc.dylib` or `umc.dll`. Keep them together. On Linux and macOS they are in `build/`; Visual Studio builds usually put them in `build/Release/`.

The bridge bytecode is embedded in the C++ agent. There is no production Java source or Java compilation step. The test fixtures still use Java.

## Use the REPL

Start Minecraft, then open UMC:

```sh
./build/umc
```

Running `umc` without arguments in a terminal opens the REPL. `umc shell` and `umc repl` do the same thing explicitly.

```text
umc[no game]> ps
umc[no game]> attach
injected into 48211: minecraft 26.3 client
26.3 is unobfuscated, no mappings needed

umc[26.3 client]> status
umc[26.3 client]> game
umc[26.3 client]> script load examples/scripts/hud.js
umc[26.3 client]> help
```

The prompt shows the connected version and side. Game logs, script output and trace events appear above it. On Linux and macOS, the REPL has saved history and tab completion for commands, module names and script IDs. It does not complete game class or method names yet. Windows uses the console's line editing; live logs are shown between commands.

If several games are running, use `attach <pid>` to inject and `use <pid>` to select the one to control. `use` without a PID returns to automatic selection. `quit` leaves the REPL; the agent stays in the game.

Commands also work directly from your terminal:

```sh
./build/umc -p 48211 status
./build/umc -p 48211 log -f
```

## Automatic injection

The daemon watches for Minecraft launches, waits for the JVM to settle, then injects UMC and loads installed scripts.

```text
umc[no game]> daemon start
umc[no game]> daemon status
umc[no game]> daemon stop
```

`daemon install` starts it at login through a systemd user service on Linux, a LaunchAgent on macOS or the Run registry key on Windows. `daemon uninstall` removes that startup entry. `umc daemon events` follows daemon activity from a terminal.

## Commands

These work in the REPL and as `umc <command>`, except `use`, `clear` and `quit`, which belong to the REPL.

| Command | What it does |
| --- | --- |
| `ps [-a]` | list Minecraft processes; `-a` includes other Java processes |
| `attach [pid] [-l library] [-o options]` | inject the agent |
| `use [pid]` | select a game in the REPL |
| `status`, `threads`, `mem`, `gc` | inspect JVM state |
| `game` | show which game accessors resolved |
| `install <file\|url>`, `uninstall <id>`, `scripts` | manage installed scripts |
| `script load <file.js>`, `script unload <id>`, `script list` | manage scripts in the current game |
| `classes <filter>` | search loaded classes |
| `fields <class>`, `methods <class>` | list members |
| `map <class> [member]` | show mapped names |
| `get <class> <field>`, `set <class> <field> <value>` | read or write static fields |
| `call <class> <method> [args]` | call a static method |
| `trace <class> <method> [descriptor]`, `untrace <id\|all>` | start or stop method tracing |
| `hooks` | list hooks |
| `dump <class> <file>` | save class bytecode |
| `log [n]`, `log -f` | read or follow the agent log |
| `mappings fetch\|import\|list` | manage mappings |
| `daemon start\|stop\|status\|events\|install\|uninstall` | manage automatic injection |
| `help` | command reference |

For native plugins, use `load <library>`, `unload <plugin>` and `reload <plugin>`. Their modules are controlled with `modules`, `enable <name>`, `disable <name>` and `mod <name> [args]`.

## JavaScript plugins

A plugin is a `.js` file. Its header gives it an ID and lists the permissions it needs:

```js
// ==UMC==
// @name Coords HUD
// @id coords-hud
// @version 1.0.0
// @permissions player render
// ==/UMC==

umc.on("render", () => {
    const me = game.player();
    if (!me) return;
    ui.rect(4, 4, 150, 16, 0x90101418);
    ui.text(`XYZ ${me.x.toFixed(1)} ${me.y.toFixed(1)} ${me.z.toFixed(1)}`, 10, 8, 0xFFE6E8EB);
});
```

Load it for the current session with `script load coords.js`. To keep it installed, run `install coords.js` or `install <url>`. UMC shows the requested permissions before asking for approval. Installed scripts load on attach, including daemon injection. `uninstall <id>` removes an installed script.

Scripts live in UMC's config directory: `~/.config/umc/plugins` on Linux, `%APPDATA%\umc\plugins` on Windows and `~/Library/Application Support/umc/plugins` on macOS.

### Permissions

Scripts run in QuickJS with JavaScript memory and execution limits. Only granted APIs are exposed. The `java` permission is an exception to the sandbox: it gives trusted scripts direct JVM access.

| Permission | API |
| --- | --- |
| always available | `umc.log(...)`, `umc.on("tick", fn)`, `game.version` |
| `player` | `game.player()`: name, uuid, id, position, rotation, health, onGround |
| `players` | `game.players()`: nearby players with the same fields |
| `chat` | `game.chat(text)`, incoming `chat` events and outgoing `send` events |
| `combat` | `attack` events and cancellation |
| `render` | `render` events, `ui.rect(...)`, `ui.text(...)`, `ui.width(text)`, `ui.screen()` |
| `storage` | `fs.read`, `fs.write`, `fs.append`, `fs.exists`, `fs.remove`, `fs.list` |
| `java` | Java classes, methods, fields, constructors, arrays and hooks |

`game.chat(text)` displays a message locally. A `chat` handler gets `e.text`; a `send` handler gets `e.text` and `e.command`; an `attack` handler gets `e.target`, which may be `null`. Call `e.cancel()` to stop the game method:

```js
umc.on("send", e => {
    if (e.text === ".coords") {
        umc.log(game.player());
        e.cancel();
    }
});
```

That handler needs `chat` and `player`. Nested chat, send and attack events are suppressed while an event handler runs.

`storage` gives each script a folder at `umc/data/<id>/` under the config directory. It accepts flat file names such as `settings.json`, with no paths, hidden files or `..`. The limits are 100 files, 1 MB per file and 10 MB total. Writes use a temporary file followed by a rename.

Locally loaded scripts receive the permissions in their header. A local script without a header receives every permission, including `java`. Installed scripts use the permissions approved at installation.

### Java access

Use `@permissions java` for code you trust. It can reach Java's file, network and process APIs. Java calls and JVM allocations are outside QuickJS's limits; a blocking Java call can block the game.

```js
const Minecraft = java.type("net.minecraft.client.Minecraft");
const mc = Minecraft.call("getInstance", "()Lnet/minecraft/client/Minecraft;");
const player = mc.get("player", "Lnet/minecraft/client/player/LocalPlayer;");
if (player) umc.log(player.call("getX", "()D"));
```

Names and descriptors are translated through the current mappings. Direct Java calls still need the member name used by that version; `game.player()` and the other built-in game APIs handle historical alternatives for you.

| Operation | Syntax |
| --- | --- |
| get a class | `java.type(className)` |
| construct an object | `Type.new(descriptor, ...args)` |
| call a static method | `Type.call(name, descriptor, ...args)` |
| call an instance method | `object.call(name, descriptor, ...args)` |
| read or write a field | `get(name, descriptor)` / `set(name, descriptor, value)` |
| create an array | `java.array(componentDescriptor, values)` |
| use an array | `array.length()`, `array.at(index)`, `array.put(index, value)` |

Strings and primitive results become JavaScript values; Java `long` uses `BigInt`. Other objects remain Java handles. Final fields cannot be written.

```js
const id = java.hook("net.minecraft.client.Minecraft", "tick", "()V", e => {
    e.proceed();
    // Runs after the original method.
});
```

A hook gets `e.self` and `e.args`. It can call `e.cancel()`, replace the result with `e.return(value)`, or run the original with `e.proceed()`. To change arguments, call `e.setArg(index, value)` before `e.proceed()`.

`java.unhook(id)` removes a hook owned by that script. Unloading a script removes all its hooks. The call object expires when the callback returns, and a handler error disables that hook. Hooks become callable after the script finishes loading. Constructors and class initializers cannot be hooked.

Change game state on the appropriate game thread, usually inside a tick or method hook. The bridge does not yet generate Java classes or implement Java interfaces with JavaScript callbacks.

See [examples/scripts](examples/scripts) for more plugins.

## Native modules

Native plugins are C++ shared libraries with full JVM access and no sandbox. They register modules through `UMC_PLUGIN`; each module can add hooks and handle commands.

The API is in [include/umc/umc.hpp](include/umc/umc.hpp). [examples/tick_counter](examples/tick_counter/tick_counter.cpp) is a complete example. `umc::Call` exposes the receiver, arguments, cancellation, replacement returns and `proceed()`. `umc::Api` resolves readable class, method and field names to JNI handles. A module's hooks are removed when it is disabled.

## Versions

| Versions | Mappings |
| --- | --- |
| 26.1+ | none needed; the game is unobfuscated |
| 1.14.4–1.21.x | Mojang mappings, fetched automatically |
| 1.7.10–1.12.2 | MCP mappings, built from MCP archives |
| 1.13–1.14.3 | no automatic mapping support |

`attach` downloads mappings on first use. You can fetch them ahead of time with `mappings fetch 1.8.9` or supply your own with `mappings import`.

[src/agent/accessors.txt](src/agent/accessors.txt) lists the known names used by the built-in game API. UMC selects the entry that exists in the running game. New names still need to be added to that table; this is not automatic discovery of a new API.

Fabric and Quilt's intermediary names, and older Forge SRG names, are not supported. NeoForge and Forge 1.20.5+ use Mojang names. Mapping support alone does not prove that every client feature works on a version.

## Tests and current status

Linux, Windows and macOS have passed injection tests against vanilla 1.21.4 and 26.3 servers. HUD drawing, position reading and local chat have also been tested on a 26.3 Linux client. Windows and macOS clients, and real pre-1.14 clients, still need testing.

CI builds all three platforms and runs the real-server tests. Linux also runs:

- `test/run.sh`: fake-game injection, scripts, storage, events, daemon and REPL tests.
- `bash test/script_api.sh`: a real JVM test of Java access and cancellable events, without control sockets.
- `test/versions/check.py`: accessor checks against mappings for 20 versions from 1.7.10 to 1.21.8.

The agent uses JVMTI to retransform methods. Each hook adds a call to the embedded bridge, which passes the receiver and boxed arguments to C++. The bytecode rewriter updates stack maps, exception tables and debug tables. The stress test in [test/stress](test/stress) checks rewritten classes with the JVM verifier.

## Layout

```text
include/umc/     native module API
src/agent/      injected agent, hooks, scripting and mappings
src/cli/        REPL, daemon, process discovery and injection
src/common/     shared platform code
examples/       script and native module examples
test/           fixtures and tests
assets/         logo and banner
```

## License

[Apache 2.0](LICENSE). Keep the license and [NOTICE](NOTICE) when redistributing UMC.
