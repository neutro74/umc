<p align="center">
  <img src="assets/banner.png" alt="UMC, Universal Minecraft Connector" width="100%">
</p>

<p align="center">
  <a href="https://github.com/neutro74/umc/actions/workflows/build.yml"><img src="https://github.com/neutro74/umc/actions/workflows/build.yml/badge.svg" alt="build"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C" alt="C++17">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20macOS-3d7a2f" alt="platforms">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-blue" alt="Apache 2.0"></a>
</p>

UMC attaches to a running Minecraft Java Edition client or server. Open the REPL to inspect the game, trace methods and load `.umc` plugins. Launch Minecraft normally; no mod or special launch arguments are needed.

## Build

You need CMake 3.16+, a C++17 compiler and a JDK for the JNI/JVMTI headers.

```sh
git clone https://github.com/neutro74/umc
cd umc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/umc
```

Keep `umc` and its agent library together: `libumc.so`, `libumc.dylib` or `umc.dll`. Visual Studio builds usually put them in `build/Release/`. Bridge bytecode is embedded in the agent; Java sources are only used for test fixtures.

## REPL

```text
umc[no game]> ps
umc[no game]> attach 48211
umc[26.3 client]> status
umc[26.3 client]> script load ~/umc/examples/scripts/hud.umc
loaded coords
loaded coords-hud
umc[26.3 client]> script list
umc[26.3 client]> help
```

`script load` expands `~`, copies files into the game's filesystem when needed (including Flatpak), and loads declared dependencies from the same directory first. Each dependency filename is `<id>.umc`; its ID and exact version must match. Already loaded dependencies are reused.

Use `attach <pid>` when several games are running and `use <pid>` to select one in the REPL. `quit` closes the REPL and leaves the agent running. Linux and macOS have history, completion and live logs. Windows uses console line editing and shows logs between commands.

Commands also work from a terminal:

```sh
./build/umc -p 48211 status
./build/umc -p 48211 log -f
```

## Plugins

`.umc` is UMC's scripting language. It supports state, functions, event handlers, commands, method hooks and deferred work. Native modules and JavaScript plugins have been removed.

```text
plugin "tick-counter";
name "Tick Counter";
version "1.0.0";
state ticks = 0;

on tick { ticks = ticks + 1; }
command "count" { return ticks; }
command "reset" { ticks = 0; return "reset"; }
```

```text
umc[26.3 client]> script load tick-counter.umc
umc[26.3 client]> script call tick-counter count
120
umc[26.3 client]> script reload tick-counter
umc[26.3 client]> script unload tick-counter
```

`check <file.umc>` validates a plugin without executing it; add `--json` for metadata and its SHA-256. See [examples/scripts](examples/scripts) and the [language reference](docs/language.md).

### Dependencies

Declare the plugin ID and exact version, then call its exported commands:

```text
plugin "coords-hud";
permissions player, render;
depends "coords" "1.0.0";

on render {
    ui.rect(4, 4, 210, 24, 0x90101418);
    ui.text("XYZ " + plugins.call("coords", "position"), 10, 10, 0xFFE6E8EB);
}
```

A plugin can call only its declared dependencies. The caller must have every permission granted to the dependency, so a call cannot gain extra access. Calls share execution limits. You cannot unload or replace a plugin while another loaded plugin depends on it.

`script load` uses local files. `install` resolves the entire dependency tree through a registry, checks versions and hashes, and shows permissions before asking for approval:

```text
install ~/umc/examples/scripts/tick-counter.umc
search coords
info coords-hud@1.0.0
versions coords-hud
install coords-hud@1.0.0
install https://example.org/plugin.umc
scripts
uninstall tick-counter
```

`search [query]` matches IDs, names, descriptions and authors. `info <id>@<version>` verifies the file and shows permissions and dependencies without executing it. `versions <id>` lists available versions. You can omit the version for `info` and `install` when the registry has only one version; otherwise choose an exact version.

Registry lookup uses `neutro74/umc-registry` by default. Set `UMC_REGISTRY` or pass `--registry <index.tsv>` to use another catalog. A local catalog may reference relative files. Downloads require HTTPS. No plugin can download code itself.

