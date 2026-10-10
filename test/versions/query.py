#!/usr/bin/env python3
"""Lists the members of a class in one or more versions' mappings, to help write accessors.txt.

usage: query.py <mappings cache dir> <class> <regex> <version>...
example: query.py ~/.cache/umc net.minecraft.client.gui.components.ChatComponent 'add.*Message' 1.20.4 1.21.4
"""
import re
import sys
from pathlib import Path

from mapdata import load_mappings

cache, cls, pattern, versions = Path(sys.argv[1]), sys.argv[2].replace(".", "/"), re.compile(sys.argv[3]), sys.argv[4:]
for version in versions:
    classes = load_mappings(cache / "mappings" / version / "client.txt")
    entry = classes.get(cls)
    if entry is None:
        print(f"{version}: class not found")
        continue
    rows = []
    for name, params, ret in sorted(entry["m"], key=lambda m: (m[0], str(m[1]))):
        if pattern.search(name):
            rows.append(f"{ret} {name}({params})" if ret else f"{name} {params}")
    for name, typ in sorted(entry["f"], key=lambda f: f[0]):
        if pattern.search(name):
            rows.append(f"field {typ or '?'} {name}")
    print(f"{version}:")
    for row in rows:
        print("   " + row)
