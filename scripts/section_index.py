#!/usr/bin/env python3
"""Table of contents / order check for the numbered banners in src/vk.h.

The header says its section banners are parsed; this is the parser. Sections
must be numbered 1..N, strictly increasing, with no gaps or duplicates, and
NEW sections must be appended rather than inserted (so a reader's mental map
and the docs' section references stay valid).

    python3 scripts/section_index.py            # print the index
    python3 scripts/section_index.py --check   # exit 1 on any violation
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

HEADER = pathlib.Path(__file__).resolve().parents[1] / "src" / "vk.h"

# A banner title is " * N. TITLE" with TITLE starting with an uppercase letter.
# Requiring ". " (dot + space) keeps decimal numbers in prose out of the index.
BANNER = re.compile(r"^ \* (\d+)\. ([A-Z][^\n]*)$", re.M)


def sections(path: pathlib.Path = HEADER):
    text = path.read_text()
    out = []
    for m in BANNER.finditer(text):
        line = text.count("\n", 0, m.start()) + 1
        out.append((int(m.group(1)), m.group(2).strip(), line))
    return out


def check(path: pathlib.Path = HEADER) -> list[str]:
    found = sections(path)
    problems = []
    nums = [n for n, _, _ in found]
    if not nums:
        return ["no section banners found"]
    if nums != list(range(1, len(nums) + 1)):
        problems.append("section numbers are not 1..N in order: %s" % nums)
    seen = {}
    for n, title, line in found:
        if n in seen:
            problems.append("duplicate section %d (lines %d and %d)"
                            % (n, seen[n], line))
        seen[n] = line
    # A section heading must be preceded by the banner rule and followed by
    # the closing rule; a stray " * N. Capitalised" line would otherwise pass.
    lines = path.read_text().splitlines()
    for n, title, line in found:
        if line < 2 or not lines[line - 2].lstrip().startswith("/* ="):
            problems.append("section %d at line %d has no opening banner rule" % (n, line))
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="exit 1 on any violation")
    ap.add_argument("--header", type=pathlib.Path, default=HEADER)
    args = ap.parse_args()

    found = sections(args.header)
    if args.check:
        problems = check(args.header)
        for p in problems:
            print("FAIL:", p)
        if problems:
            return 1
        print("OK: %d sections, 1..%d contiguous" % (len(found), len(found)))
        return 0

    print("vk.h section index (%s)" % args.header)
    print("-" * 72)
    for n, title, line in found:
        print("%2d. %-52s line %4d" % (n, title, line))
    print("-" * 72)
    print("%d sections" % len(found))
    problems = check(args.header)
    for p in problems:
        print("FAIL:", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
