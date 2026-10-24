#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

# roundtrip.py ALLOCATED DISASM: checks that the bytes the compiler emitted
# decode back to the program it allocated.

# ALLOCATED is `jelloc --jlo` (the allocated program, printed by the compiler
# before encoding); DISASM is `jlo_tool disasm` of the emitted module (decoded
# by the VM's loader, after validation). The opcode table below is written from
# the IR's meaning and BYTECODE.md, not from Emit's table, so a wrong opcode,
# operand slot, pool index, type entry or jump offset shows up as a mismatch.

# Control flow is compared by walking both graphs from the entry: each block
# must sit at one pc, each jump and branch must land on its target's pc, each
# try region change must be the ENDTRY and TRY pairs it implies, and every pc
# must belong to exactly one block. Layout is free; what Emit may change is
# the IR's own equivalences:
#
#   - a jump to an empty block that only jumps on (not a catch block) goes to
#     where that block goes; a branch does so only when that is in its own
#     try region;
#   - a jump to an empty block that returns is that return, whatever regions
#     the jump leaves or enters: a frame's try scopes end with it;
#   - a branch whose two targets go to one place is a jump there;
#   - a jump or branch to the block laid out next is left out.
#
# Prints the mismatches and exits 1.

import re, struct, sys

problems = []


def problem(where, message):
    problems.append("%s: %s" % (where, message))


def parenthesized(text):
    """TEXT up to the parenthesis closing one already open, and the rest after it."""
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            if depth == 0:
                return text[:i], text[i + 1:]
            depth -= 1
    return text, ""


def split_top(text):
    """Splits TEXT at top-level commas."""
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "([{<":
            depth += 1
        elif ch in ")]}" or (ch == ">" and text[i - 1] != "-"):
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
    if text[start:].strip():
        parts.append(text[start:].strip())
    return parts


def bracketed(line, key):
    return split_top(line[len(key) + 1:].strip()[1:-1])


# --- The decoded module ---------------------------------------------------

def parse_disasm(path):
    module = {"types": [], "sigs": [], "structs": [], "globals": [], "bytes": [], "i64": [], "f64": [], "imports": [], "funcs": []}
    func = None
    for line in open(path, encoding="latin-1"):
        line = line.rstrip("\n")
        if line.startswith("types:"):
            module["types"] = bracketed(line, "types")
        elif line.startswith("sigs:"):
            for sig in bracketed(line, "sigs"):
                m = re.match(r"\{ ret=(\S+), nargs=\d+, args=\[(.*)\] \}$", sig)
                module["sigs"].append((m.group(1), split_top(m.group(2))))
        elif line.startswith("struct_sigs:"):
            for sig in bracketed(line, "struct_sigs"):
                m = re.match(r"\{ nfields=\d+ named=0 mutable=\d fields=\[(.*)\] \}$", sig)
                module["structs"].append(split_top(m.group(1)))
        elif line.startswith("globals:"):
            module["globals"] = bracketed(line, "globals")
        elif line.startswith("const_i64:"):
            module["i64"] = bracketed(line, "const_i64")
        elif line.startswith("const_f64:"):
            module["f64"] = bracketed(line, "const_f64")
        elif line.startswith("imports:"):
            module["imports"] = bracketed(line, "imports")
        elif line.startswith("const_bytes:"):
            module["bytes"] = re.findall(r'"((?:[^"\\]|\\.)*)"', line[len("const_bytes:"):])
        elif line.startswith("func["):
            m = re.match(r"func\[\d+\](?: name=\S+)? nregs=(\d+)(?: cap_start=(\d+))?(?: nargs=(\d+))? reg_types=\[(.*)\]$", line)
            nregs = int(m.group(1))
            func = {"nregs": nregs, "cap_start": int(m.group(2)) if m.group(2) else nregs,
                    "nargs": int(m.group(3)) if m.group(3) else nregs, "regs": split_top(m.group(4)), "code": []}
            module["funcs"].append(func)
        elif re.match(r"^ +\d+: ", line):
            m = re.match(r"^ +(\d+): (\w+) a=r(\d+) b=r(\d+) c=r(\d+) imm=(-?\d+)$", line)
            if not m or int(m.group(1)) != len(func["code"]):
                sys.exit("roundtrip.py: unreadable instruction: " + line)
            func["code"].append((m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))))
    return module


