#!/usr/bin/env python3
"""Checks every accessor in src/agent/accessors.txt against Mojang mappings for each version.

usage: check.py <mappings cache dir> <version>...
A label passes for a version when at least one of its candidates exists.
"""
import sys
from collections import defaultdict
from pathlib import Path

from mapdata import PRIMS, java_type, load_mappings, split_desc

root = Path(__file__).resolve().parents[2]


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


def hook_report(cache, versions):
    """For each multi-candidate hook label, show which candidates exist per version."""
    table = parse_table()
    for label in ("hook.chat", "hook.send", "hook.command", "hook.attack"):
        print(label)
        for version in versions:
            classes = load_mappings(cache / "mappings" / version / "client.txt")
            found = [f"{o.split('/')[-1]}.{n}" for (o, k, n, d) in table[label] if exists(classes, o, k, n, d)]
            print(f"  {version:<8} {', '.join(found) or '-'}")


def main():
    if sys.argv[1] == "--hooks":
        hook_report(Path(sys.argv[2]), sys.argv[3:])
        return 0
    cache, versions = Path(sys.argv[1]), sys.argv[2:]
    table = parse_table()
    failures = 0
    width = max(len(v) for v in versions)
    for version in versions:
        classes = load_mappings(cache / "mappings" / version / "client.txt")
        ok = {label: any(exists(classes, *c) for c in cands) for label, cands in table.items()}
        # Before 1.19 commands share the outgoing chat method; newer versions split them.
        if tuple(int(part) for part in version.split(".")) < (1, 19):
            ok["hook.command"] = ok["hook.send"]
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
