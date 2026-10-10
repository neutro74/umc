#!/usr/bin/env python3
"""Checks every accessor in src/agent/accessors.txt against Mojang mappings for each version.

usage: check.py <mappings cache dir> <version>...
A label passes for a version when at least one of its candidates exists.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

root = Path(__file__).resolve().parents[2]


def load_mappings(path):
    classes, current = {}, None
    for line in path.read_text().splitlines():
        if not line or line[0] == "#":
            continue
        if not line.startswith(" "):
            current = line.split(" -> ")[0].strip().replace(".", "/")
            classes[current] = {"m": set(), "f": set()}
        elif current and line.strip().startswith("!f "):
            classes[current]["f"].add((line.split(" -> ")[0].strip()[3:], None))  # type unknown
        elif current and line.strip().startswith("!m "):
            desc, name = line.split(" -> ")[0].strip()[3:].split(" ", 1)
            classes[current]["m"].add((name, desc, None))  # named descriptor, kept as written
        elif current:
            lhs = re.sub(r"^\d+:\d+:", "", line.split(" -> ")[0].strip())
            lhs = re.sub(r":\d+:\d+$", "", lhs)
            ret, _, rest = lhs.partition(" ")
            if "(" in rest:
                name, args = rest[:-1].split("(", 1)
                classes[current]["m"].add((name, args, ret))
            else:
                classes[current]["f"].add((rest, ret))
    return classes


PRIMS = {"V": "void", "Z": "boolean", "B": "byte", "C": "char", "S": "short",
         "I": "int", "J": "long", "F": "float", "D": "double"}


def java_type(desc):
    dims = ""
    while desc.startswith("["):
        dims, desc = dims + "[]", desc[1:]
    return (PRIMS[desc] if desc in PRIMS else desc[1:-1].replace("/", ".")) + dims


def split_desc(desc):
    params, i = [], 1
    while desc[i] != ")":
        start = i
        while desc[i] == "[":
            i += 1
        if desc[i] == "L":
            i = desc.index(";", i)
        i += 1
        params.append(java_type(desc[start:i]))
    return ",".join(params), java_type(desc[i + 1:])


def parse_table():
    table = defaultdict(list)
    for line in (root / "src/agent/accessors.txt").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        label, owner, *alts = line.split()
        for alt in alts:
            kind, name, desc = alt.split(":", 2)
            table[label].append((owner.replace(".", "/"), kind, name, desc))
    return table


def exists(classes, owner, kind, name, desc):
    cls = classes.get(owner)
    if cls is None:
        return False
    if kind == "m" and name == "<init>":
        return True  # constructors keep their name, only the class has to be known
    if kind == "m":
        if any(m[0] == name and m[1] == desc and m[2] is None for m in cls["m"]):
            return True  # legacy mapping: matched by full descriptor
        if desc.startswith("(") and any(m[2] is None for m in cls["m"]):
            return False
        params, ret = split_desc(desc)
        if name == "<init>":
            return (name, params, "void") in cls["m"] or ("<init>", params, "void") in cls["m"]
        return (name, params, ret) in cls["m"]
    return (name, java_type(desc)) in cls["f"] or (name, None) in cls["f"]


def main():
    cache, versions = Path(sys.argv[1]), sys.argv[2:]
    table = parse_table()
    failures = 0
    width = max(len(v) for v in versions)
    for version in versions:
        classes = load_mappings(cache / "mappings" / version / "client.txt")
        legacy = any(m[2] is None for c in classes.values() for m in c["m"])
        # Labels tagged modern.* only apply to Mojang-mapped versions, and legacy.* only to MCP ones.
        ok = {label: any(exists(classes, *c) for c in cands) for label, cands in table.items()}
        # Screen size is one capability with two implementations; either one is enough.
        modern = all(ok[l] for l in ("minecraft.window", "window.width", "window.height"))
        old = all(ok[l] for l in ("scaled.new", "scaled.width", "scaled.height"))
        for group in (("minecraft.window", "window.width", "window.height"), ("scaled.new", "scaled.width", "scaled.height")):
            for label in group:
                ok[label] = ok[label] or modern or old
        missing = [label for label, good in ok.items() if not good]
        status = "ok" if not missing else "missing: " + ", ".join(missing)
        print(f"{version:<{width}}  {status}")
        failures += bool(missing)
    return 1 if failures else 0


sys.exit(main())
