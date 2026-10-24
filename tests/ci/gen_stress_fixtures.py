#!/usr/bin/env python3
"""Generate stress fixtures that are combinatorial or too large to maintain
by hand. Writes .jlo.txt (and .expect where the result is a
fixed numeric) into the directory given as argv[1]."""

from __future__ import annotations

import os
import sys


def write(out_dir: str, name: str, body: str) -> None:
    path = os.path.join(out_dir, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(body)
        if not body.endswith("\n"):
            f.write("\n")


def nregs_at_loader_maximum() -> str:
    kinds = ["I32", "I64", "F64", "BOOL", "BYTES", "STRUCT", "OBJECT", "DYNAMIC"]
    n = 256
    reg_types = []
    for i in range(n):
        k = kinds[i % 8]
        if k == "STRUCT":
            reg_types.append("STRUCT")
        else:
            reg_types.append(k)
    lines = [
        ".jlo module version=1 features=[CONST64] entry=0",
        "types: [I32, I64, F64, BOOL, BYTES, STRUCT(0), OBJECT, DYNAMIC]",
        "sigs: []",
        "atoms: [x]",
        "struct_sigs: [{ nfields=1 named=1 mutable=1 fields=[I32] names=[x] }]",
        "const_i64: [1]",
        "const_f64: [1.0]",
        f"func[0] nregs={n} nargs=0 reg_types=[{', '.join(reg_types)}]",
    ]
    pc = 0
    lines.append(f"  {pc}: CONST_I32 a=r0 b=r0 c=r0 imm=1")
    pc += 1
    for i in range(1, n):
        k = kinds[i % 8]
        if k == "I32":
            lines.append(f"  {pc}: CONST_I32 a=r{i} b=r0 c=r0 imm=1")
        elif k == "I64":
            lines.append(f"  {pc}: CONST_I64 a=r{i} b=r0 c=r0 imm=0")
        elif k == "F64":
            lines.append(f"  {pc}: CONST_F64 a=r{i} b=r0 c=r0 imm=0")
        elif k == "BOOL":
            lines.append(f"  {pc}: CONST_BOOL a=r{i} b=r0 c=r0 imm=1")
        elif k == "BYTES":
            lines.append(f"  {pc}: BYTES_NEW a=r{i} b=r0 c=r0 imm=0")
        elif k == "STRUCT":
            lines.append(f"  {pc}: STRUCT_NEW a=r{i} b=r0 c=r0 imm=0")
        elif k == "OBJECT":
            lines.append(f"  {pc}: OBJ_NEW a=r{i} b=r0 c=r0 imm=0")
        else:
            lines.append(f"  {pc}: TO_DYN a=r{i} b=r0 c=r0 imm=0")
        pc += 1
    lines.append(f"  {pc}: RET a=r0 b=r0 c=r0 imm=0")
    return "\n".join(lines) + "\n"


def nregs_over_loader_maximum() -> str:
    types = ", ".join(["I32"] * 257)
    return (
        ".jlo module version=1 features=[] entry=0\n"
        "types: [I32]\n"
        "sigs: []\n"
        "atoms: []\n"
        "struct_sigs: []\n"
        f"func[0] nregs=257 nargs=0 reg_types=[{types}]\n"
        "  0: CONST_I32 a=r0 b=r0 c=r0 imm=0\n"
        "  1: RET a=r0 b=r0 c=r0 imm=0\n"
    )


def bytes_stack_ceiling() -> str:
    n = 256
    types = ", ".join(["I32"] * n)
    return (
        ".jlo module version=1 features=[] entry=0\n"
        "types: [I32]\n"
        "sigs: []\n"
        "atoms: []\n"
        "struct_sigs: []\n"
        f"func[0] nregs={n} nargs=0 reg_types=[{types}]\n"
        "  0: CALL a=r0 b=r0 c=r0 imm=0\n"
        "  1: RET a=r0 b=r0 c=r0 imm=0\n"
    )


def reg_pressure_gpr() -> str:
    n_live = 16
    nregs = n_live + 1
    types = ", ".join(["I32"] * nregs)
    lines = [
        ".jlo module version=1 features=[] entry=0",
        "types: [I32]",
        "sigs: []",
        "atoms: []",
        "struct_sigs: []",
        f"func[0] nregs={nregs} nargs=0 reg_types=[{types}]",
    ]
    pc = 0
    for i in range(n_live):
        lines.append(f"  {pc}: CONST_I32 a=r{i} b=r0 c=r0 imm={i + 1}")
        pc += 1
    lines.append(f"  {pc}: ADD_I32 a=r{n_live} b=r0 c=r1 imm=0")
    pc += 1
    for i in range(2, n_live):
        lines.append(f"  {pc}: ADD_I32 a=r{n_live} b=r{n_live} c=r{i} imm=0")
        pc += 1
    lines.append(f"  {pc}: RET a=r{n_live} b=r0 c=r0 imm=0")
    return "\n".join(lines) + "\n"


def reg_pressure_xmm() -> str:
    n_live = 16
    nregs = n_live + 1
    types = ", ".join(["F64"] * nregs)
    consts = ", ".join(f"{i + 1}.0" for i in range(n_live))
    lines = [
        ".jlo module version=1 features=[CONST64] entry=0",
        "types: [F64]",
        "sigs: []",
        "atoms: []",
        "struct_sigs: []",
        "const_i64: []",
        f"const_f64: [{consts}]",
        f"func[0] nregs={nregs} nargs=0 reg_types=[{types}]",
    ]
    pc = 0
    for i in range(n_live):
        lines.append(f"  {pc}: CONST_F64 a=r{i} b=r0 c=r0 imm={i}")
        pc += 1
    lines.append(f"  {pc}: ADD_F64 a=r{n_live} b=r0 c=r1 imm=0")
    pc += 1
    for i in range(2, n_live):
        lines.append(f"  {pc}: ADD_F64 a=r{n_live} b=r{n_live} c=r{i} imm=0")
        pc += 1
    lines.append(f"  {pc}: RET a=r{n_live} b=r0 c=r0 imm=0")
    return "\n".join(lines) + "\n"


def reg_pressure_mixed() -> str:
    n_i = 12
    n_f = 12
    nregs = n_i + n_f + 3
    reg_types = ["I32"] * n_i + ["F64"] * n_f + ["F64", "I32", "I32"]
    consts = ", ".join(f"{i + 1}.0" for i in range(n_f))
    lines = [
        ".jlo module version=1 features=[CONST64] entry=0",
        "types: [I32, F64]",
        "sigs: []",
        "atoms: []",
        "struct_sigs: []",
        "const_i64: []",
        f"const_f64: [{consts}]",
        f"func[0] nregs={nregs} nargs=0 reg_types=[{', '.join(reg_types)}]",
    ]
    pc = 0
    for i in range(n_i):
        lines.append(f"  {pc}: CONST_I32 a=r{i} b=r0 c=r0 imm={i + 1}")
        pc += 1
    for i in range(n_f):
        lines.append(f"  {pc}: CONST_F64 a=r{n_i + i} b=r0 c=r0 imm={i}")
        pc += 1
    acc_i = n_i + n_f + 1
    acc_f = n_i + n_f
    conv = n_i + n_f + 2
    lines.append(f"  {pc}: ADD_I32 a=r{acc_i} b=r0 c=r1 imm=0")
    pc += 1
    for i in range(2, n_i):
        lines.append(f"  {pc}: ADD_I32 a=r{acc_i} b=r{acc_i} c=r{i} imm=0")
        pc += 1
    lines.append(f"  {pc}: ADD_F64 a=r{acc_f} b=r{n_i} c=r{n_i + 1} imm=0")
    pc += 1
    for i in range(2, n_f):
        lines.append(f"  {pc}: ADD_F64 a=r{acc_f} b=r{acc_f} c=r{n_i + i} imm=0")
        pc += 1
    lines.append(f"  {pc}: I32_FROM_F64 a=r{conv} b=r{acc_f} c=r0 imm=0")
    pc += 1
    lines.append(f"  {pc}: ADD_I32 a=r{acc_i} b=r{acc_i} c=r{conv} imm=0")
    pc += 1
    lines.append(f"  {pc}: RET a=r{acc_i} b=r0 c=r0 imm=0")
    return "\n".join(lines) + "\n"


def reg_pressure_survive() -> str:
    extra = 10
    nregs1 = 7 + extra
    rtypes1 = ["I32", "BOOL"] + ["I32"] * (nregs1 - 2)
    body = []
    pc = 2
    for i in range(extra):
        body.append(f"  {pc}: CONST_I32 a=r{7 + i} b=r0 c=r0 imm=0")
        pc += 1
    body.append(f"  {pc}: SUB_I32_IMM a=r2 b=r0 c=r0 imm=1")
    pc += 1
    body.append(f"  {pc}: CALL a=r3 b=r2 c=r1 imm=1")
    pc += 1
    body.append(f"  {pc}: SUB_I32_IMM a=r4 b=r0 c=r0 imm=2")
    pc += 1
    body.append(f"  {pc}: CALL a=r5 b=r4 c=r1 imm=1")
    pc += 1
    body.append(f"  {pc}: ADD_I32 a=r6 b=r3 c=r5 imm=0")
    pc += 1
    for i in range(extra):
        body.append(f"  {pc}: ADD_I32 a=r6 b=r6 c=r{7 + i} imm=0")
        pc += 1
    body.append(f"  {pc}: RET a=r6 b=r0 c=r0 imm=0")
    pc += 1
    base_pc = pc
    jmp_imm = base_pc - 2
    lines = [
        ".jlo module version=1 features=[] entry=0",
        "types: [I32, BOOL]",
        "sigs: []",
        "atoms: []",
        "struct_sigs: []",
        "func[0] nregs=2 nargs=0 reg_types=[I32, I32]",
        "  0: CONST_I32 a=r0 b=r0 c=r0 imm=10",
        "  1: CALL a=r1 b=r0 c=r1 imm=1",
        "  2: RET a=r1 b=r0 c=r0 imm=0",
        f"func[1] nregs={nregs1} nargs=1 reg_types=[{', '.join(rtypes1)}]",
        "  0: LT_I32_IMM a=r1 b=r0 c=r0 imm=2",
        f"  1: JMP_IF a=r1 b=r0 c=r0 imm={jmp_imm}",
    ]
    lines.extend(body)
    lines.append(f"  {base_pc}: RET a=r0 b=r0 c=r0 imm=0")
    return "\n".join(lines) + "\n"


def all_size_classes_churn() -> str:
    lengths = [1, 20, 80, 180, 400, 900, 1800, 3500]
    nregs = 4 + len(lengths)
    rtypes = ["I32", "BOOL", "I32", "I32"] + ["BYTES"] * len(lengths)
    n_rounds = 8000
    header = [
        ".jlo module version=1 features=[] entry=0",
        "types: [I32, BOOL, BYTES]",
        "sigs: []",
        "atoms: []",
        "struct_sigs: []",
        f"func[0] nregs={nregs} nargs=0 reg_types=[{', '.join(rtypes)}]",
        f"  0: CONST_I32 a=r0 b=r0 c=r0 imm={n_rounds}",
        "  1: EQ_I32_IMM a=r1 b=r0 c=r0 imm=0",
    ]
    body = []
    pc = 3
    for i, ln in enumerate(lengths):
        body.append(f"  {pc}: CONST_I32 a=r2 b=r0 c=r0 imm={ln}")
        pc += 1
        body.append(f"  {pc}: BYTES_NEW a=r{4 + i} b=r2 c=r0 imm=0")
        pc += 1
    body.append(f"  {pc}: SUB_I32_IMM a=r0 b=r0 c=r0 imm=1")
    pc += 1
    jmp_pc = pc
    body.append("JMP_PLACEHOLDER")
    pc += 1
    exit_pc = pc
    body.append(f"  {pc}: CONST_I32 a=r3 b=r0 c=r0 imm=0")
    pc += 1
    body.append(f"  {pc}: RET a=r3 b=r0 c=r0 imm=0")
    jmp_imm = 1 - (jmp_pc + 1)
    body[body.index("JMP_PLACEHOLDER")] = f"  {jmp_pc}: JMP a=r0 b=r0 c=r0 imm={jmp_imm}"
    jmp_if_imm = exit_pc - 3
    header.append(f"  2: JMP_IF a=r1 b=r0 c=r0 imm={jmp_if_imm}")
    return "\n".join(header + body) + "\n"


def struct_object_pointer_churn() -> str:
    n_rounds = 100000
    return f"""\
.jlo module version=1 features=[] entry=0
types: [I32, BOOL, BYTES, STRUCT(0), OBJECT]
sigs: []
atoms: [buf, inner]
struct_sigs: [{{ nfields=2 named=1 mutable=1 fields=[I32, BYTES] names=[buf, inner] }}]
func[0] nregs=8 nargs=0 reg_types=[I32, BOOL, I32, BYTES, STRUCT, OBJECT, I32, I32]
  0: CONST_I32 a=r0 b=r0 c=r0 imm={n_rounds}
  1: EQ_I32_IMM a=r1 b=r0 c=r0 imm=0
  2: JMP_IF a=r1 b=r0 c=r0 imm=9
  3: CONST_I32 a=r2 b=r0 c=r0 imm=24
  4: BYTES_NEW a=r3 b=r2 c=r0 imm=0
  5: STRUCT_NEW a=r4 b=r0 c=r0 imm=0
  6: STRUCT_SET a=r0 b=r4 c=r0 imm=0
  7: STRUCT_SET a=r3 b=r4 c=r0 imm=1
  8: OBJ_NEW a=r5 b=r0 c=r0 imm=0
  9: OBJ_SET_ATOM a=r4 b=r5 c=r0 imm=1
  10: SUB_I32_IMM a=r0 b=r0 c=r0 imm=1
  11: JMP a=r0 b=r0 c=r0 imm=-11
  12: CONST_I32 a=r6 b=r0 c=r0 imm=0
  13: RET a=r6 b=r0 c=r0 imm=0
"""


def bits_width_fixture(width: int) -> str:
    buf_bytes = 8
    bit_off = buf_bytes * 8 - width
    value = (1 << min(width, 31)) - 1 if width < 32 else 0x7FFFFFFF
    return f"""\
.jlo module version=1 features=[] entry=0
types: [I32, BYTES, BOOL]
sigs: []
atoms: []
struct_sigs: []
func[0] nregs=5 nargs=0 reg_types=[I32, BYTES, I32, I32, BOOL]
  0: CONST_I32 a=r0 b=r0 c=r0 imm={buf_bytes}
  1: BYTES_NEW a=r1 b=r0 c=r0 imm=0
  2: CONST_I32 a=r2 b=r0 c=r0 imm={value}
  3: BITS_WRITE a=r1 b=r2 c=r{width} imm={bit_off}
  4: BITS_READ a=r3 b=r1 c=r{width} imm={bit_off}
  5: EQ_I32 a=r4 b=r2 c=r3 imm=0
  6: RET a=r4 b=r0 c=r0 imm=0
"""


def blit_exactly_at_end() -> str:
    return """\
.jlo module version=1 features=[] entry=0
types: [I32, BYTES]
sigs: []
atoms: []
struct_sigs: []
func[0] nregs=4 nargs=0 reg_types=[I32, BYTES, BYTES, I32]
  0: CONST_I32 a=r0 b=r0 c=r0 imm=16
  1: BYTES_NEW a=r1 b=r0 c=r0 imm=0
  2: CONST_I32 a=r0 b=r0 c=r0 imm=4
  3: BYTES_NEW a=r2 b=r0 c=r0 imm=0
  4: CONST_I32 a=r3 b=r0 c=r0 imm=12
  5: BYTES_BLIT a=r1 b=r2 c=r3 imm=0
  6: RET a=r3 b=r0 c=r0 imm=0
"""


def blit_one_past_end() -> str:
    return """\
.jlo module version=1 features=[] entry=0
types: [I32, BYTES]
sigs: []
atoms: []
struct_sigs: []
func[0] nregs=4 nargs=0 reg_types=[I32, BYTES, BYTES, I32]
  0: CONST_I32 a=r0 b=r0 c=r0 imm=16
  1: BYTES_NEW a=r1 b=r0 c=r0 imm=0
  2: CONST_I32 a=r0 b=r0 c=r0 imm=4
  3: BYTES_NEW a=r2 b=r0 c=r0 imm=0
  4: CONST_I32 a=r3 b=r0 c=r0 imm=13
  5: BYTES_BLIT a=r1 b=r2 c=r3 imm=0
  6: RET a=r3 b=r0 c=r0 imm=0
"""


def blit_u32_max_offset() -> str:
    return """\
.jlo module version=1 features=[] entry=0
types: [I32, BYTES]
sigs: []
atoms: []
struct_sigs: []
func[0] nregs=4 nargs=0 reg_types=[I32, BYTES, BYTES, I32]
  0: CONST_I32 a=r0 b=r0 c=r0 imm=16
  1: BYTES_NEW a=r1 b=r0 c=r0 imm=0
  2: CONST_I32 a=r0 b=r0 c=r0 imm=1
  3: BYTES_NEW a=r2 b=r0 c=r0 imm=0
  4: CONST_I32 a=r3 b=r0 c=r0 imm=-1
  5: BYTES_BLIT a=r1 b=r2 c=r3 imm=0
  6: RET a=r3 b=r0 c=r0 imm=0
"""


def blit_near_u32_max() -> str:
    return """\
.jlo module version=1 features=[] entry=0
types: [I32, BYTES]
sigs: []
atoms: []
struct_sigs: []
func[0] nregs=4 nargs=0 reg_types=[I32, BYTES, BYTES, I32]
  0: CONST_I32 a=r0 b=r0 c=r0 imm=16
  1: BYTES_NEW a=r1 b=r0 c=r0 imm=0
  2: CONST_I32 a=r0 b=r0 c=r0 imm=8
  3: BYTES_NEW a=r2 b=r0 c=r0 imm=0
  4: CONST_I32 a=r3 b=r0 c=r0 imm=-8
  5: BYTES_BLIT a=r1 b=r2 c=r3 imm=0
  6: RET a=r3 b=r0 c=r0 imm=0
"""


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: gen_stress_fixtures.py <out-dir>", file=sys.stderr)
        return 2
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    write(out_dir, "nregs_at_loader_maximum.jlo.txt", nregs_at_loader_maximum())
    write(out_dir, "nregs_over_loader_maximum.jlo.txt", nregs_over_loader_maximum())
    write(out_dir, "bytes_stack_ceiling.jlo.txt", bytes_stack_ceiling())
    write(out_dir, "reg_pressure_gpr.jlo.txt", reg_pressure_gpr())
    write(out_dir, "reg_pressure_xmm.jlo.txt", reg_pressure_xmm())
    write(out_dir, "reg_pressure_mixed.jlo.txt", reg_pressure_mixed())
    write(out_dir, "reg_pressure_survive_interaction.jlo.txt", reg_pressure_survive())
    write(out_dir, "all_size_classes_churn.jlo.txt", all_size_classes_churn())
    write(out_dir, "struct_object_pointer_churn.jlo.txt", struct_object_pointer_churn())
    for w in range(1, 33):
        write(out_dir, f"bits_width_{w:02d}.jlo.txt", bits_width_fixture(w))
    write(out_dir, "blit_exactly_at_end.jlo.txt", blit_exactly_at_end())
    write(out_dir, "blit_one_past_end.jlo.txt", blit_one_past_end())
    write(out_dir, "blit_u32_max_offset.jlo.txt", blit_u32_max_offset())
    write(out_dir, "blit_near_u32_max.jlo.txt", blit_near_u32_max())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
