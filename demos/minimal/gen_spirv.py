#!/usr/bin/env python3
"""Emit minimal_spirv.h: two hand-assembled SPIR-V modules for demos/minimal.

There is no glslang/slangc in every environment, and this demo exists to be
copy-pasteable, so the two smallest useful modules are assembled here and
committed as a header. Run this only if the shaders need to change:

    python3 demos/minimal/gen_spirv.py

    k_vs_words  gl_VertexIndex -> vec4 gl_Position (a screen-covering triangle)
    k_fs_words  vec4 push constant -> out vec4, location 0

SPIR-V 1.0 requires its sections in a fixed order, and putting a type or a
constant after an OpFunction is invalid:

    capabilities -> memory model -> entry points -> annotations
    -> types and constants and global variables -> functions

so `build()` collects each category separately and `finish()` concatenates
them in that order. Getting this wrong is what a validator catches and a
"looks like SPIR-V" eyeball does not.
"""

import struct
import sys

# ---- opcodes (SPIR-V 1.0) --------------------------------------------------
OP = {
    "OpCapability": 17, "OpMemoryModel": 14, "OpEntryPoint": 15,
    "OpExecutionMode": 16,
    "OpTypeVoid": 19, "OpTypeBool": 20, "OpTypeInt": 21, "OpTypeFloat": 22,
    "OpTypeVector": 23, "OpTypeStruct": 30, "OpTypePointer": 32,
    "OpTypeFunction": 33,
    "OpConstant": 43, "OpConstantComposite": 44,
    "OpFunction": 54, "OpFunctionParameter": 55, "OpFunctionEnd": 56,
    "OpVariable": 59, "OpLoad": 61, "OpStore": 62, "OpAccessChain": 65,
    "OpDecorate": 71, "OpMemberDecorate": 72,
    "OpCompositeConstruct": 80, "OpCompositeExtract": 81,
    "OpSelect": 169, "OpIEqual": 170,
    "OpLabel": 248, "OpReturn": 253,
}

CAP_SHADER = 1
EXEC_VERTEX, EXEC_FRAGMENT = 0, 4
SC_VERTEX, SC_FRAGMENT = 0, 4

DECOR_BLOCK, DECOR_BUILTIN, DECOR_LOCATION = 2, 11, 30
BI_POSITION, BI_VERTEX_INDEX = 0, 42
OFFSET_DECORATION = 35

STORAGE_INPUT, STORAGE_OUTPUT, STORAGE_PUSH = 1, 3, 9
FUNC_NONE = 0
MEMORY_GLSL450, ADDRESSING_LOGICAL = 1, 0

HEADER_MAGIC = 0x07230203
VERSION = 0x00010000
GENERATOR = 0x00080001