Installed plugins load on attach and daemon injection, with dependencies first. Updating an installed file does not replace its running instance; unload dependents first, then reload the dependency and load dependents again. Installed files live in UMC's config directory under `scripts/`.

The separate registry stores metadata in Git and scripts as release assets. Its sharing page creates catalog submissions, and automated checks accept valid plugin submissions without an AI reviewer. See [registry setup](docs/registry.md).

### Sandbox and permissions

Every plugin uses the same sandbox. There is no trusted mode, unrestricted JVM bridge, native plugin loader, process API or network API. The compiler accepts only `.umc` syntax and permitted calls. It emits internal code for QuickJS; raw JavaScript is never a plugin entry point.

| Permission | Access |
| --- | --- |
| always available | `log`, basic helpers, tick events, commands, `game.version()` |
| `player` | `game.player()` with plain position, rotation, health and identity fields |
| `players` | `game.players()` with nearby player records |
| `chat` | incoming `chat`, outgoing `send`, cancellation and local `game.chat(text)` |
| `combat` | attack events and cancellation |
| `render` | render events and `ui.rect`, `ui.text`, `ui.width`, `ui.screen` |
| `storage` | small files in the plugin's own folder |
| `hooks` | intercept Minecraft methods, read plain arguments and replace plain results |

Declare permissions explicitly: `permissions player, render;`. Omitting the declaration grants none. Direct local loads grant declared permissions; installed plugins use the grants approved at installation. Java objects stay inside the agent; plugins receive primitive values, strings or plain records. Object reads cannot traverse prototypes.

The shared VM has a 64 MB memory limit and 512 KB stack limit. Each invocation has an instruction budget and a time limit (500 ms during load, 50 ms for handlers and commands). A plugin may have 128 scheduled tasks; the engine accepts 32 loaded plugins. These limits bound script execution; native game calls still depend on the game returning.

Storage accepts flat filenames, with no paths, hidden files, symlinks or `..`. Limits: 100 files, 1 MB each, 10 MB total. Data lives under `data/<id>/` in UMC's config directory.

## Commands

| Command | Purpose |
| --- | --- |
| `ps [-a]`, `attach [pid]`, `use [pid]` | find, attach and select games |
| `status`, `threads`, `mem`, `gc`, `game` | inspect JVM and resolved game accessors |
| `check <file.umc> [--json]` | compile without running |
| `search [query]`, `info <id@version>`, `versions <id>` | browse the registry |
| `install <file\|url\|id@version>`, `uninstall <id>`, `scripts` | manage installed plugins |
| `script load\|unload\|reload\|list\|call` | manage running plugins |
| `classes`, `fields`, `methods`, `map` | inspect classes and mappings |
| `get`, `set`, `call` | use static JVM members interactively |
| `trace`, `untrace`, `hooks`, `dump` | trace methods and inspect bytecode |
| `log [n]`, `log -f` | read or follow logs |
| `mappings fetch\|import\|list` | manage mappings |
| `daemon start\|stop\|status\|events\|install\|uninstall` | automatic injection and startup |

Interactive inspection commands have the user's JVM access; plugins cannot invoke those commands. `daemon install` starts UMC at login through systemd, a LaunchAgent or the Windows Run key.

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

## Tests

```sh
bash test/run.sh
bash test/script_api.sh
python3 test/language_test.py
```

The first suite exercises injection, the REPL and the daemon. The second uses a real JVM with fake Minecraft classes to test hooks, cancellation, dependencies, permissions, storage and execution limits. The third checks compiler rejection and dependency installation without a game or network.

CI builds Linux, Windows and macOS, runs real-server injection tests, and checks accessors against mappings for older versions. Client HUD behavior and older real clients still need broader testing.

The agent uses JVMTI to retransform methods. The bytecode rewriter updates stack maps, exception tables and debug tables; [test/stress](test/stress) contains JVM verifier fixtures.

## Layout

```text
src/agent/      injected agent, hooks and plugin runtime
src/cli/        REPL, daemon, installation and injection
src/common/     language compiler and shared platform code
examples/       .umc plugins
docs/           language and registry documentation
test/           fixtures and tests
assets/         logo and banner
```

[Apache 2.0](LICENSE). Keep the license and [NOTICE](NOTICE) when redistributing UMC.
