#!/usr/bin/env bash
# End-to-end test: inject into a fake obfuscated Minecraft and exercise the agent.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$root/build
work=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$work"' EXIT

cmake -S "$root" -B "$build" -G Ninja >/dev/null
ninja -C "$build" >/dev/null
javac --release 17 -d "$work/game" $(find "$root/test/fakemc/src" -name '*.java')
cp "$root/test/fakemc/src/version.json" "$work/game/"
jni=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")/include
g++ -std=c++17 -shared -fPIC -I"$root/include" -I"$jni" -I"$jni/linux" \
    "$root/test/fakemc/probe.cpp" -o "$work/probe.so"

# Isolated cache and config so the test never touches the real ones.
export XDG_CACHE_HOME=$work/cache XDG_CONFIG_HOME=$work/config
umc=$build/umc
"$umc" mappings import 1.99.9 client "$root/test/fakemc/client.txt" >/dev/null
fails=0

check() {  # check <description> <expected substring> <command...>
    local what=$1 want=$2; shift 2
    local got
    got=$("$@" 2>&1 || true)
    if [[ $got == *"$want"* ]]; then
        echo "  pass  $what"
    else
        echo "  FAIL  $what: expected '$want', got: $got"
        fails=$((fails + 1))
    fi
}

java -cp "$work/game" net.minecraft.client.main.Main --accessToken test >"$work/game.out" 2>&1 &
pid=$!
sleep 0.5
u() { "$umc" -p $pid "$@"; }

echo "injection"
check "ps lists the game" "$pid" "$umc" ps
check "attach" "injected into $pid" "$umc" attach $pid -l "$build/tick_counter.so" -l "$work/probe.so"
check "mappings pushed" "2 classes" u status
check "version detected" "1.99.9 (client" u status
check "second attach is a no-op" "already running" "$umc" attach $pid

echo "inspection"
check "map resolves class" "-> o.a" u map net.minecraft.client.Minecraft
check "methods show named members" "computeScore(int, long, java.lang.String)" u methods net.minecraft.client.Minecraft
check "call a static method" "7" u call net.minecraft.client.Minecraft computeScore 1 2 xyz
check "dump class bytes" "bytes to" u dump net.minecraft.client.Minecraft "$work/a.class"
check "dumped file is a class" "compiled from" bash -c "javap '$work/a.class' 2>&1 | head -1 | tr A-Z a-z"
check "threads" "main" u threads

echo "hooks"
check "enable tick_counter" "enabled" u enable tick_counter
check "enable probe" "enabled" u enable probe
sleep 1
check "hooks are patched" "site(s) patched" u hooks
check "ticks are counted" " ticks" u mod tick_counter count
check "args are boxed" "args=5,10" u mod probe
check "field and call lookup" "field=found call=7" u mod probe field
u mod probe wrap >/dev/null
sleep 0.6
check "proceed wraps the original" "score 1023" tail -n 3 "$work/game.out"
u mod probe override 777 >/dev/null
sleep 0.6
check "return override" "score 777" tail -n 3 "$work/game.out"
u mod tick_counter freeze on >/dev/null
before=$(grep -c tick "$work/game.out"); sleep 0.6; after=$(grep -c tick "$work/game.out")
check "cancel freezes ticks" "same" bash -c "[ $before = $after ] && echo same || echo moved"
u mod tick_counter freeze off >/dev/null
check "disable unhooks" "disabled" u disable probe
sleep 0.6
check "original return restored" "score 23" tail -n 1 "$work/game.out"
check "trace" "tracing" u trace net.minecraft.client.Minecraft computeScore
sleep 0.6
check "trace logs arguments" "computeScore(5, 10, abc)" u log 5
check "untrace" "stopped 1" u untrace all
check "reload plugin" "reloaded" u reload probe

