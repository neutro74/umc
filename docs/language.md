# UMC language

A file starts with `plugin "id";`. IDs use lowercase letters, digits and hyphens. Optional metadata: `name`, `version` (defaults to `1.0.0`), `author` and `description`, each followed by a quoted string and semicolon. `//` starts a comment.

```text
plugin "example";
version "1.0.0";
permissions player, chat;
depends "coords" "1.0.0";
state calls = 0;
fn twice(x) { return x * 2; }

on send(event) {
    if event.text == ".coords" {
        game.chat(plugins.call("coords", "position"));
        cancel;
    }
}
command "count"(args) { return calls; }
init { after 20 { log("one second later, at 20 TPS"); } }
```

Declarations may contain `state`, `fn`, `on`, `command`, `hook` and `init`. `init` runs during load. State persists until unload or reload. Commands receive an array of string arguments from the REPL; dependency calls can pass an optional array of plain values. Functions may refer to other declared functions.

Statements: `let x = value;`, `x = value;`, `if condition { } else { }`, `while condition { }`, `return value;`, `cancel;`, `after ticks { }`, and `try { } catch(error) { }`. `cancel` is valid in events and method hooks. `after` schedules once on a future tick (1–1,000,000 ticks); unloading cancels pending work.

Values: finite numbers (decimal or `0x` hex), quoted strings, booleans, `null` and arrays. String escapes: `\n`, `\r`, `\t`, `\0`, `\\`, `\"`. Operators: `+ - * / %`, `== != < <= > >=`, `&& || !`. Read fields with `player.x` and arrays with `args[0]`. Collection mutation, arbitrary method calls, object constructors and dynamic code execution are not supported.

Helpers: `log(...)`, `str(value)`, `len(value)`, `round(number)`, `contains(text, fragment)`. `game.version()` returns the detected version. Game, UI and storage calls require their declared permissions; see the README. Missing players and unsupported Java objects become `null`.

Events: `tick`, `render`, `chat`, `send`, `attack`. Chat and send records have `text`; send also has `command`. Attack records have `target`. Nested chat/send/attack events are suppressed during handlers.

```text
plugin "score-wrapper";
permissions hooks;
hook "net.minecraft.client.Minecraft" "computeScore" "(IJLjava/lang/String;)I" {
    log(args[0], args[1]);
    return original() + 1000;
}
```

Hooks use mapped Minecraft class/method names and JVM descriptors. The class must be under `net.minecraft.`; constructors and class initializers cannot be hooked. `args` contains plain primitive/string arguments; other objects become `null`. `original()` runs the original once. A non-void `return` replaces the result after type checking; `cancel;` skips the method. Hook errors disable that hook. Unloading removes all owned hooks.

Files are limited to 256 KB, 32,768 tokens and 64 levels of nesting. String literals are limited to 64 KB, array literals to 1,024 elements and function calls to 32 arguments. Runtime memory, instruction and scheduling limits are described in the README.