def type_index(module, ref):
    """The type table index a reference names: KIND#N is entry N, a bare
    KIND the first entry of that kind."""
    if "#" in ref:
        return int(ref.split("#")[1])
    for i, entry in enumerate(module["types"]):
        if entry.split("(")[0] == ref:
            return i
    return -1


# --- The allocated program ------------------------------------------------

def parse_ir(path):
    program = {"globals": [], "natives": [], "wides": [], "floats": [], "funcs": []}
    func = block = None
    for line in open(path, encoding="latin-1"):
        line = line.rstrip("\n")
        m = re.match(r"global g\d+: (.*)$", line)
        if m:
            program["globals"].append(m.group(1))
            continue
        m = re.match(r"native n\d+: (\S+) fn\((.*)\) -> (.*)$", line)
        if m:
            program["natives"].append((m.group(1), split_top(m.group(2)), m.group(3)))
            continue
        m = re.match(r"i64 #\d+ = (.*)$", line)
        if m:
            program["wides"].append(m.group(1))
            continue
        # The disassembly shows an F64 as C's %g does.
        m = re.match(r"f64 #\d+ = bits (-?\d+)$", line)
        if m:
            program["floats"].append("%g" % struct.unpack("<d", struct.pack("<q", int(m.group(1))))[0])
            continue
        m = re.match(r"fn @(\d+)\((.*) entry (\d+) \{$", line)
        if m:
            params, rest = parenthesized(m.group(2))
            captures = parenthesized(rest[len(" captures("):])[0] if rest.startswith(" captures(") else ""
            func = {"id": int(m.group(1)), "nparams": len(split_top(params)), "ncaps": len(split_top(captures)),
                    "regs": [], "entry": int(m.group(3)), "blocks": {}, "order": []}
            program["funcs"].append(func)
            continue
        m = re.match(r"  register %\d+: (.*)$", line)
        if m:
            func["regs"].append(m.group(1))
            continue
        m = re.match(r"  block (\d+)(?: catch block (\d+) as %(\d+))?:$", line)
        if m:
            handler = (int(m.group(2)), int(m.group(3))) if m.group(2) else None
            block = {"id": int(m.group(1)), "insns": [], "term": None, "handler": handler}
            func["blocks"][block["id"]] = block
            func["order"].append(block["id"])
            continue
        if line.startswith("    "):
            text = line.strip()
            term = parse_terminator(text)
            if term:
                block["term"] = term
            else:
                block["insns"].append(text)
    return program


def parse_terminator(text):
    m = re.match(r"jump block (\d+)$", text)
    if m:
        return ("jump", int(m.group(1)))
    m = re.match(r"branch %(\d+), block (\d+), block (\d+)$", text)
    if m:
        return ("branch", int(m.group(1)), int(m.group(2)), int(m.group(3)))
    m = re.match(r"return %(\d+)$", text)
    if m:
        return ("return", int(m.group(1)))
    m = re.match(r"throw %(\d+)$", text)
    if m:
        return ("throw", int(m.group(1)))
    return None


# --- Types ----------------------------------------------------------------

SCALARS = {"i32": "I32", "bool": "BOOL", "i64": "I64", "f16": "F16", "f32": "F32", "f64": "F64", "atom": "ATOM", "bytes": "BYTES", "dynamic": "DYNAMIC"}
# A native crosses only scalars, Bytes and Dynamic (BYTECODE.md, CALL_NATIVE).
NATIVE_KINDS = SCALARS


def same_type(module, ir, ref):
    """Whether IR type text IR is the module type REF names."""
    index = type_index(module, ref)
    if index < 0 or index >= len(module["types"]):
        return False
    entry = module["types"][index]
    if ir in SCALARS:
        return entry == SCALARS[ir]
    if ir == "object":
        return entry == "OBJECT"
    # A nominal type, an enum or a record, is the type of its own index.
    m = re.match(r"type@(\d+)$", ir)
    if m:
        return index == int(m.group(1)) and entry.split("(")[0] in ("ENUM", "STRUCT")
    m = re.match(r"(array|list)<(.*)>$", ir)
    if m:
        inner = re.match(m.group(1).upper() + r"\((.*)\)$", entry)
        return bool(inner) and same_type(module, m.group(2), inner.group(1))
    if ir.startswith("("):
        inner = re.match(r"STRUCT\((\d+)\)$", entry)
        if not inner:
            return False
        fields, wanted = module["structs"][int(inner.group(1))], split_top(ir[1:-1])
        return len(fields) == len(wanted) and all(same_type(module, w, f) for w, f in zip(wanted, fields))
    if ir.startswith("fn("):
        params, result = parenthesized(ir[3:])
        inner = re.match(r"FUNCTION\((\d+)\)$", entry)
        if not inner or not result.startswith(" -> "):
            return False
        ret, args = module["sigs"][int(inner.group(1))]
        wanted = split_top(params)
        return (len(args) == len(wanted) and all(same_type(module, w, a) for w, a in zip(wanted, args))
                and same_type(module, result[len(" -> "):], ret))
    return False


