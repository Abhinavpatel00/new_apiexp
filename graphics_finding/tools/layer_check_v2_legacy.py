#!/usr/bin/env python3
"""Layering / naming / cross-file constant contract for the v2 API.

The header's opening comment says scripts/layer_check.py enforces call-graph
rules. This is that check, expressed as three invariants that can be verified
without a C parser:

  1. NO SLOW VERBS. The public declaration list contains none of the Vulkan
     operations the v2 ban list removes. Comments may discuss them; a declared
     function may not. (vk_scan.py does the same for vk.c; this covers the
     header surface itself.)
  2. NAMING (AGENTS.md §12.1): types PascalCase, functions snake_case.
  3. CONSTANT SYNC. The hardware numbers in src/vk_v2.h §1/§16 must equal
     their shader-side copies in shaders/mu/dispatch.slang and
     shaders/mu/format.slang, because the CPU frame program budgets a pass
     with exactly the numbers the shader was compiled against.

    python3 scripts/layer_check.py
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "vk_v2.h"
DISPATCH = ROOT / "shaders" / "mu" / "dispatch.slang"
FORMAT = ROOT / "shaders" / "mu" / "format.slang"
EXPORTS = ROOT / "scripts" / "vk_v2_exports.txt"
COMMON = ROOT / "shaders" / "mu" / "common.slang"

# Strip C comments AND preprocessor lines, so prose and macros never look
# like declarations. Macro continuations are joined first so a multi-line
# #define is removed as one unit.
def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    src = re.sub(r"\\\n", " ", src)
    src = re.sub(r"^#.*$", " ", src, flags=re.M)
    return src


# Public declarations are the file-scope statements that end in ";". Track
# brace depth so bodies of static inline helpers are never mistaken for
# declarations, and join continuation lines before matching.
DECL = re.compile(
    r"^(?:[A-Za-z_][A-Za-z0-9_]*\s+){1,3}\**\s*([a-z_][A-Za-z0-9_]*)\s*\(",
)

def declarations(code: str):
    depth = 0
    pending = ""
    for raw in code.splitlines():
        line = raw.strip()
        if not line:
            continue
        if depth == 0:
            # A "{" before the first ";" starts an inline function body, not
            # a declaration; do not fold its return statement into pending.
            if "{" in line and ";" not in (pending + line.split("{", 1)[0]):
                pending = ""
            else:
                pending = (pending + " " + line).strip()
                if ";" not in pending:
                    continue
                stmt = pending
                pending = ""
                m = DECL.match(stmt)
                if m and stmt.rstrip().endswith(";"):
                    yield m.group(1)
        depth += line.count("{") - line.count("}")
        if depth < 0:
            depth = 0

# The ban list, expressed as the v2 verbs that must not exist (the Vulkan
# spellings are what vk_scan.py looks for in vk.c).
BANNED_FUNCS = {
    "wait_idle", "queue_wait_idle", "device_wait_idle",
    "queue_submit", "frame_submit", "queue_present",
    "cmd_pipeline_barrier", "pipeline_barrier", "flush_barriers",
    "clear_color_image", "clear_depth_image", "clear_image",
    "create_graphics_pipeline", "create_compute_pipeline",
    "update_descriptor_sets", "write_descriptor", "bind_descriptor_sets",
    "allocate_descriptor_sets", "create_descriptor_pool",
    "queue_bind_sparse", "wait_for_fences", "create_fence",
    "map_memory", "unmap_memory", "flush_mapped_memory_ranges",
}

# Substrings that must never appear in a declared function name.
BANNED_SUBSTRINGS = ("wait_idle", "barrier", "descriptor_write", "pipeline_create")

SNAKE = re.compile(r"^[a-z][a-z0-9_]*$")
PASCAL = re.compile(r"^[A-Z][A-Za-z0-9]*$")

# (C macro, Slang macro, file) pairs that must agree numerically.
CONSTANT_PAIRS = [
    ("VGPR_FULL_OCC", "MU_VGPR_FULL_OCC", DISPATCH),
    ("WAVES_PER_CU", "MU_WAVES_PER_CU", DISPATCH),
    ("LDS_BYTES_PER_CU", "MU_LDS_BYTES_PER_CU", DISPATCH),
    ("ICACHE_BYTES_WGP", "MU_ICACHE_BYTES_WGP", DISPATCH),
    ("SECTOR_BYTES", "MU_SECTOR_BYTES", DISPATCH),
    ("L2_LINE_BYTES", "MU_L2_LINE_BYTES", DISPATCH),
    ("WG_64", "MU_WG_64", DISPATCH),
    ("WG_128", "MU_WG_128", DISPATCH),
    ("WG_256", "MU_WG_256", DISPATCH),
    ("WG_1024", "MU_WG_1024", DISPATCH),
    ("FILTER_TILE_X", "MU_FILTER_TILE_X", DISPATCH),
    ("FILTER_TILE_Y", "MU_FILTER_TILE_Y", DISPATCH),
]

# (C format case, Slang MU_BPP_* macro). Checked through format_bytes().
BPP_PAIRS = [
    ("FMT_R8_UNORM", "MU_BPP_R8_UNORM"),
    ("FMT_RG8_UNORM", "MU_BPP_RG8_UNORM"),
    ("FMT_RGBA8_UNORM", "MU_BPP_RGBA8_UNORM"),
    ("FMT_RGBA8_SRGB", "MU_BPP_RGBA8_SRGB"),
    ("FMT_RGBA8_SNORM", "MU_BPP_RGBA8_SNORM"),
    ("FMT_RGB10A2_UNORM", "MU_BPP_RGB10A2_UNORM"),
    ("FMT_R11G11B10F", "MU_BPP_R11G11B10F"),
    ("FMT_RG16F", "MU_BPP_RG16F"),
    ("FMT_RGBA16F", "MU_BPP_RGBA16F"),
    ("FMT_RGBA32F", "MU_BPP_RGBA32F"),
    ("FMT_D16_UNORM", "MU_BPP_D16_UNORM"),
    ("FMT_D24S8", "MU_BPP_D24S8"),
    ("FMT_D32F", "MU_BPP_D32F"),
]


def macro_value(src: str, name: str):
    clean = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    clean = re.sub(r"//[^\n]*", " ", clean)
    m = re.search(r"^#define\s+%s\s+(.+?)\s*$" % re.escape(name), clean, re.M)
    if not m:
        return None
    v = m.group(1).strip()
    v = v.strip("()").strip()
    v = re.sub(r"[fuUlL]+$", "", v).strip()
    try:
        return int(v, 0)
    except ValueError:
        return v


def format_byte_cases(header: str) -> dict:
    """Pull the numeric returns out of format_bytes()'s switch."""
    body = re.search(r"static inline uint32_t format_bytes\(Format f\) \{(.*?)\n\}",
                     header, re.S)
    out = {}
    if not body:
        return out
    current = None
    for line in body.group(1).splitlines():
        if "default:" in line:
            current = None
        if "case " in line:
            current = None
            for m in re.finditer(r"case\s+(FMT_\w+)\s*:", line):
                out.setdefault(m.group(1), None)
                current = m.group(1)
        m = re.search(r"return\s+(\d+)u\s*;", line)
        if m and current:
            out[current] = int(m.group(1))
            for k, v in list(out.items()):
                if v is None:
                    out[k] = int(m.group(1))
    return out


