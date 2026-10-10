#!/usr/bin/env bash
# Injects UMC into a real vanilla Minecraft dedicated server and checks mappings, hooks and scripts.
# usage: server_e2e.sh <umc binary> <minecraft version>
set -uo pipefail

umc=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") version=$2
work=$(mktemp -d)
trap 'kill $server 2>/dev/null; sleep 1; rm -rf "$work"' EXIT
export XDG_CACHE_HOME=$work/cache XDG_CONFIG_HOME=$work/config
fails=0

check() {  # check <description> <expected substring> <command...>
    local what=$1 want=$2; shift 2
    local got
    got=$("$@" 2>&1 || true)
    if [[ $got == *"$want"* ]]; then echo "  pass  $what"; else echo "  FAIL  $what: expected '$want', got: $got"; fails=$((fails + 1)); fi
}

manifest=$(curl -fsSL https://piston-meta.mojang.com/mc/game/version_manifest_v2.json)
PY=$(command -v python3 || command -v python)
meta_url=$(printf '%s' "$manifest" | "$PY" -c "import json,sys; print(next(v['url'] for v in json.load(sys.stdin)['versions'] if v['id']=='$version'))")
jar_url=$(curl -fsSL "$meta_url" | "$PY" -c "import json,sys; print(json.load(sys.stdin)['downloads']['server']['url'])")
mkdir -p "$work/server" && cd "$work/server"
curl -fsSL -o server.jar "$jar_url"
echo "eula=true" > eula.txt
printf 'online-mode=false\nlevel-type=flat\nspawn-protection=0\nview-distance=4\n' > server.properties

java -Xmx1G -jar server.jar nogui > "$work/server.log" 2>&1 &
server=$!
for _ in $(seq 120); do grep -q "Done (" "$work/server.log" && break; sleep 1; done
grep -q "Done (" "$work/server.log" || { echo "server never started"; tail -20 "$work/server.log"; exit 1; }

echo "server $version"
check "ps finds the server" "$server" "$umc" ps -a
out=$("$umc" attach "$server" 2>&1); echo "$out" | sed 's/^/        /'
check "attach" "injected into" echo "$out"
check "server side detected" "server" "$umc" -p "$server" status
check "version detected" "minecraft $version" "$umc" -p "$server" status
check "classes are named" "net.minecraft.server.MinecraftServer" "$umc" -p "$server" classes MinecraftServer
"$umc" -p "$server" trace net.minecraft.server.MinecraftServer tickServer >/dev/null
sleep 3
check "tick hook patches the real class" "site(s) patched" "$umc" -p "$server" hooks
check "traced calls arrive" "trace net.minecraft.server.MinecraftServer.tickServer" "$umc" -p "$server" log 20
check "untrace" "stopped" "$umc" -p "$server" untrace all

cat > "$work/ticks.js" <<'JS'
// ==UMC==
// @name Ticks
// @id ticks
// ==/UMC==
let n = 0;
umc.on("tick", () => { if (++n === 40) umc.log("server ticks:", n); });
JS
check "script loads" "loaded" "$umc" -p "$server" script load "$work/ticks.js"
sleep 4
check "script ticks with the server" "server ticks: 40" "$umc" -p "$server" log 30
check "server still responsive" "pong" "$umc" -p "$server" ping

kill $server 2>/dev/null
if [ $fails -gt 0 ]; then echo "$fails failed"; exit 1; fi
echo "all passed"
