"""Reads Mojang/MCP mapping files as UMC writes them. Shared by check.py and query.py."""
import re
from pathlib import Path


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
