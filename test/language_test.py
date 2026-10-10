#!/usr/bin/env python3
"""Compiler and installer tests; no game or network required."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import sys

binary = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/umc').resolve())
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    env = dict(os.environ, XDG_CONFIG_HOME=str(root / 'config'), XDG_CACHE_HOME=str(root / 'cache'), UMC_RUNTIME_DIR=str(root / 'runtime'))
    def run(*args, ok=True):
        result = subprocess.run([binary, *map(str, args)], env=env, text=True, capture_output=True, timeout=10)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result.stdout + result.stderr
    def plugin(name, text='', perms='', deps=''):
        path = root / (name + '.umc')
        path.write_text(f'plugin "{name}"; permissions {perms}; {deps} {text}')
        return path
    def registry(paths, override=None):
        rows = []
        for p in paths:
            m = json.loads(run('check', p, '--json'))
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            assert digest == m['sha256']
            rows.append(f"{m['id']}\t{m['version']}\t{override or digest}\t{p.name}\n")
        path = root / 'index.tsv'
        path.write_text(''.join(rows))
        return path
    valid = plugin('valid', 'state n = 0; fn sum(x) { return x + 1; } command "value" { return sum(n); }')
    run('check', valid)
    for text in ['permissions hooks; hook "net.minecraft.client.Minecraft" "tick" "" {}', 'permissions hooks; hook "net.minecraft.client.Minecraft" "tick" "()" {}', 'on nope {}', 'on render {}', 'fn f(x,x) {}', 'init { let x = 1; let x = 2; }', 'init { eval("x"); }', 'init { java.type("x"); }', 'init { Function("x"); }', 'init { let x = `hi`; }', 'init { let x = () => 1; }', 'init { let __umc_step = 1; }', 'permissions java;', 'hook "java.lang.Runtime" "exec" "()V" {}', 'init { let x = '+('!'*200)+'true; }']:
        bad = plugin('bad', text)
        run('check', bad, ok=False)
    base = plugin('base', 'command "value" { return 42; }')
    child = plugin('child', 'command "value" { return plugins.call("base", "value"); }', deps='depends "base" "1.0.0";')
    index = registry([base, child])
    assert 'child@1.0.0' in run('search', 'child', '--registry', index)
    assert 'base@1.0.0' in run('versions', 'base', '--registry', index)
    assert 'depends: base@1.0.0' in run('info', 'child@1.0.0', '--registry', index)
    assert 'no matching' in run('search', 'absent', '--registry', index)
    run('info', 'absent', '--registry', index, ok=False)
    # Optional display fields let search find names and authors without downloading scripts.
    from urllib.parse import quote
    rows = index.read_text().splitlines()
    index.write_text('\n'.join(row + '\t' + quote('Display Name') + '\t' + quote('Useful test plugin') + '\t' + 'alice' for row in rows) + '\n')
    assert 'Display Name' in run('search', 'alice', '--registry', index)
    run('install', 'child', '--registry', index, '--yes')
    installed = root / 'config/umc/scripts'
    assert (installed / 'base.umc').read_bytes() == base.read_bytes()
    assert (installed / 'child.umc').read_bytes() == child.read_bytes()
    run('uninstall', 'base', ok=False)
    run('uninstall', 'child'); run('uninstall', 'base')
    index = registry([base, child], override='0'*64)
    run('install', 'child@1.0.0', '--registry', index, '--yes', ok=False)
    assert not list(installed.glob('*.umc'))
    privileged = plugin('privileged', perms='storage')
    weak = plugin('weak', deps='depends "privileged" "1.0.0";')
    index = registry([privileged, weak])
    run('install', weak, '--registry', index, '--yes', ok=False)
    assert not list(installed.glob('*.umc'))
    a = plugin('a', deps='depends "b" "1.0.0";')
    b = plugin('b', deps='depends "a" "1.0.0";')
    index = registry([a,b]); run('install', a, '--registry', index, '--yes', ok=False)
    missing = plugin('missing', deps='depends "absent" "1.0.0";')
    run('install', missing, '--registry', index, '--yes', ok=False)
    base2 = root / 'base2.umc'; base2.write_text('plugin "base"; version "2.0.0";')
    other = plugin('other', deps='depends "base" "2.0.0";')
    conflict = plugin('conflict', deps='depends "child" "1.0.0"; depends "other" "1.0.0";')
    index = registry([base,base2,child,other,conflict]); run('install', conflict, '--registry', index, '--yes', ok=False)
    run('install', 'base', '--registry', index, '--yes', ok=False)
    run('info', 'base', '--registry', index, ok=False)
    assert not list(installed.glob('*.umc'))
    index = registry([base, child, base2])
    run('install', 'child@1.0.0', '--registry', index, '--yes')
    run('install', 'base@2.0.0', '--registry', index, '--yes', ok=False)
    assert (installed / 'base.umc').read_bytes() == base.read_bytes()
print('all compiler and installer tests passed')
