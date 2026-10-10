#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cmake -S "$root" -B "$root/build" >/dev/null
cmake --build "$root/build" --target umc_script_test --parallel 3 >/dev/null
javac --release 8 -d "$work/classes" "$root"/test/interop/*.java "$root"/test/fakemc18/src/*.java
export XDG_CACHE_HOME=$work/cache XDG_CONFIG_HOME=$work/config XDG_STATE_HOME=$work/state UMC_RUNTIME_DIR=$work/runtime
"$root/build/umc_script_test" "$work/classes" "$root"