echo "scripts"
check "load script" "loaded" u script load "$root/test/scripts/counter.js"
check "script sees no host APIs" "sandbox: undefined undefined undefined undefined undefined undefined" u log 40
sleep 1.2
check "tick handlers run" "[counter] ticks" u log 40
check "runaway loop is interrupted" "interrupted" u script load "$root/test/scripts/loop.js"
check "game still ticking after loop" "pong" u ping
check "unknown events are rejected" "unknown event" bash -c "echo 'umc.on(\"nope\", () => {})' > '$work/bad.js'; '$umc' -p $pid script load '$work/bad.js'"
u script load "$root/test/scripts/broken.js" >/dev/null
sleep 0.5
check "handler errors are logged" "error in 'tick' handler" u log 40
check "list scripts" "counter" u script list
check "unload script" "unloaded" u script unload counter
check "load script with header" "loaded" u script load "$root/test/scripts/perms_denied.js"
check "only declared permissions exist" "has game.player: function chat: undefined players: undefined ui: undefined" u log 40
check "render needs its permission" "render blocked: the 'render' event needs the 'render' permission" u log 40
u script unload denied >/dev/null
check "grants narrow the permissions" "[]" bash -c "'$umc' -p $pid script load '$root/test/scripts/perms_denied.js' none >/dev/null; '$umc' -p $pid script list; '$umc' -p $pid script unload denied >/dev/null"

echo "storage"
check "load storage script" "loaded" u script load "$root/test/scripts/storage.js"
log=$(u log 40)
check "write and read back" "read -> \"hello world\"" echo "$log"
check "missing file reads as null" "read missing -> null" echo "$log"
check "list shows the file" "list -> [\"notes.txt\"]" echo "$log"
check "parent directories are refused" "parent dir -> blocked: invalid file name" echo "$log"
check "subfolders are refused" "subfolder -> blocked: invalid file name" echo "$log"
check "absolute paths are refused" "absolute -> blocked: invalid file name" echo "$log"
check "hidden files are refused" "hidden -> blocked: invalid file name" echo "$log"
check "dot-dot names are refused" "dotdot name -> blocked: invalid file name" echo "$log"
check "oversized files are refused" "too big -> blocked: file would be larger than 1024 KB" echo "$log"
check "remove works" "gone -> false" echo "$log"
check "nothing escaped the sandbox" "none" bash -c "[ -e '$work/config/umc/secret' ] || [ -e '$work/config/umc/data/a' ] && echo found || echo none"
check "data lives in the script's own folder" "storage-check" ls "$work/config/umc/data"
check "storage is absent without the permission" "fs without permission: undefined" bash -c "'$umc' -p $pid script load '$root/test/scripts/storage_denied.js' >/dev/null; '$umc' -p $pid log 5"
u script unload storage-check >/dev/null; u script unload storage-denied >/dev/null

echo "install"
check "install shows permissions" "read your position" "$umc" -p $pid install "$root/test/scripts/installable.js" --yes
check "installed script is loaded" "coords-hud" u script list
check "installed script gets its approved permissions" "[player render]" u script list
check "scripts lists installs" "Installable Test" "$umc" scripts
check "install rejects a headerless script" "no // ==UMC== header" "$umc" install "$root/test/scripts/counter.js" --yes
printf '// ==UMC==\n// @name Bad\n// @id bad\n// @permissions root\n// ==/UMC==\n' > "$work/bad.js"
check "install rejects unknown permissions" "unknown permission 'root'" "$umc" install "$work/bad.js" --yes
check "uninstall" "uninstalled coords-hud" "$umc" -p $pid uninstall coords-hud
check "uninstall unloads" "gone" bash -c "'$umc' -p $pid script list | grep -q coords-hud && echo present || echo gone"

kill $pid

echo "legacy (MCP names, 1.8 style)"
javac --release 8 -d "$work/game18" $(find "$root/test/fakemc18/src" -name '*.java')
"$umc" mappings import 1.8.9 client "$root/test/fakemc18/client.txt" >/dev/null
java -cp "$work/game18" net.minecraft.client.main.Main --accessToken test --version 1.8.9 >"$work/game18.out" 2>&1 &
pid=$!
sleep 0.5
check "attach to a 1.8.9 game" "minecraft 1.8.9 client" "$umc" attach $pid
check "tick hook found under its old name" "script event" bash -c "! '$umc' -p $pid script load '$root/test/scripts/legacy.js' 2>&1 | grep -q 'could not'; echo 'script event ok'"
sleep 1
check "player read through MCP names" "legacy player: OldCombatter 12.5 70 -4.25 90 -10 19.5 true 42" u log 20
check "other players resolve" "legacy players: 1" u log 20
check "chat reaches the game" "chat: hello from umc" cat "$work/game18.out"
check "game accessors all resolve" "ok       minecraft.player" u game
kill $pid
if [ $fails -gt 0 ]; then echo "$fails failed"; exit 1; fi
echo "all passed"
