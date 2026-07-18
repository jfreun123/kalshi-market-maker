#!/usr/bin/env python3
"""Layer-include lint: enforce the source/ dependency DAG at header level.

Usage: python3 scripts/check_layer_includes.py [source_dir]

Every project include must be layer-prefixed (#include "engine/orderbook.hpp")
and may only point at a layer the including file's layer is allowed to depend
on (mirrors the target_link_libraries edges in source/CMakeLists.txt). Link
time catches upward calls from .cpp files; this catches header-only
violations, which is how the config.hpp inversion originally crept in.
Exits non-zero listing every violation.
"""

import re
import sys
from pathlib import Path

ALLOWED = {
    "core": {"core"},
    "net": {"net", "core"},
    "engine": {"engine", "core"},
    "exchange": {"exchange", "core", "net", "engine"},
    "strategy": {"strategy", "core", "engine"},
    "tooling": {"tooling", "engine", "core"},
    "app": {"app", "core", "net", "engine", "exchange", "strategy", "tooling"},
}

INCLUDE_PATTERN = re.compile(r'^\s*#include\s+"([^"]+)"', re.MULTILINE)


def main():
    source_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "source")
    violations = []
    for path in sorted(source_dir.rglob("*")):
        if path.suffix not in (".hpp", ".cpp"):
            continue
        relative = path.relative_to(source_dir)
        if len(relative.parts) < 2:
            violations.append(f"{path}: file is not inside a layer directory")
            continue
        layer = relative.parts[0]
        if layer not in ALLOWED:
            violations.append(f"{path}: unknown layer '{layer}'")
            continue
        for target in INCLUDE_PATTERN.findall(path.read_text()):
            parts = target.split("/")
            if len(parts) == 1:
                violations.append(
                    f"{path}: unprefixed project include \"{target}\" — "
                    f"use \"<layer>/{target}\"")
                continue
            target_layer = parts[0]
            if target_layer not in ALLOWED:
                violations.append(
                    f"{path}: include \"{target}\" names unknown layer "
                    f"'{target_layer}'")
            elif target_layer not in ALLOWED[layer]:
                violations.append(
                    f"{path}: layer '{layer}' may not include "
                    f"'{target_layer}' (\"{target}\")")
    if violations:
        print(f"{len(violations)} layer-include violation(s):")
        for violation in violations:
            print(f"  {violation}")
        return 1
    print("layer includes clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
