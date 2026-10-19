"""Differential audit of the lifter's narrow-width integer lowering (pd-q8z).

Lifts one instruction per case with the real Lifter, compiles all cases into one
C program with MSVC, runs corner values, and compares against reference C
computed with explicit-width types.

  .venv-recomp/Scripts/python.exe audit_narrow.py      # writes audit_narrow_gen.c
  then compile + run it with the MSVC environment (see run_audit_narrow.bat)
"""
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "xboxrecomp"))
from tools.recomp.disasm import Instruction, Operand  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402


def R(n):
    return Operand(type="reg", reg=n)


def I(v):
    return Operand(type="imm", imm=v)


def M(sz):
    return Operand(type="mem", mem_base="esi", mem_size=sz)


def lift(m, *ops):
    insn = Instruction(0, 2, m, "", "00")
    insn.operands = list(ops)
    return " ".join(Lifter().lift_instruction(insn))


V8 = [0, 1, 2, 0x7F, 0x80, 0x81, 0xFE, 0xFF, 0x55, 0xAA, 0x10]
V16 = [0, 1, 2, 0x7FFF, 0x8000, 0x8001, 0xFFFF, 0x1234, 0xFF00, 0x00FF, 0x00FE, 0x0100]
V32 = [0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xFFFF0000,
       0xDEADBEEF, 0x0000FFFF, 0x10000]
VALS = {8: V8, 16: V16, 32: V32}
TYPES = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t"}

PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO8(r)  ((uint8_t)((r) & 0xFF))
#define HI8(r)  ((uint8_t)(((r) >> 8) & 0xFF))
#define LO16(r) ((uint16_t)((r) & 0xFFFF))
#define SET_LO8(r, v)  ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(uint8_t)(v)))
#define SET_HI8(r, v)  ((r) = ((r) & 0xFFFF00FFu) | (((uint32_t)(uint8_t)(v)) << 8))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | ((uint32_t)(uint16_t)(v)))
static uint32_t _fa, _fb; static int32_t _fas, _fbs; static int _cf;
static uint8_t g_ram[64];
#define MEM8(a)  (*(volatile uint8_t *)(g_ram + (a)))
#define MEM16(a) (*(volatile uint16_t *)(g_ram + (a)))
#define MEM32(a) (*(volatile uint32_t *)(g_ram + (a)))
#define SMEM8(a)  (*(volatile int8_t *)(g_ram + (a)))
#define SMEM16(a) (*(volatile int16_t *)(g_ram + (a)))
#define SMEM32(a) (*(volatile int32_t *)(g_ram + (a)))
/* Verbatim from templates/runtime/recomp_types.h */
static inline uint32_t ROL32(uint32_t val, int n) { n &= 31; return n ? ((val << n) | (val >> (32 - n))) : val; }
static inline uint32_t ROR32(uint32_t val, int n) { n &= 31; return n ? ((val >> n) | (val << (32 - n))) : val; }
static inline uint8_t ROL8(uint8_t val, int n) { n = (n & 31) % 8; return n ? (uint8_t)((val << n) | (val >> (8 - n))) : val; }
static inline uint8_t ROR8(uint8_t val, int n) { n = (n & 31) % 8; return n ? (uint8_t)((val >> n) | (val << (8 - n))) : val; }
static inline uint16_t ROL16(uint16_t val, int n) { n = (n & 31) % 16; return n ? (uint16_t)((val << n) | (val >> (16 - n))) : val; }
static inline uint16_t ROR16(uint16_t val, int n) { n = (n & 31) % 16; return n ? (uint16_t)((val >> n) | (val << (16 - n))) : val; }
#define ZX8(v)   ((uint32_t)(uint8_t)(v))
#define ZX16(v)  ((uint32_t)(uint16_t)(v))
#define SX8(v)   ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v)  ((uint32_t)(int32_t)(int16_t)(v))
static inline uint32_t BSWAP32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000u);
}
#define RECOMP_UNIMPL(t, va) do { printf("UNLIFTED INSTRUCTION: %s\n", t); fails++; } while (0)
static int fails;
#define REGS uint32_t eax=0xAAAAAAAAu, ecx=0xBBBBBBBBu, edx=0xCCCCCCCCu, ebx=0xDDDDDDDDu, esi=0; (void)eax;(void)ecx;(void)edx;(void)ebx;(void)esi;
#define CHECK(name, cond, ...) do { if (!(cond)) { if (fails < 120) { printf("FAIL %s: ", name); printf(__VA_ARGS__); printf("\n"); } fails++; } } while (0)
"""

cases = []


def add(name, body):
    cases.append((name, body))


def arr(w):
    return "static const uint64_t VS[] = {" + ",".join("0x%XULL" % v for v in VALS[w]) + "};"


def varr(w):
    return "static const uint64_t VA[] = {" + ",".join("0x%XULL" % v for v in VALS[w]) + "};"


# --------------------------------------------------------------------------
# A: one-operand mul / imul / div / idiv
# --------------------------------------------------------------------------
def muldiv_case(m, w, src):
    name = f"{m}{w}-{src}"
    U = TYPES[w]
    op = R({8: "cl", 16: "cx", 32: "ecx"}[w]) if src == "reg" else M(w // 8)
    stmt = lift(m, op)
    # dividends: 16-bit AX for w=8; DX:AX for w=16 (DX derived); EDX:EAX for w=32
    dvals = VALS[16] if w == 8 else VALS[w]
    body = arr(w) + "\n" + "static const uint64_t VA[] = {" + ",".join("0x%XULL" % v for v in dvals) + "};\n"
    body += "for (unsigned i=0;i<sizeof(VA)/8;i++) for (unsigned j=0;j<sizeof(VS)/8;j++) {\n"
    body += " REGS uint64_t a=VA[i], b=VS[j];\n"
    if src == "mem":
        body += f" MEM{w}(0)=({U})b;\n"
    if w == 8:
        body += " eax=(eax&0xFFFF0000u)|(uint32_t)(a&0xFFFF); ecx=(ecx&0xFFFFFF00u)|(uint32_t)(b&0xFF);\n"
    elif w == 16:
        body += (" eax=(eax&0xFFFF0000u)|(uint32_t)a; edx=(edx&0xFFFF0000u)|(uint32_t)(((a*0x9E3779B1ULL)>>7)&0xFFFF);"
                 " ecx=(ecx&0xFFFF0000u)|(uint32_t)b;\n")
    else:
        body += " eax=(uint32_t)a; edx=(uint32_t)((a*0x9E3779B97F4A7C15ULL)>>13); ecx=(uint32_t)b;\n"

    # skip inputs that would raise #DE on real hardware
    if m == "div":
        if w == 8:
            body += " if ((uint8_t)b==0 || (uint16_t)a/(uint8_t)b>0xFF) continue;\n"
        elif w == 16:
            body += " { uint32_t dv=((uint32_t)LO16(edx)<<16)|(uint16_t)a; if ((uint16_t)b==0 || dv/(uint16_t)b>0xFFFF) continue; }\n"
        else:
            body += " { uint64_t dv=((uint64_t)edx<<32)|(uint32_t)a; if ((uint32_t)b==0 || dv/(uint32_t)b>0xFFFFFFFFULL) continue; }\n"
    if m == "idiv":
        if w == 8:
            body += " { int16_t dv=(int16_t)(uint16_t)a; int8_t d=(int8_t)b; if (d==0) continue; { int32_t q=(int32_t)dv/d; if (q>127||q<-128) continue; } }\n"
        elif w == 16:
            body += " { int32_t dv=(int32_t)(((uint32_t)LO16(edx)<<16)|(uint16_t)a); int16_t d=(int16_t)b; if (d==0) continue; { int64_t q=(int64_t)dv/d; if (q>32767||q<-32768) continue; } }\n"
        else:
            body += " { int64_t dv=(int64_t)(((uint64_t)edx<<32)|(uint32_t)a); int32_t d=(int32_t)b; if (d==0 || (d==-1 && dv==INT64_MIN)) continue; { int64_t q=dv/d; if (q>INT32_MAX||q<INT32_MIN) continue; } }\n"
    body += " " + stmt + "\n"

    N = f'"{name}"'
    if m == "mul":
        if w == 8:
            body += f" {{ uint16_t r=(uint16_t)((uint8_t)a*(uint8_t)b); CHECK({N}, LO16(eax)==r && (eax>>16)==0xAAAAu, \"a=%llX b=%llX got=%X want=%X\", a,b,eax&0xFFFF,r); }}\n"
        elif w == 16:
            body += f" {{ uint32_t r=(uint32_t)(uint16_t)a*(uint16_t)b; CHECK({N}, LO16(eax)==(r&0xFFFF) && LO16(edx)==(r>>16) && (eax>>16)==0xAAAAu && (edx>>16)==0xCCCCu, \"a=%llX b=%llX ax=%X dx=%X want=%X\", a,b,eax&0xFFFF,edx&0xFFFF,r); }}\n"
        else:
            body += f" {{ uint64_t r=(uint64_t)(uint32_t)a*(uint32_t)b; CHECK({N}, eax==(uint32_t)r && edx==(uint32_t)(r>>32), \"a=%llX b=%llX eax=%X edx=%X\", a,b,eax,edx); }}\n"
    elif m == "imul":
        if w == 8:
            body += f" {{ int16_t r=(int16_t)((int8_t)a*(int8_t)b); CHECK({N}, LO16(eax)==(uint16_t)r && (eax>>16)==0xAAAAu, \"a=%llX b=%llX got=%X want=%X\", a,b,eax&0xFFFF,(uint16_t)r); }}\n"
        elif w == 16:
            body += f" {{ int32_t r=(int32_t)(int16_t)a*(int16_t)b; CHECK({N}, LO16(eax)==(uint16_t)r && LO16(edx)==(uint16_t)((uint32_t)r>>16) && (eax>>16)==0xAAAAu && (edx>>16)==0xCCCCu, \"a=%llX b=%llX ax=%X dx=%X\", a,b,eax&0xFFFF,edx&0xFFFF); }}\n"
        else:
            body += f" {{ int64_t r=(int64_t)(int32_t)a*(int32_t)b; CHECK({N}, eax==(uint32_t)r && edx==(uint32_t)((uint64_t)r>>32), \"a=%llX b=%llX eax=%X edx=%X\", a,b,eax,edx); }}\n"
    elif m == "div":
        if w == 8:
            body += f" {{ uint8_t q=(uint8_t)((uint16_t)a/(uint8_t)b), rm=(uint8_t)((uint16_t)a%(uint8_t)b); CHECK({N}, LO8(eax)==q && HI8(eax)==rm && (eax>>16)==0xAAAAu, \"a=%llX b=%llX al=%X ah=%X\", a,b,eax&0xFF,(eax>>8)&0xFF); }}\n"
        elif w == 16:
            body += f" {{ uint32_t dv=((uint32_t)(0xCCCC0000u|0)>>16)*0; (void)dv; }}\n"
            # reference needs the original dx: recompute from a
            body += f" {{ uint32_t dx0=(uint32_t)(((a*0x9E3779B1ULL)>>7)&0xFFFF); uint32_t dv=(dx0<<16)|(uint16_t)a; uint16_t q=(uint16_t)(dv/(uint16_t)b), rm=(uint16_t)(dv%(uint16_t)b); CHECK({N}, LO16(eax)==q && LO16(edx)==rm && (eax>>16)==0xAAAAu && (edx>>16)==0xCCCCu, \"dv=%X b=%llX ax=%X dx=%X\", dv,b,eax&0xFFFF,edx&0xFFFF); }}\n"
        else:
            body += f" {{ uint32_t dx0=(uint32_t)((a*0x9E3779B97F4A7C15ULL)>>13); uint64_t dv=((uint64_t)dx0<<32)|(uint32_t)a; uint32_t q=(uint32_t)(dv/(uint32_t)b), rm=(uint32_t)(dv%(uint32_t)b); CHECK({N}, eax==q && edx==rm, \"dv=%llX b=%llX eax=%X edx=%X\", dv,b,eax,edx); }}\n"
    else:  # idiv
        if w == 8:
            body += f" {{ int16_t dv=(int16_t)(uint16_t)a; int8_t d=(int8_t)b; int8_t q=(int8_t)((int32_t)dv/d), rm=(int8_t)((int32_t)dv%d); CHECK({N}, LO8(eax)==(uint8_t)q && HI8(eax)==(uint8_t)rm && (eax>>16)==0xAAAAu, \"a=%llX b=%llX al=%X ah=%X\", a,b,eax&0xFF,(eax>>8)&0xFF); }}\n"
        elif w == 16:
            body += f" {{ uint32_t dx0=(uint32_t)(((a*0x9E3779B1ULL)>>7)&0xFFFF); int32_t dv=(int32_t)((dx0<<16)|(uint16_t)a); int16_t d=(int16_t)b; int16_t q=(int16_t)((int64_t)dv/d), rm=(int16_t)((int64_t)dv%d); CHECK({N}, LO16(eax)==(uint16_t)q && LO16(edx)==(uint16_t)rm && (eax>>16)==0xAAAAu && (edx>>16)==0xCCCCu, \"dv=%X b=%llX ax=%X dx=%X\", dv,b,eax&0xFFFF,edx&0xFFFF); }}\n"
        else:
            body += f" {{ uint32_t dx0=(uint32_t)((a*0x9E3779B97F4A7C15ULL)>>13); int64_t dv=(int64_t)(((uint64_t)dx0<<32)|(uint32_t)a); int32_t d=(int32_t)b; int32_t q=(int32_t)(dv/d), rm=(int32_t)(dv%d); CHECK({N}, eax==(uint32_t)q && edx==(uint32_t)rm, \"dv=%llX b=%llX eax=%X edx=%X\", dv,b,eax,edx); }}\n"
    body += "}\n"
    add(name, body)


for m in ("mul", "imul", "div", "idiv"):
    for w in (8, 16, 32):
        for src in ("reg", "mem"):
            muldiv_case(m, w, src)

# --------------------------------------------------------------------------
# B: imul two- and three-operand
# --------------------------------------------------------------------------
for w, d, s in ((16, "ax", "cx"), (32, "eax", "ecx")):
    stmt = lift("imul", R(d), R(s))
    body = arr(w) + "\nfor (unsigned i=0;i<sizeof(VS)/8;i++) for (unsigned j=0;j<sizeof(VS)/8;j++) { REGS uint64_t a=VS[i], b=VS[j];\n"
    if w == 16:
        body += " eax=(eax&0xFFFF0000u)|(uint32_t)a; ecx=(ecx&0xFFFF0000u)|(uint32_t)b;\n"
    else:
        body += " eax=(uint32_t)a; ecx=(uint32_t)b;\n"
    body += " " + stmt + "\n"
    if w == 16:
        body += ' { uint16_t r=(uint16_t)((int32_t)(int16_t)a*(int16_t)b); CHECK("imul16-2op", LO16(eax)==r && (eax>>16)==0xAAAAu, "a=%llX b=%llX got=%X want=%X", a,b,eax&0xFFFF,r); }\n'
    else:
        body += ' { uint32_t r=(uint32_t)((int64_t)(int32_t)a*(int32_t)b); CHECK("imul32-2op", eax==r, "a=%llX b=%llX got=%X want=%X", a,b,eax,r); }\n'
    body += "}\n"
    add(f"imul{w}-2op", body)
    for imm in (3, -1, -128, 127, 0x1234, 0x7FFF if w == 16 else 0x12345):
        stmt = lift("imul", R(d), R(s), I(imm & 0xFFFFFFFF))
        body = arr(w) + f"\nfor (unsigned j=0;j<sizeof(VS)/8;j++) {{ REGS uint64_t b=VS[j]; int64_t im={imm};\n"
        if w == 16:
            body += " eax=(eax&0xFFFF0000u)|0x5A5Au; ecx=(ecx&0xFFFF0000u)|(uint32_t)b;\n"
        else:
            body += " eax=0x5A5A5A5Au; ecx=(uint32_t)b;\n"
        body += " " + stmt + "\n"
        if w == 16:
            body += f' {{ uint16_t r=(uint16_t)((int32_t)(int16_t)b*(int32_t)im); CHECK("imul16-3op imm={imm}", LO16(eax)==r && (eax>>16)==0xAAAAu, "b=%llX got=%X want=%X", b,eax&0xFFFF,r); }}\n'
        else:
            body += f' {{ uint32_t r=(uint32_t)((int64_t)(int32_t)b*im); CHECK("imul32-3op imm={imm}", eax==r, "b=%llX got=%X want=%X", b,eax,r); }}\n'
        body += "}\n"
        add(f"imul{w}-3op-{imm}", body)

# --------------------------------------------------------------------------
# C: movzx / movsx
# --------------------------------------------------------------------------
for m in ("movzx", "movsx"):
    for dw, dn in ((16, "ax"), (32, "eax")):
        for sw, sn in ((8, "cl"), (8, "ch"), (16, "cx"), (8, "mem8"), (16, "mem16")):
            if sw >= dw:
                continue
            sop = M(sw // 8) if sn.startswith("mem") else R(sn)
            stmt = lift(m, R(dn), sop)
            body = arr(sw) + "\nfor (unsigned j=0;j<sizeof(VS)/8;j++) { REGS uint64_t b=VS[j];\n"
            if sn.startswith("mem"):
                body += f" MEM{sw}(0)=(uint{sw}_t)b;\n"
            elif sn == "cl":
                body += " ecx=(ecx&0xFFFFFF00u)|(uint32_t)b;\n"
            elif sn == "ch":
                body += " ecx=(ecx&0xFFFF00FFu)|((uint32_t)b<<8);\n"
            else:
                body += " ecx=(ecx&0xFFFF0000u)|(uint32_t)b;\n"
            body += " " + stmt + "\n"
            su = "uint8_t" if sw == 8 else "uint16_t"
            ss = "int8_t" if sw == 8 else "int16_t"
            want = f"(uint32_t)({su})b" if m == "movzx" else f"(uint32_t)(int32_t)({ss})b"
            if dw == 16:
                body += f' {{ uint16_t w=(uint16_t)({want}); CHECK("{m}16<-{sn}", LO16(eax)==w && (eax>>16)==0xAAAAu, "b=%llX got=%X want=%X", b,eax&0xFFFF,w); }}\n'
            else:
                body += f' {{ uint32_t w={want}; CHECK("{m}32<-{sn}", eax==w, "b=%llX got=%X want=%X", b,eax,w); }}\n'
            body += "}\n"
            add(f"{m}{dw}-{sn}", body)

# --------------------------------------------------------------------------
# D: sign-extension helpers
# --------------------------------------------------------------------------
for m in ("cbw", "cwde", "cwd", "cdq"):
    stmt = lift(m)
    body = arr(32) + "\nfor (unsigned j=0;j<sizeof(VS)/8;j++) { REGS uint64_t b=VS[j];\n eax=(uint32_t)b;\n " + stmt + "\n"
    if m == "cbw":
        body += ' { uint16_t w=(uint16_t)(int16_t)(int8_t)(uint8_t)b; CHECK("cbw", LO16(eax)==w && (eax>>16)==(uint32_t)(b>>16), "b=%llX got=%X want=%X", b,eax,w); }\n'
    if m == "cwde":
        body += ' { uint32_t w=(uint32_t)(int32_t)(int16_t)(uint16_t)b; CHECK("cwde", eax==w, "b=%llX got=%X want=%X", b,eax,w); }\n'
    if m == "cwd":
        body += ' { uint16_t w=((int16_t)(uint16_t)b<0)?0xFFFF:0; CHECK("cwd", LO16(edx)==w && (edx>>16)==0xCCCCu, "b=%llX dx=%X want=%X", b,edx,w); }\n'
    if m == "cdq":
        body += ' { uint32_t w=((int32_t)(uint32_t)b<0)?0xFFFFFFFFu:0; CHECK("cdq", edx==w, "b=%llX edx=%X want=%X", b,edx,w); }\n'
    body += "}\n"
    add(m, body)

# --------------------------------------------------------------------------
# E/F/G: unary, ALU, shifts on narrow registers (value results + untouched bits)
# --------------------------------------------------------------------------
REGSPEC = [("al", 8, "eax", 0, "0xFFFFFF00u"), ("ah", 8, "eax", 8, "0xFFFF00FFu"),
           ("cl", 8, "ecx", 0, "0xFFFFFF00u"), ("ch", 8, "ecx", 8, "0xFFFF00FFu"),
           ("ax", 16, "eax", 0, "0xFFFF0000u"), ("cx", 16, "ecx", 0, "0xFFFF0000u"),
           ("eax", 32, "eax", 0, "0u")]
INITS = {"eax": "0xAAAAAAAAu", "ecx": "0xBBBBBBBBu"}


def rdv(w, reg, sh):
    if w == 32:
        return reg
    return f"(({reg}>>{sh})&0x{(1 << w) - 1:X}u)"


def wrs(w, reg, sh, mask, val):
    if w == 32:
        return f"{reg}=(uint32_t)({val});"
    return f"{reg}=({reg}&{mask})|(((uint32_t)({val})&0x{(1 << w) - 1:X}u)<<{sh});"


def untouched(reg, w, mask):
    if w == 32:
        return "1"
    return f"(({reg}&{mask})==({INITS[reg]}&{mask}))"


for name, w, reg, sh, mask in REGSPEC:
    U = TYPES[w]
    for m in ("neg", "not", "inc", "dec"):
        stmt = lift(m, R(name))
        ref = {"neg": f"({U})(0-({U})b)", "not": f"({U})(~({U})b)",
               "inc": f"({U})(b+1)", "dec": f"({U})(b-1)"}[m]
        body = (arr(w) + "\nfor (unsigned j=0;j<sizeof(VS)/8;j++) { REGS uint64_t b=VS[j];\n " + wrs(w, reg, sh, mask, "b") + "\n " + stmt
                + f'\n {{ {U} want={ref}; CHECK("{m} {name}", (({U}){rdv(w, reg, sh)})==want && {untouched(reg, w, mask)}, "b=%llX got=%X want=%X", b,{rdv(w, reg, sh)},want); }}\n}}\n')
        add(f"{m}-{name}", body)
    opsym = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}
    for m in ("add", "sub", "and", "or", "xor"):
        for sname, sw, sreg, ssh, smask in REGSPEC:
            if sw != w or sname == name or sreg == reg:
                continue
            stmt = lift(m, R(name), R(sname))
            body = (arr(w) + "\nfor (unsigned i=0;i<sizeof(VS)/8;i++) for (unsigned j=0;j<sizeof(VS)/8;j++) { REGS uint64_t a=VS[i], b=VS[j];\n "
                    + wrs(w, reg, sh, mask, "a") + " " + wrs(w, sreg, ssh, smask, "b") + "\n " + stmt
                    + f'\n {{ {U} want=({U})(({U})a {opsym[m]} ({U})b); CHECK("{m} {name},{sname}", (({U}){rdv(w, reg, sh)})==want && {untouched(reg, w, mask)}, "a=%llX b=%llX got=%X want=%X", a,b,{rdv(w, reg, sh)},want); }}\n}}\n')
            add(f"{m}-{name}-{sname}", body)
        immv = 0x5B if w == 8 else 0x5B5B
        stmt = lift(m, R(name), I(immv))
        body = (arr(w) + "\nfor (unsigned i=0;i<sizeof(VS)/8;i++) { REGS uint64_t a=VS[i];\n " + wrs(w, reg, sh, mask, "a") + "\n " + stmt
                + f'\n {{ {U} want=({U})(({U})a {opsym[m]} ({U}){immv}); CHECK("{m} {name},imm", (({U}){rdv(w, reg, sh)})==want && {untouched(reg, w, mask)}, "a=%llX got=%X want=%X", a,{rdv(w, reg, sh)},want); }}\n}}\n')
        add(f"{m}-{name}-imm", body)
    for m in ("shl", "shr", "rol", "ror"):
        for cnt in (1, 3, 7, 8, 15, 31):
            stmt = lift(m, R(name), I(cnt))
            c = cnt & 31
            if m == "shl":
                ref = f"({U})((uint64_t)({U})b << {c})"
            elif m == "shr":
                ref = f"({U})((uint64_t)({U})b >> {c})"
            elif m == "rol":
                k = c % w
                ref = f"({U})((({U})b << {k}) | (({U})b >> ({w}-{k})))" if k else f"({U})b"
            else:
                k = c % w
                ref = f"({U})((({U})b >> {k}) | (({U})b << ({w}-{k})))" if k else f"({U})b"
            body = (arr(w) + "\nfor (unsigned j=0;j<sizeof(VS)/8;j++) { REGS uint64_t b=VS[j];\n " + wrs(w, reg, sh, mask, "b") + "\n " + stmt
                    + f'\n {{ {U} want={ref}; CHECK("{m} {name},{cnt}", (({U}){rdv(w, reg, sh)})==want && {untouched(reg, w, mask)}, "b=%llX got=%X want=%X", b,{rdv(w, reg, sh)},want); }}\n}}\n')
            add(f"{m}-{name}-{cnt}", body)

# --------------------------------------------------------------------------
# H: xchg / bswap
# --------------------------------------------------------------------------
XCHG = {("al", "cl"): ("0x11223388u", "0x55667744u"), ("ah", "ch"): ("0x11227744u", "0x55663388u"),
        ("al", "ah"): ("0x11224433u", "0x55667788u"), ("ax", "cx"): ("0x11227788u", "0x55663344u"),
        ("eax", "ecx"): ("0x55667788u", "0x11223344u")}
for (a, b), exp in XCHG.items():
    stmt = lift("xchg", R(a), R(b))
    body = "REGS eax=0x11223344u; ecx=0x55667788u;\n" + stmt + "\n"
    body += f'CHECK("xchg {a},{b}", eax=={exp[0]} && ecx=={exp[1]}, "eax=%X ecx=%X", eax, ecx);\n'
    add(f"xchg-{a}-{b}", body)
add("bswap", "REGS eax=0x11223344u;\n" + lift("bswap", R("eax")) + '\nCHECK("bswap", eax==0x44332211u, "eax=%X", eax);\n')


def build():
    src = PRELUDE
    for i, (n, b) in enumerate(cases):
        src += f"static void t{i}(void) {{ /* {n} */\n{b}\n}}\n"
    src += "int main(void){\n"
    for i, (n, b) in enumerate(cases):
        src += f' {{ int f0=fails; t{i}(); if (fails!=f0) printf("  ^ case {n}: %d failures\\n", fails-f0); }}\n'
    src += f' printf("cases={len(cases)} total_failures=%d\\n", fails);\n return fails?1:0;\n}}\n'
    return src


if __name__ == "__main__":
    out = os.path.join(HERE, "audit_narrow_gen.c")
    open(out, "w").write(build())
    print("wrote", out, "cases", len(cases))