# --- Instructions ---------------------------------------------------------

def regs(text):
    return [int(r) for r in re.findall(r"%(\d+)", text)]


def window(args):
    """The first register of consecutive ARGS, or None when they are not."""
    if any(b != a + 1 for a, b in zip(args, args[1:])):
        return None
    return args[0] if args else "any"


def expect(text, dst, func, module, indexes):
    """The instruction IR TEXT must encode to, as (op, a, b, c, imm) with None
    for an operand left unchecked, or a reason it cannot."""
    types = func["regs"]
    width = lambda r: types[r]
    m = re.match(r"(add|sub|mul)\.wrap\.i32 %(\d+), (-?\d+)$", text)
    if m:
        return (m.group(1).upper() + "_I32_IMM", dst, int(m.group(2)), 0, int(m.group(3)))
    m = re.match(r"(eq|lt\.s)\.i32 %(\d+), (-?\d+)$", text)
    if m:
        return (m.group(1).split(".")[0].upper() + "_I32_IMM", dst, int(m.group(2)), 0, int(m.group(3)))
    m = re.match(r"(div|rem|lt)\.u\.(i32|i64) (.*)$", text)
    if m:
        args = regs(m.group(3))
        name = {"rem": "MOD"}.get(m.group(1), m.group(1).upper()) + ("_U64" if m.group(2) == "i64" else "_U32")
        return (name, dst, args[0], args[1], 0)
    m = re.match(r"(add|sub|mul|div|rem|shl|shr|bitand|bitor|bitxor|neg|eq|lt)\.(?:wrap\.|s\.)?(i32|i64) (.*)$", text)
    if m:
        op, ty, args = m.group(1), m.group(2), regs(m.group(3))
        if ty != width(args[0]):
            return "the operand is %s, not %s" % (width(args[0]), ty)
        name = {"rem": "MOD"}.get(op, op.upper()) + ("_I64" if ty == "i64" else "_I32")
        return (name, dst, args[0], args[1] if len(args) > 1 else 0, 0)
    m = re.match(r"(add|sub|mul|div|neg|eq|lt)\.(f16|f32|f64) (.*)$", text)
    if m:
        op, ty, args = m.group(1), m.group(2), regs(m.group(3))
        if ty != width(args[0]):
            return "the operand is %s, not %s" % (width(args[0]), ty)
        return (op.upper() + "_" + ty.upper(), dst, args[0], args[1] if len(args) > 1 else 0, 0)
    m = re.match(r"convert\.(\w+)\.(\w+) %(\d+)$", text)
    if m:
        return (m.group(2).upper() + "_FROM_" + m.group(1).upper(), dst, int(m.group(3)), 0, 0)
    simple = {
        "not.bool": "NOT_BOOL", "sext.i32.i64": "SEXT_I64", "zext.i32.i64": "ZEXT_I64", "trunc.i64.i32": "I32_FROM_I64", "box": "TO_DYN",
        "kind_of": "KINDOF", "is_null": "IS_NULL", "array.new": "ARRAY_NEW", "array.len": "ARRAY_LEN",
        "array.get": "ARRAY_GET", "array.concat": "ARRAY_CONCAT2", "bytes.new": "BYTES_NEW", "bytes.len": "BYTES_LEN", "bytes.get_u8": "BYTES_GET_U8",
        "bytes.concat": "BYTES_CONCAT2", "bytes.join": "BYTES_JOIN", "bytes.eq": "BYTES_EQ", "enum.tag": "ENUM_TAG", "move": "MOV",
        "list.nil": "LIST_NIL", "list.cons": "LIST_CONS", "list.head": "LIST_HEAD", "list.tail": "LIST_TAIL",
        "list.is_nil": "LIST_IS_NIL", "eq.atom": "PHYSEQ", "eq.fn": "PHYSEQ", "eq.object": "PHYSEQ", "eq.dynamic": "PHYSEQ",
        "object.remove": "OBJ_REMOVE", "object.copy": "OBJ_COPY", "object.keys": "OBJ_KEYS",
    }
    head, _, rest = text.partition(" ")
    if head in simple:
        args = regs(rest) + [0, 0]
        return (simple[head], dst, args[0], args[1], 0)
    if head == "unbox":
        name = {"i32": "FROM_DYN_I32", "i64": "FROM_DYN_I64", "bool": "FROM_DYN_BOOL", "atom": "FROM_DYN_ATOM",
                "f16": "FROM_DYN_F16", "f32": "FROM_DYN_F32", "f64": "FROM_DYN_F64"}.get(types[dst], "FROM_DYN_PTR")
        return (name, dst, regs(rest)[0], 0, 0)
    m = re.match(r"bool (true|false)$", text)
    if m:
        return ("CONST_BOOL", dst, 0, 0, 1 if m.group(1) == "true" else 0)
    m = re.match(r"i32 (-?\d+)$", text)
    if m:
        return ("CONST_I32", dst, 0, 0, int(m.group(1)))
    m = re.match(r"i64 #(\d+)$", text)
    if m:
        return ("CONST_I64", dst, 0, 0, int(m.group(1)))
    m = re.match(r"f64 #(\d+)$", text)
    if m:
        return ("CONST_F64", dst, 0, 0, int(m.group(1)))
    m = re.match(r"(f16|f32) bits (-?\d+)$", text)
    if m:
        return ("CONST_" + m.group(1).upper(), dst, 0, 0, int(m.group(2)))
    # A program's atoms come first in the module's atom table.
    m = re.match(r"atom #(\d+)$", text)
    if m:
        return ("CONST_ATOM", dst, 0, 0, int(m.group(1)))
    m = re.match(r"bytes (\".*\")$", text)
    if m:
        literal = m.group(1)[1:-1]
        return ("CONST_BYTES", dst, 0, 0, ("pool", literal))
    if text == "null":
        return ("CONST_NULL", dst, 0, 0, 0)
    if text == "struct.new":
        return ("STRUCT_NEW", dst, 0, 0, 0)
    if text == "object.new":
        return ("OBJ_NEW", dst, 0, 0, 0)
    # A named key is an atom in imm; any other is an Atom register.
    m = re.match(r"object\.(get|has) %(\d+)(?:, %(\d+)|, #(\d+))$", text)
    if m:
        op = "OBJ_" + m.group(1).upper()
        if m.group(4) is not None:
            return (op + "_ATOM", dst, int(m.group(2)), 0, int(m.group(4)))
        return (op, dst, int(m.group(2)), int(m.group(3)), 0)
    m = re.match(r"object\.set %(\d+), %(\d+)(?:, %(\d+))?(?:, #(\d+))?$", text)
    if m:
        if m.group(4) is not None:
            return ("OBJ_SET_ATOM", int(m.group(2)), int(m.group(1)), 0, int(m.group(4)))
        return ("OBJ_SET", int(m.group(3)), int(m.group(1)), int(m.group(2)), 0)
    m = re.match(r"object\.clear %(\d+)$", text)
    if m:
        return ("OBJ_CLEAR", 0, int(m.group(1)), 0, 0)
    m = re.match(r"function @(\d+)$", text)
    if m:
        return ("CONST_FUN", dst, 0, 0, indexes.get(int(m.group(1)), -1))
    m = re.match(r"function @(\d+)\((.*)\)$", text)
    if m:
        args = regs(m.group(2)); start = window(args)
        return ("CLOSURE", dst, start, len(args), indexes.get(int(m.group(1)), -1)) if start is not None else "captures not consecutive"
    m = re.match(r"is_enum @(\d+) %(\d+)$", text)
    if m:
        return ("IS_ENUM", dst, int(m.group(2)), 0, ("enum", m.group(1)))
    m = re.match(r"null_check %(\d+)$", text)
    if m:
        return ("NULL_CHECK", int(m.group(1)), 0, 0, 0)
    m = re.match(r"bytes\.freeze %(\d+)$", text)
    if m:
        return ("BYTES_FREEZE", int(m.group(1)), 0, 0, 0)
    m = re.match(r"array\.set %(\d+), %(\d+), %(\d+)$", text)
    if m:
        return ("ARRAY_SET", int(m.group(3)), int(m.group(1)), int(m.group(2)), 0)
    m = re.match(r"bytes\.set_u8 %(\d+), %(\d+), %(\d+)$", text)
    if m:
        return ("BYTES_SET_U8", int(m.group(3)), int(m.group(1)), int(m.group(2)), 0)
    m = re.match(r"bytes\.blit %(\d+), %(\d+), %(\d+)$", text)
    if m:
        return ("BYTES_BLIT", int(m.group(1)), int(m.group(2)), int(m.group(3)), 0)
    m = re.match(r"bits\.read %(\d+), (\d+), (\d+)$", text)
    if m:
        return ("BITS_READ", dst, int(m.group(1)), int(m.group(3)), int(m.group(2)))
    m = re.match(r"bits\.write %(\d+), %(\d+), (\d+), (\d+)$", text)
    if m:
        return ("BITS_WRITE", int(m.group(1)), int(m.group(2)), int(m.group(4)), int(m.group(3)))
    m = re.match(r"bytes\.read\.(\w+)\.(le|be) %(\d+), %(\d+)$", text)
    if m:
        return ("BYTES_READ_%s_%s" % (m.group(1).upper(), m.group(2).upper()), dst, int(m.group(3)), int(m.group(4)), 0)
    m = re.match(r"bytes\.write\.(\w+)\.(le|be) %(\d+), %(\d+), %(\d+)$", text)
    if m:
        return ("BYTES_WRITE_%s_%s" % (m.group(1).upper(), m.group(2).upper()), int(m.group(5)), int(m.group(3)), int(m.group(4)), 0)
    m = re.match(r"array\.resize %(\d+), %(\d+)$", text)
    if m:
        return ("ARRAY_RESIZE", 0, int(m.group(1)), int(m.group(2)), 0)
    m = re.match(r"bytes\.slice %(\d+), %(\d+), %(\d+)$", text)
    if m:
        start = window([int(m.group(2)), int(m.group(3))])
        return ("BYTES_SLICE", dst, int(m.group(1)), start, 0) if start is not None else "slice bounds not consecutive"
    m = re.match(r"tuple\.get %(\d+), (\d+)$", text)
    if m:
        return ("STRUCT_GET", dst, int(m.group(1)), 0, int(m.group(2)))
    m = re.match(r"struct\.set %(\d+), (\d+), %(\d+)$", text)
    if m:
        return ("STRUCT_SET", int(m.group(3)), int(m.group(1)), 0, int(m.group(2)))
    m = re.match(r"enum\.get %(\d+), %(\d+), -?\d+$", text)
    if m:
        return ("ENUM_GET", dst, int(m.group(1)), int(m.group(2)), 0)
    m = re.match(r"enum\.new (\d+)\((.*)\)$", text)
    if m:
        args = regs(m.group(2)); start = window(args)
        return ("ENUM_NEW", dst, start, len(args), int(m.group(1))) if start is not None else "payload not consecutive"
    m = re.match(r"bytes\.concat_many (.*)$", text)
    if m:
        args = regs(m.group(1)); start = window(args)
        return ("BYTES_CONCAT_MANY", dst, start, len(args), 0) if start is not None else "parts not consecutive"
    m = re.match(r"call @(\d+)\((.*)\)$", text)
    if m:
        args = regs(m.group(2)); start = window(args)
        return ("CALL", dst, start, len(args), indexes.get(int(m.group(1)), -1)) if start is not None else "arguments not consecutive"
    m = re.match(r"call native n(\d+)\((.*)\)$", text)
    if m:
        args = regs(m.group(2)); start = window(args)
        return ("CALL_NATIVE", dst, start, len(args), int(m.group(1))) if start is not None else "arguments not consecutive"
    m = re.match(r"call %(\d+)\((.*)\)$", text)
    if m:
        args = regs(m.group(2)); start = window(args)
        return ("CALLR", dst, int(m.group(1)), len(args), start) if start is not None else "arguments not consecutive"
    m = re.match(r"spill\.push %(\d+), (\d+)$", text)
    if m:
        return ("SPILL_PUSH", int(m.group(1)), 0, 0, int(m.group(2)))
    m = re.match(r"spill\.pop (\d+)$", text)
    if m:
        return ("SPILL_POP", dst, 0, 0, int(m.group(1)))
    m = re.match(r"global\.get g(\d+)$", text)
    if m:
        return ("GLOBAL_GET", dst, 0, 0, int(m.group(1)))
    m = re.match(r"global\.set g(\d+), %(\d+)$", text)
    if m:
        return ("GLOBAL_SET", int(m.group(2)), 0, 0, int(m.group(1)))
    return "no encoding known for this instruction"