def main() -> int:
    problems = []
    h = HEADER.read_text()
    code = strip_comments(h)

    funcs = sorted(set(declarations(code)))
    if not funcs:
        problems.append("no function declarations found — declaration regex broke")

    # 1. no slow verbs
    for f in funcs:
        if f in BANNED_FUNCS:
            problems.append("banned slow verb declared: %s" % f)
        for bad in BANNED_SUBSTRINGS:
            if bad in f:
                problems.append("declared function contains banned '%s': %s" % (bad, f))

    # 2. naming
    for f in funcs:
        if not SNAKE.match(f):
            problems.append("function not snake_case (AGENTS.md §12.1): %s" % f)
    # A macro name such as ALIGNAS( is not a type name.
    types = re.findall(r"typedef\s+(?:enum|struct)\s+(\w+)(?!\()", code)
    types += re.findall(r"typedef\s+\w+\s+(\w+)\s*;", code)
    for t in set(types):
        if not PASCAL.match(t):
            problems.append("type not PascalCase (AGENTS.md §12.1): %s" % t)

    # 3a. numeric constant sync
    d = DISPATCH.read_text()
    for c, s, path in CONSTANT_PAIRS:
        a, b = macro_value(h, c), macro_value(path.read_text(), s)
        if a is None or b is None:
            problems.append("missing constant: %s / %s" % (c, s))
        elif a != b:
            problems.append("constant drift: %s=%s but %s=%s" % (c, a, s, b))

    # 3b. format byte-size sync
    fmt = FORMAT.read_text()
    cases = format_byte_cases(code)
    for fmt_name, bpp_macro in BPP_PAIRS:
        a, b = cases.get(fmt_name), macro_value(fmt, bpp_macro)
        if a is None or b is None:
            problems.append("missing format byte size: %s / %s" % (fmt_name, bpp_macro))
        elif a != b:
            problems.append("bpp drift: %s=%s but %s=%s" % (fmt_name, a, bpp_macro, b))

    # 3c. the viewport convention actually agrees with the shader
    cx, cy = macro_value(h, "NDC_TO_UV_X"), macro_value(h, "NDC_TO_UV_Y")
    common = COMMON.read_text()
    m = re.search(r"#define\s+MU_NDC_TO_UV\s+float2\(\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\)", common)
    if not m or cx is None or cy is None:
        problems.append("viewport convention macro missing")
    elif abs(float(m.group(1)) - float(cx)) > 1e-6 or abs(float(m.group(2)) - float(cy)) > 1e-6:
        problems.append("viewport drift: header (%s,%s) vs MU_NDC_TO_UV (%s,%s)"
                        % (cx, cy, m.group(1), m.group(2)))

    # 3d. implementation completeness: every public declaration must be
    # defined by src/vk_v2.c (the manifest is emitted by build.sh from nm).
    if EXPORTS.exists():
        exported = set(EXPORTS.read_text().split())
        # These two exist only when the backend is built with MU_ESCAPE.
        escape_only = {"gpu_raw_device", "pass_raw_cmd"}
        missing_impl = [f for f in funcs
                        if f not in exported and f not in escape_only]
        for f in missing_impl:
            problems.append("declared but not implemented: %s" % f)
        impl_note = "%d exports" % len(exported)
    else:
        impl_note = "no export manifest (run ./build.sh)"

    # 4. section order
    sys.path.insert(0, str(ROOT / "scripts"))
    import section_index  # noqa: E402
    for p in section_index.check(HEADER):
        problems.append("section index: %s" % p)

    for p in problems:
        print("FAIL:", p)
    if problems:
        print("\n%d problem(s)" % len(problems))
        return 1
    print("OK: %d public functions (%s), %d types, %d constants, %d bpp values, "
          "%d sections in sync"
          % (len(funcs), impl_note, len(set(types)), len(CONSTANT_PAIRS),
             len(BPP_PAIRS), len(section_index.sections(HEADER))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