def float_bits(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


class Module:
    """Collects instructions by SPIR-V section, then concatenates in order."""

    def __init__(self):
        self.capabilities = []
        self.memory_model = []
        self.entry_points = []
        self.annotations = []
        self.types = []        # types, constants, global variables
        self.functions = []
        self.section = "types"
        self.next_id = 1
        self.names = {}
        self.entry_interface = []

    def id(self, name):
        assert name not in self.names, "duplicate id name %s" % name
        i = self.next_id
        self.next_id += 1
        self.names[name] = i
        return i

    def _emit(self, target, op, *args):
        words = []
        for a in args:
            if isinstance(a, str):
                words.extend(self._string(a))
            elif isinstance(a, (list, tuple)):
                words.extend(a)
            else:
                words.append(a & 0xFFFFFFFF)
        total = 1 + len(words)
        assert total < 0x10000, "%s too long" % op
        target.append((total << 16) | OP[op])
        target.extend(words)

    @staticmethod
    def _string(s):
        data = s.encode("utf-8") + b"\0"
        data += b"\0" * ((4 - len(data) % 4) % 4)
        return list(struct.unpack("<%dI" % (len(data) // 4), data))

    # -- section shortcuts --------------------------------------------------
    def cap(self, op, *a):        self._emit(self.capabilities, op, *a)
    def mem(self, op, *a):        self._emit(self.memory_model, op, *a)
    def entry(self, op, *a):      self._emit(self.entry_points, op, *a)
    def annotate(self, op, *a):   self._emit(self.annotations, op, *a)
    def type_(self, op, *a):      self._emit(self.types, op, *a)
    def func(self, op, *a):       self._emit(self.functions, op, *a)

    def finish(self):
        body = (self.capabilities + self.memory_model + self.entry_points +
                self.annotations + self.types + self.functions)
        return [HEADER_MAGIC, VERSION, GENERATOR, self.next_id, 0] + body


def build_vertex():
    m = Module()
    m.cap("OpCapability", CAP_SHADER)
    m.mem("OpMemoryModel", ADDRESSING_LOGICAL, MEMORY_GLSL450)

    fn = m.id("main")
    var_index = m.id("var_index")
    var_pos = m.id("var_pos")
    m.entry("OpEntryPoint", SC_VERTEX, fn, "main", var_index)
    m.entry("OpExecutionMode", fn, EXEC_VERTEX)

    m.annotate("OpDecorate", var_index, DECOR_BUILTIN, BI_VERTEX_INDEX)
    m.annotate("OpDecorate", var_pos, DECOR_BUILTIN, BI_POSITION)

    # -- types -------------------------------------------------------------
    void = m.id("void")
    f32 = m.id("f32")
    i32 = m.id("i32")
    bool_ = m.id("bool")
    v2 = m.id("v2")
    v4 = m.id("v4")
    ptr_in_i32 = m.id("ptr_in_i32")
    ptr_out_v4 = m.id("ptr_out_v4")
    fn_type = m.id("fn_type")
    m.type_("OpTypeVoid", void)
    m.type_("OpTypeFloat", f32, 32)
    m.type_("OpTypeInt", i32, 32, 1)
    m.type_("OpTypeBool", bool_)
    m.type_("OpTypeVector", v2, f32, 2)
    m.type_("OpTypeVector", v4, f32, 4)
    m.type_("OpTypePointer", ptr_in_i32, STORAGE_INPUT, i32)
    m.type_("OpTypePointer", ptr_out_v4, STORAGE_OUTPUT, v4)
    m.type_("OpTypeFunction", fn_type, void)

    # -- constants (still the types/constants section) ---------------------
    def const_f32(value, name):
        cid = m.id(name)
        m.type_("OpConstant", f32, cid, float_bits(value))
        return cid

    def const_i32(value, name):
        cid = m.id(name)
        m.type_("OpConstant", i32, cid, value)
        return cid

    c_m1 = const_f32(-1.0, "c_m1")
    c_3 = const_f32(3.0, "c_3")
    c_0f = const_f32(0.0, "c_0f")
    c_1f = const_f32(1.0, "c_1f")
    c_0i = const_i32(0, "c_0i")
    c_1i = const_i32(1, "c_1i")

    vec_a = m.id("vec_a")
    vec_b = m.id("vec_b")
    vec_c = m.id("vec_c")
    m.type_("OpConstantComposite", v2, vec_a, c_m1, c_m1)   # (-1,-1)
    m.type_("OpConstantComposite", v2, vec_b, c_3, c_m1)    # ( 3,-1)
    m.type_("OpConstantComposite", v2, vec_c, c_m1, c_3)    # (-1, 3)

    # -- global variables --------------------------------------------------
    m.type_("OpVariable", ptr_in_i32, var_index, STORAGE_INPUT)
    m.type_("OpVariable", ptr_out_v4, var_pos, STORAGE_OUTPUT)

    # -- function ----------------------------------------------------------
    idx = m.id("idx")
    eq0 = m.id("eq0")
    eq1 = m.id("eq1")
    sel0 = m.id("sel0")
    sel1 = m.id("sel1")
    x = m.id("x")
    y = m.id("y")
    pos = m.id("pos")
    label = m.id("label")

    m.func("OpFunction", void, fn, FUNC_NONE, fn_type)
    m.func("OpLabel", label)
    m.func("OpLoad", i32, idx, var_index)
    m.func("OpIEqual", bool_, eq0, idx, c_0i)
    m.func("OpIEqual", bool_, eq1, idx, c_1i)
    # idx == 1 ? B : (idx == 0 ? A : C)
    m.func("OpSelect", v2, sel0, eq0, vec_a, vec_c)
    m.func("OpSelect", v2, sel1, eq1, vec_b, sel0)
    m.func("OpCompositeExtract", f32, x, sel1, 0)
    m.func("OpCompositeExtract", f32, y, sel1, 1)
    m.func("OpCompositeConstruct", v4, pos, x, y, c_0f, c_1f)
    m.func("OpStore", var_pos, pos)
    m.func("OpReturn")
    m.func("OpFunctionEnd")
    return m


def build_fragment():
    m = Module()
    m.cap("OpCapability", CAP_SHADER)
    m.mem("OpMemoryModel", ADDRESSING_LOGICAL, MEMORY_GLSL450)

    fn = m.id("main")
    var_pc = m.id("var_pc")
    var_out = m.id("var_out")
    m.entry("OpEntryPoint", SC_FRAGMENT, fn, "main", var_out)
    m.entry("OpExecutionMode", fn, EXEC_FRAGMENT)

    m.annotate("OpDecorate", var_out, DECOR_LOCATION, 0)

    void = m.id("void")
    f32 = m.id("f32")
    i32 = m.id("i32")
    v4 = m.id("v4")
    block = m.id("block")
    ptr_pc_v4 = m.id("ptr_pc_v4")
    ptr_pc_block = m.id("ptr_pc_block")
    ptr_out_v4 = m.id("ptr_out_v4")
    fn_type = m.id("fn_type")
    m.type_("OpTypeVoid", void)
    m.type_("OpTypeFloat", f32, 32)
    m.type_("OpTypeInt", i32, 32, 1)
    m.type_("OpTypeVector", v4, f32, 4)
    m.type_("OpTypeStruct", block, v4)
    m.type_("OpTypePointer", ptr_pc_v4, STORAGE_PUSH, v4)
    m.type_("OpTypePointer", ptr_out_v4, STORAGE_OUTPUT, v4)
    m.type_("OpTypePointer", ptr_pc_block, STORAGE_PUSH, block)
    m.type_("OpTypeFunction", fn_type, void)

    c_0i = m.id("c_0i")
    m.type_("OpConstant", i32, c_0i, 0)

    m.type_("OpVariable", ptr_out_v4, var_out, STORAGE_OUTPUT)
    m.type_("OpVariable", ptr_pc_block, var_pc, STORAGE_PUSH)

    # The block's single member is at offset 0 (std430-compatible: a vec4 is
    # 16-byte aligned and 16 bytes wide).
    m.annotate("OpDecorate", block, DECOR_BLOCK)
    m.annotate("OpMemberDecorate", block, 0, OFFSET_DECORATION, 0)

    member = m.id("member")
    colour = m.id("colour")
    label = m.id("label")
    m.func("OpFunction", void, fn, FUNC_NONE, fn_type)
    m.func("OpLabel", label)
    m.func("OpAccessChain", ptr_pc_v4, member, var_pc, c_0i)
    m.func("OpLoad", v4, colour, member)
    m.func("OpStore", var_out, colour)
    m.func("OpReturn")
    m.func("OpFunctionEnd")
    return m


# ---- operand signatures ----------------------------------------------------
# Taken from spirv.core.grammar.json (KhronosGroup/SPIRV-Headers), for exactly
# the opcodes these two modules use. A wrong word count is the most likely
# hand-assembly error and it produces a module that is structurally coherent but
# semantically wrong, so each instruction's operand list is checked against the
# spec rather than against the generator's own bookkeeping.
#
#   T IdResultType   R IdResult   r IdRef   l literal   s LiteralString
#   a variadic tail (zero or more)   O optional (zero or one)
SIGNATURES = {
    17: "l",            # OpCapability
    14: "ll",           # OpMemoryModel
    15: "T" + "rs" + "a",  # OpEntryPoint: model, id, name, interface*
    16: "rl" + "a",     # OpExecutionMode: entry point, mode, literals
    19: "R",            # OpTypeVoid
    20: "R",            # OpTypeBool
    21: "Rll",          # OpTypeInt: width, signedness
    22: "Rl" + "O",     # OpTypeFloat: width, optional floating point encoding
    23: "Rrl",          # OpTypeVector: component type, count
    30: "R" + "a",      # OpTypeStruct: member types*
    32: "R" + "lr",     # OpTypePointer: storage class, type
    33: "Rr" + "a",     # OpTypeFunction: return type, params*
    43: "TR" + "a",     # OpConstant: value words
    44: "TR" + "a",     # OpConstantComposite: constituents*
    54: "TRlr",         # OpFunction: control, function type
    56: "",             # OpFunctionEnd
    59: "TRl" + "O",    # OpVariable: storage class, optional initializer
    61: "TRr" + "O",    # OpLoad: pointer, optional memory access
    62: "rr" + "O",     # OpStore: pointer, object, optional memory access
    65: "TRr" + "a",    # OpAccessChain: base, indexes*
    71: "rl" + "a",     # OpDecorate: target, decoration, literals
    72: "rll" + "a",    # OpMemberDecorate: struct, member, decoration, literals
    80: "TR" + "a",     # OpCompositeConstruct
    81: "TRr" + "a",    # OpCompositeExtract
    169: "TRrrr",       # OpSelect: condition, object 1, object 2
    170: "TRrr",        # OpIEqual
    248: "R",           # OpLabel
    253: "",            # OpReturn
}


def check_signature(opcode, operands):
    """Return a list of problems with one instruction's operand list."""
    spec = SIGNATURES.get(opcode)
    if spec is None:
        return ["no signature for opcode %d" % opcode]
    problems = []
    i = 0
    k = 0
    n = len(operands)
    # An instruction whose operand list already failed a basic shape check is
    # diagnosed once, by the caller, not re-walked here.
    if n == 0 and spec not in ("", "a"):
        return ["has no operands"]
    while k < len(spec):
        if spec[k] == " ":
            k += 1
            continue
        if spec[k] == "a":
            return problems          # variadic tail absorbs the rest
        if spec[k] == "O":
            if i < n:
                i += 1
            k += 1
            continue
        # a string consumes words up to and including a null terminator
        if spec[k] == "s":
            start = i
            while i < n:
                terminated = 0 in struct.pack("<I", operands[i])
                i += 1
                if terminated:
                    break
            else:
                problems.append("unterminated string at operand %d" % start)
            k += 1
            continue
        if i >= n:
            problems.append("too few operands: expected %r at %d, have %d"
                            % (spec, i, n))
            return problems
        i += 1
        k += 1
    if i != n:
        problems.append("%d trailing operand(s): expected %d, have %d"
                        % (n - i, i, n))
    return problems


# ---- validation ------------------------------------------------------------
# An independent structural pass. The generator's own bookkeeping is exactly
# what would be wrong if the output were wrong, so this re-derives everything
# from the emitted words.
RESULT_ID_INDEX = {
    # opcode: index of the result id within its operand list (after the word
    # count). Instructions with a result TYPE put the id right after it.
    19: 0, 20: 0, 21: 0, 22: 0, 23: 0, 30: 0, 32: 0, 33: 0,
    43: 1, 44: 1,
    54: 1, 55: 1, 248: 0,
    59: 1, 61: 1, 62: None, 65: 1,
    80: 1, 81: 1, 169: 1, 170: 1,
}
SECTION_OF = {
    17: "cap", 14: "mem", 15: "entry", 16: "entry",
    71: "annot", 72: "annot",
    19: "type", 20: "type", 21: "type", 22: "type", 23: "type",
    30: "type", 32: "type", 33: "type", 43: "type", 44: "type",
    59: "type", 54: "func", 56: "func", 61: "func", 62: "func",
    65: "func", 80: "func", 81: "func", 169: "func", 170: "func",
    248: "func", 253: "func", 55: "func",
}
ORDER = {"cap": 0, "mem": 1, "entry": 2, "annot": 3, "type": 4, "func": 5}


def validate(label, words):
    errors = []
    if words[0] != HEADER_MAGIC:
        errors.append("bad magic 0x%08x" % words[0])
    if words[4] != 0:
        errors.append("reserved schema word is not 0")
    bound = words[3]
    defined = {}
    prev_section = -1
    i = 5
    while i < len(words):
        w = words[i]
        wc, opcode = w >> 16, w & 0xFFFF
        if wc == 0:
            errors.append("zero word count at word %d" % i)
            break
        if i + wc > len(words):
            errors.append("instruction at word %d runs past the end" % i)
            break
        operands = words[i + 1:i + wc]

        section = SECTION_OF.get(opcode)
        if section is None:
            errors.append("unexpected opcode %d at word %d" % (opcode, i))
        else:
            rank = ORDER[section]
            if rank < prev_section:
                errors.append("opcode %d in section %s after a later section"
                              % (opcode, section))
            prev_section = max(prev_section, rank)

        for problem in check_signature(opcode, operands):
            errors.append("opcode %d at word %d: %s" % (opcode, i, problem))

        slot = RESULT_ID_INDEX.get(opcode)
        if slot is not None:
            if slot >= len(operands):
                errors.append("opcode %d truncated" % opcode)
            else:
                rid = operands[slot]
                if not 0 < rid < bound:
                    errors.append("result id %d outside 1..%d" % (rid, bound - 1))
                if rid in defined:
                    errors.append("id %d defined twice (%d then %d)"
                                  % (rid, defined[rid], opcode))
                defined[rid] = opcode
        i += wc

    if i != len(words) and not errors:
        errors.append("trailing partial instruction")

    # Every id the module references must be defined somewhere, except the
    # entry point's interface list, which is checked separately below.
    refs = set()
    i = 5
    while i < len(words):
        wc, opcode = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0 or i + wc > len(words):
            break                   # already reported by the first pass
        ops = words[i + 1:i + wc]
        if opcode == 15 and len(ops) >= 3:
            # OpEntryPoint: ExecutionModel, EntryPoint <id>, Name <string>,
            # Interface <id>*. The id is operand 1; the name starts at 2 and
            # occupies whole words up to and including its null terminator.
            if ops[1] not in defined:
                errors.append("entry point id %d undefined" % ops[1])
            k = 2
            while k < len(ops):
                if 0 in struct.pack("<I", ops[k]):
                    break
                k += 1
            k += 1                      # skip the word holding the terminator
            for iface in ops[k:]:
                refs.add(iface)
        # Fixed operand positions, read defensively: a truncated instruction
        # is already an error, and it must not crash the validator as well.
        def ref(*idx):
            for j in idx:
                if j < len(ops):
                    refs.add(ops[j])

        if opcode == 61:            # OpLoad: type, result, pointer
            ref(0, 2)
        elif opcode == 62:          # OpStore: pointer, object
            ref(0, 1)
        elif opcode == 65:          # OpAccessChain: type, result, base, indexes
            ref(0, 2)
            refs.update(ops[3:])
        elif opcode == 169:         # OpSelect: type, result, condition, a, b
            ref(0, 2, 3, 4)
        elif opcode == 170:         # OpIEqual: type, result, a, b
            ref(0, 2, 3)
        elif opcode == 23:          # OpTypeVector: result, component type, count
            ref(1)
        # OpTypeInt (21) and OpTypeFloat (22) carry a bit width, not an id.
        elif opcode == 30:          # OpTypeStruct: result, member types
            refs.update(ops[1:])
        elif opcode == 32:          # OpTypePointer: result, storage class, type
            ref(1)
        elif opcode == 43:          # OpConstant: type, result, value words
            ref(0)
        elif opcode == 44:          # OpConstantComposite: type, result, members
            ref(0)
            refs.update(ops[2:])
        elif opcode == 54:          # OpFunction: type, result, ctrl, fn type
            ref(0, 3)
        elif opcode == 59:          # OpVariable: type, result, storage class
            ref(0)
        elif opcode == 80:          # OpCompositeConstruct: type, result, parts
            ref(0)
            refs.update(ops[2:])
        elif opcode == 81:          # OpCompositeExtract: type, result, composite
            ref(0, 2)
        i += wc

    for r in sorted(refs):
        if r not in defined:
            errors.append("reference to undefined id %d" % r)

    status = "ok" if not errors else "FAILED"
    instrs = 0
    i = 5
    while i < len(words):
        wc = words[i] >> 16
        if wc == 0:
            break                   # malformed; already reported
        i += wc
        instrs += 1
    print("  %-10s %4d words, %3d instructions, bound %3d -> %s"
          % (label, len(words), instrs, bound, status))
    for e in errors:
        print("      - %s" % e)
    return errors


def emit(name, words):
    out = ["static const uint32_t %s[] = {" % name]
    for i in range(0, len(words), 4):
        out.append("    " + ", ".join("0x%08xu" % w for w in words[i:i + 4]) + ",")
    out.append("};")
    out.append("static const uint32_t %s_count = %uu;" % (name, len(words)))
    return "\n".join(out)


def main():
    vs = build_vertex().finish()
    fs = build_fragment().finish()

    print("validating:")
    errors = validate("vertex", vs) + validate("fragment", fs)
    if errors:
        print("refusing to write a header with invalid SPIR-V")
        return 1

    header = [
        "/* minimal_spirv.h - GENERATED by demos/minimal/gen_spirv.py.",
        " *",
        " * Do not edit by hand: change the script and re-run it.",
        " *",
        " * Two hand-assembled SPIR-V 1.0 modules, committed as words so this",
        " * demo builds with no shader toolchain.",
        " *",
        " *   k_vs_words  gl_VertexIndex -> vec4 gl_Position (screen-covering triangle)",
        " *   k_fs_words  vec4 push constant -> vec4 out at location 0",
        " */",
        "#ifndef MINIMAL_SPIRV_H",
        "#define MINIMAL_SPIRV_H",
        "",
        "#include <stdint.h>",
        "",
        emit("k_vs_words", vs),
        "",
        emit("k_fs_words", fs),
        "",
        "#endif /* MINIMAL_SPIRV_H */",
    ]
    with open("demos/minimal/minimal_spirv.h", "w") as f:
        f.write("\n".join(header) + "\n")
    print("wrote demos/minimal/minimal_spirv.h")
    return 0


if __name__ == "__main__":
    sys.exit(main())