def matches(want, got, module):
    op, a, b, c, imm = want
    if got[0] != op or got[1] != (a if a is not None else got[1]):
        return False
    # An empty window may name any register.
    if b != "any" and got[2] != b:
        return False
    if c != "any" and got[3] != c:
        return False
    if imm == "any":
        return True
    if isinstance(imm, tuple):
        kind, value = imm
        if kind == "pool":
            return 0 <= got[4] < len(module["bytes"]) and module["bytes"][got[4]] == value
        if kind == "enum":
            return got[4] == int(value) and got[4] < len(module["types"]) and module["types"][got[4]].startswith("ENUM(")
    return got[4] == imm


# --- Control flow ---------------------------------------------------------

def check_function(fi, func, code, module, indexes):
    where = "fn @%d (func[%d])" % (func["id"], fi)
    blocks = func["blocks"]
    # A terminator parse_terminator does not know is left among the
    # instructions, and the block has none.
    unended = [b["id"] for b in blocks.values() if b["term"] is None]
    if unended:
        problem(where, "blocks %s end in no terminator this script knows" % unended[:8])
        return
    catches = {b["handler"][0] for b in blocks.values() if b["handler"]}

    def regions(block):
        handler = blocks[block]["handler"]
        return [] if handler is None else regions(handler[0]) + [handler]

    def transition(source, target):
        left, entered, shared = regions(source), regions(target), 0
        while shared < min(len(left), len(entered)) and left[shared] == entered[shared]:
            shared += 1
        return len(left) - shared, entered[shared:]

    def forward(target):
        seen = set()
        while target not in seen:
            seen.add(target)
            b = blocks[target]
            if b["insns"] or b["term"][0] != "jump" or target in catches:
                break
            target = b["term"][1]
        return target

    def branch_target(source, target):
        """A branch forwards only when it stays in its own try region."""
        to = forward(target)
        return to if blocks[to]["handler"] == blocks[source]["handler"] else target

    pcs, owner, work = {}, {}, [(func["entry"], 0, "the entry")]

    def claim(pc, block):
        if pc >= len(code):
            problem(where, "block %d runs past the end at pc %d" % (block, pc))
            return False
        if pc in owner:
            problem(where, "pc %d belongs to block %d and block %d" % (pc, owner[pc], block))
            return False
        owner[pc] = block
        return True

    def at(pc, op):
        return pc < len(code) and code[pc][0] == op

    while work:
        block, pc, why = work.pop()
        if block in pcs:
            if pcs[block] != pc:
                problem(where, "%s reaches block %d at pc %d, but it is at pc %d" % (why, block, pc, pcs[block]))
            continue
        pcs[block] = pc
        b = blocks[block]
        for text in b["insns"]:
            m = re.match(r"%(\d+) = (.*)$", text)
            dst, body = (int(m.group(1)), m.group(2)) if m else (None, text)
            want = expect(body, dst, func, module, indexes)
            if not claim(pc, block):
                break
            if isinstance(want, str):
                problem(where, "block %d `%s`: %s" % (block, text, want))
            elif not matches(want, code[pc], module):
                problem(where, "block %d `%s` at pc %d: expected %s, decoded %s" % (block, text, pc, want, code[pc]))
            pc += 1
        term = b["term"]
        if term[0] in ("return", "throw"):
            op = "RET" if term[0] == "return" else "THROW"
            if claim(pc, block) and code[pc][:2] != (op, term[1]):
                problem(where, "block %d `%s %%%d` at pc %d decoded as %s" % (block, term[0], term[1], pc, code[pc]))
        elif term[0] == "jump":
            target = forward(term[1])
            t = blocks[target]
            if not t["insns"] and t["term"][0] == "return":
                if claim(pc, block) and code[pc][:2] != ("RET", t["term"][1]):
                    problem(where, "block %d's jump to returning block %d at pc %d decoded as %s" % (block, target, pc, code[pc]))
                continue
            left, opened = transition(block, target)
            for _ in range(left):
                if claim(pc, block) and not at(pc, "ENDTRY"):
                    problem(where, "block %d leaves a try region: expected ENDTRY at pc %d, decoded %s" % (block, pc, code[pc]))
                pc += 1
            for catch, payload in opened:
                if claim(pc, block):
                    op, a, _, _, imm = code[pc]
                    if op != "TRY" or a != payload:
                        problem(where, "block %d enters a try region: expected TRY a=r%d at pc %d, decoded %s" % (block, payload, pc, code[pc]))
                    else:
                        work.append((catch, pc + 1 + imm, "a TRY in block %d" % block))
                pc += 1
            if at(pc, "JMP") and pcs.get(target) != pc and pc not in owner:
                claim(pc, block)
                work.append((target, pc + 1 + code[pc][4], "block %d's jump" % block))
            else:
                work.append((target, pc, "block %d's fallthrough" % block))
        elif term[0] == "branch":
            _, cond, then, other = term
            then, other = branch_target(block, then), branch_target(block, other)
            if then == other:
                if at(pc, "JMP") and pcs.get(then) != pc and pc not in owner:
                    claim(pc, block)
                    work.append((then, pc + 1 + code[pc][4], "block %d's jump" % block))
                else:
                    work.append((then, pc, "block %d's fallthrough" % block))
                continue
            if claim(pc, block):
                if code[pc][:2] != ("JMP_IF", cond):
                    problem(where, "block %d's branch on %%%d at pc %d decoded as %s" % (block, cond, pc, code[pc]))
                else:
                    work.append((then, pc + 1 + code[pc][4], "block %d's branch" % block))
            pc += 1
            if at(pc, "JMP") and pcs.get(other) != pc and pc not in owner:
                claim(pc, block)
                work.append((other, pc + 1 + code[pc][4], "block %d's else jump" % block))
            else:
                work.append((other, pc, "block %d's else fallthrough" % block))
    stray = [pc for pc in range(len(code)) if pc not in owner]
    if stray:
        problem(where, "pcs %s belong to no block" % stray[:8])


