#!/usr/bin/env bash
# End-to-end test: inject into a fake obfuscated Minecraft and exercise the agent.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=$root/build
work=$(mktemp -d)
pid=
cleanup() {
    if [[ -n $pid ]]; then kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; fi
    "$build/umc" daemon stop >/dev/null 2>&1 || true
    rm -rf "$work"
}
trap cleanup EXIT

cmake -S "$root" -B "$build" -G Ninja >/dev/null
ninja -C "$build" >/dev/null
javac --release 17 -d "$work/game" $(find "$root/test/fakemc/src" -name '*.java')
cp "$root/test/fakemc/src/version.json" "$work/game/"
# Isolated cache and config so the test never touches the real ones.
export XDG_CACHE_HOME=$work/cache XDG_CONFIG_HOME=$work/config UMC_RUNTIME_DIR=$work/rt XDG_STATE_HOME=$work/state
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
check "attach" "injected into $pid" "$umc" attach $pid
if ! u ping >/dev/null 2>&1; then echo "agent attachment failed; stopping integration test"; exit 1; fi
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

echo "sandboxed plugins"
check "counter loads" "loaded" u script load "$root/examples/scripts/tick-counter.umc"
sleep 0.6
check "tick commands run" "counted" bash -c "'$umc' -p $pid script call tick-counter count | grep -Eq '^[1-9][0-9]*$' && echo counted"
check "method plugin loads" "loaded" u script load "$root/test/scripts/probe.umc"
sleep 0.6
check "original method wraps" "score 1023" tail -n 5 "$work/game.out"
u script call probe mode override >/dev/null
sleep 0.6
check "replacement return" "score 777" tail -n 5 "$work/game.out"
check "hook unload" "unloaded" u script unload probe
sleep 0.6
check "original restored" "score 23" tail -n 5 "$work/game.out"
check "loop bounded" "budget" u script load "$root/test/scripts/loop.umc"
check "game responsive" "pong" u ping
check "native modules removed" "unknown command" u modules
check "install" "installed tick-counter" "$umc" -p $pid install "$root/examples/scripts/tick-counter.umc" --yes
check "installed listing" "Tick Counter" "$umc" scripts
check "uninstall" "uninstalled tick-counter" "$umc" -p $pid uninstall tick-counter
check "trace" "tracing" u trace net.minecraft.client.Minecraft computeScore
sleep 0.6
check "trace arguments" "computeScore(5, 10, abc)" u log 10
check "untrace" "stopped 1" u untrace all

kill $pid

echo "shell"
java -cp "$work/game" net.minecraft.client.main.Main --accessToken test >"$work/shell.out" 2>&1 &
pid=$!
sleep 0.5
"$umc" attach $pid >/dev/null
check "shell session in a pseudo-terminal" "all passed" bash -c "python3 '$root/test/shell_test.py' '$umc' $pid | tee '$work/shell.log' | grep -E 'FAIL' ; ! grep -q FAIL '$work/shell.log' && echo 'all passed'"
sed -n '1,20p' "$work/shell.log"
kill $pid

echo "daemon"
"$umc" mappings import 1.99.9 client "$root/test/fakemc/client.txt" >/dev/null
check "daemon is not running yet" "not running" "$umc" daemon status
check "daemon starts" "daemon started" "$umc" daemon start
check "daemon reports no game" "no Minecraft running" "$umc" daemon status
java -cp "$work/game" net.minecraft.client.main.Main --accessToken test >"$work/auto.out" 2>&1 &
pid=$!
for _ in $(seq 40); do "$umc" -p $pid ping >/dev/null 2>&1 && break; sleep 0.5; done
check "daemon attached to a game nobody told it about" "pong" "$umc" -p $pid ping
check "daemon tracks the game" "$pid  client  1.99.9  attached" "$umc" daemon status
check "daemon log says what happened" "attached $pid" cat "$work/state/umc/daemon.log"
check "install for daemon" "installed tick-counter" "$umc" install "$root/examples/scripts/tick-counter.umc" --yes
kill $pid
for _ in $(seq 20); do "$umc" daemon status | grep -q "no Minecraft running" && break; sleep 0.5; done
check "daemon notices the game exited" "no Minecraft running" "$umc" daemon status
java -cp "$work/game" net.minecraft.client.main.Main --accessToken test >"$work/auto2.out" 2>&1 &
pid=$!
for _ in $(seq 40); do "$umc" -p $pid script list 2>/dev/null | grep -q tick-counter && break; sleep 0.5; done
check "daemon loads installed plugins" "tick-counter" "$umc" -p $pid script list
kill $pid
check "daemon stops" "stopping" "$umc" daemon stop
sleep 1
check "daemon is gone" "not running" "$umc" daemon status

if [ $fails -gt 0 ]; then echo "$fails failed"; exit 1; fi
echo "all passed"