def main():
    program, module = parse_ir(sys.argv[1]), parse_disasm(sys.argv[2])
    if len(program["funcs"]) != len(module["funcs"]):
        problem("module", "%d functions allocated, %d decoded" % (len(program["funcs"]), len(module["funcs"])))
    if len(program["globals"]) != len(module["globals"]):
        problem("module", "%d globals allocated, %d decoded" % (len(program["globals"]), len(module["globals"])))
    for i, (ir, ref) in enumerate(zip(program["globals"], module["globals"])):
        if not same_type(module, ir, ref):
            problem("global g%d" % i, "%s decoded as %s" % (ir, ref))
    if program["wides"] != module["i64"]:
        problem("module", "i64 pool %s decoded as %s" % (program["wides"], module["i64"]))
    if program["floats"] != module["f64"]:
        problem("module", "f64 pool %s decoded as %s" % (program["floats"], module["f64"]))
    natives = ["%s(%s) -> %s" % (name, " ".join(NATIVE_KINDS.get(a, "?" + a) for a in args), NATIVE_KINDS.get(ret, "?" + ret))
               for name, args, ret in program["natives"]]
    if natives != module["imports"]:
        problem("module", "natives %s decoded as imports %s" % (natives, module["imports"]))
    indexes = {f["id"]: i for i, f in enumerate(program["funcs"])}
    for fi, (func, decoded) in enumerate(zip(program["funcs"], module["funcs"])):
        where = "fn @%d (func[%d])" % (func["id"], fi)
        if decoded["nregs"] != len(func["regs"]) or decoded["nargs"] != func["nparams"]:
            problem(where, "%d registers and %d parameters allocated, decoded %d and %d"
                    % (len(func["regs"]), func["nparams"], decoded["nregs"], decoded["nargs"]))
            continue
        # Captures follow the arguments; without any, cap_start is nregs.
        if decoded["cap_start"] != (func["nparams"] if func["ncaps"] else len(func["regs"])):
            problem(where, "%d captures after %d arguments, decoded cap_start=%d"
                    % (func["ncaps"], func["nparams"], decoded["cap_start"]))
        for r, (ir, ref) in enumerate(zip(func["regs"], decoded["regs"])):
            if not same_type(module, ir, ref):
                problem(where, "register %%%d is %s, decoded as %s" % (r, ir, ref))
        check_function(fi, func, decoded["code"], module, indexes)
    print("\n".join(problems[:30]))
    sys.exit(1 if problems else 0)


main()
