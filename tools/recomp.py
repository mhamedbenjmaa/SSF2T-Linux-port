#!/usr/bin/env python3
"""Static recompiler: SF2TURBO.EXE x86 code -> portable C.

Every original function becomes a C function F_xxxxxx(void) that reproduces
its instructions on a global CPU state (cpu.h) and the game memory (g_mem).
The output is mechanical and faithful, not readable: it is the baseline the
hand-written decompilation replaces module by module.

usage: recomp.py SF2TURBO.EXE function-ranges.json OUTDIR
"""
import json, struct, sys, os, collections, hashlib
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_REG, CS_OP_IMM, CS_OP_MEM
from capstone import x86_const as X

exe_path, index_path, outdir = sys.argv[1:4]
data = open(exe_path, 'rb').read()
want = json.load(open(index_path)).get('target_sha256')
if want and hashlib.sha256(data).hexdigest() != want:
    sys.exit('recomp.py: %s is not the supported SF2TURBO.EXE (v1.0, sha256 %s...)' % (exe_path, want[:12]))

# ------------------------------------------------------------------ PE ----
pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
image_base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
sections = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = data[o:o + 8].rstrip(b'\0').decode()
    vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', data, o + 8)
    sections.append((name, image_base + va, max(vsize, rawsz), rawptr, rawsz))

def read(va, n):
    for name, s, size, raw, rawsz in sections:
        if s <= va < s + size:
            off = va - s
            return data[raw + off: raw + min(off + n, rawsz)].ljust(min(n, s + size - va), b'\0')
    return None

code_sec = next(s for s in sections if s[0] == 'CODE')
CODE_START, CODE_END = code_sec[1], code_sec[1] + code_sec[2]

def is_code(va):
    return CODE_START <= va < CODE_END

# IAT: address of each import slot -> name
iat = {}
imp_rva = struct.unpack_from('<I', data, pe + 24 + 96 + 8)[0]
if imp_rva:
    d = image_base + imp_rva
    while True:
        ent = read(d, 20)
        ilt, _, _, name_rva, iat_rva = struct.unpack('<IIIII', ent)
        if name_rva == 0: break
        a = image_base + (ilt or iat_rva)
        slot = image_base + iat_rva
        while True:
            e = struct.unpack('<I', read(a, 4))[0]
            if e == 0: break
            nm = read(image_base + e + 2, 64).split(b'\0')[0].decode()
            iat[slot] = nm
            a += 4; slot += 4
        d += 20

# ------------------------------------------------------------ functions --
idx = json.load(open(index_path))['functions']
starts = set()
for f in idx:
    if f['address'].startswith('0x') and f['body']['available']:
        starts.add(int(f['address'], 16))
starts.add(0x110000)

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

insn_cache = {}
def decode(va):
    if va in insn_cache: return insn_cache[va]
    b = read(va, 16)
    ins = None
    if b:
        for i in md.disasm(b, va, count=1):
            ins = i
    insn_cache[va] = ins
    return ins

COND_JMP = {X.X86_INS_JE, X.X86_INS_JNE, X.X86_INS_JB, X.X86_INS_JAE, X.X86_INS_JBE, X.X86_INS_JA,
            X.X86_INS_JL, X.X86_INS_JGE, X.X86_INS_JLE, X.X86_INS_JG, X.X86_INS_JS, X.X86_INS_JNS,
            X.X86_INS_JO, X.X86_INS_JNO, X.X86_INS_JP, X.X86_INS_JNP,
            X.X86_INS_JCXZ, X.X86_INS_JECXZ, X.X86_INS_LOOP, X.X86_INS_LOOPE, X.X86_INS_LOOPNE}
TERMINATORS = {X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRET, X.X86_INS_IRETD, X.X86_INS_HLT}

def jump_table(ins, block):
    """jmp dword ptr [reg*4 + table]: return (index reg, table address, entries)."""
    op = ins.operands[0]
    if op.type != CS_OP_MEM or op.mem.scale != 4 or op.mem.base != 0 or op.mem.index == 0:
        return None
    reg = op.mem.index
    table = op.mem.disp & 0xFFFFFFFF
    n = None
    # look back for "cmp reg, imm" + "ja default"
    for prev in reversed(block[-6:]):
        if prev.id == X.X86_INS_CMP and len(prev.operands) == 2 and prev.operands[0].type == CS_OP_REG \
           and prev.operands[1].type == CS_OP_IMM and same_reg(prev.operands[0].reg, reg):
            n = prev.operands[1].imm + 1
            break
    entries = []
    for i in range(n if n else 256):
        b = read(table + 4 * i, 4)
        if not b: break
        t = struct.unpack('<I', b)[0]
        if not is_code(t):
            break
        entries.append(t)
    return reg, table, entries

def same_reg(a, b):
    return reg_full(a) == reg_full(b)

FULL = {}
for full, parts in [('eax', 'eax ax al ah'), ('ebx', 'ebx bx bl bh'), ('ecx', 'ecx cx cl ch'),
                    ('edx', 'edx dx dl dh'), ('esi', 'esi si'), ('edi', 'edi di'),
                    ('ebp', 'ebp bp'), ('esp', 'esp sp')]:
    for p in parts.split():
        FULL[p] = full

def reg_full(r):
    return FULL.get(md.reg_name(r), md.reg_name(r))

class Func:
    def __init__(self, start):
        self.start = start
        self.insns = {}          # va -> insn
        self.labels = set()
        self.tables = {}         # jmp va -> (reg, table, entries)
        self.tailcalls = set()
        self.problems = []

def explore(start):
    f = Func(start)
    work = [start]
    seen = set()
    while work:
        va = work.pop()
        if va in seen: continue
        block = []
        while True:
            if va in seen: break
            seen.add(va)
            ins = decode(va)
            if ins is None:
                f.problems.append('undecodable at %x' % va)
                break
            f.insns[va] = ins
            block.append(ins)
            nxt = va + ins.size
            iid = ins.id
            if iid in TERMINATORS:
                break
            if iid == X.X86_INS_JMP:
                op = ins.operands[0]
                if op.type == CS_OP_IMM:
                    t = op.imm & 0xFFFFFFFF
                    if t in starts and t != start:
                        f.tailcalls.add(t)
                    else:
                        f.labels.add(t); work.append(t)
                else:
                    jt = jump_table(ins, block)
                    if jt and jt[2]:
                        f.tables[va] = jt
                        for t in jt[2]:
                            f.labels.add(t); work.append(t)
                break
            if iid in COND_JMP:
                t = ins.operands[0].imm & 0xFFFFFFFF
                f.labels.add(t); work.append(t)
            if iid == X.X86_INS_CALL and ins.operands[0].type == CS_OP_IMM:
                t = ins.operands[0].imm & 0xFFFFFFFF
                # calls to a known no-return function would fall into data; we
                # still continue after every call like the CPU would.
            va = nxt
            if va in starts and va != start:
                # fall-through into the next function: treat as a tail call
                f.tailcalls.add(va)
                f.fallthrough = va
                break
    return f

def explore_ok(v):
    # accept a candidate only if its code decodes cleanly and stays sane
    f = explore(v)
    if f.problems or not f.insns: return None
    bad = {X.X86_INS_INVALID} if hasattr(X, 'X86_INS_INVALID') else set()
    for ins in f.insns.values():
        if ins.id in bad or ins.mnemonic in ('(bad)', 'sysret', 'movhps', 'hlt', 'das', 'aas', 'aaa', 'daa', 'arpl'):
            return None
    return f

# ------------------------------------------------------------- codegen --
SEG = {X.X86_REG_ES: 'es', X.X86_REG_CS: 'cs', X.X86_REG_SS: 'ss', X.X86_REG_DS: 'ds',
       X.X86_REG_FS: 'fs', X.X86_REG_GS: 'gs'}

REG8 = {'al': ('eax', 0), 'ah': ('eax', 8), 'bl': ('ebx', 0), 'bh': ('ebx', 8), 'cl': ('ecx', 0),
        'ch': ('ecx', 8), 'dl': ('edx', 0), 'dh': ('edx', 8)}

def rname(r):
    return md.reg_name(r)

def reg_read(r, size=None):
    n = rname(r)
    if n in ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp'):
        return 'R.' + n
    if n in ('ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'):
        return '((uint16_t)R.%s)' % FULL[n]
    if n in REG8:
        full, sh = REG8[n]
        return '((uint8_t)(R.%s >> %d))' % (full, sh) if sh else '((uint8_t)R.%s)' % full
    if n in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
        return 'R.%s' % n
    raise ValueError('register ' + n)

def reg_write(r, expr):
    n = rname(r)
    if n in ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp', 'esp'):
        return 'R.%s = (uint32_t)(%s);' % (n, expr)
    if n in ('ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'):
        return 'SET16(R.%s, %s);' % (FULL[n], expr)
    if n in REG8:
        full, sh = REG8[n]
        return ('SET8H(R.%s, %s);' if sh else 'SET8L(R.%s, %s);') % (full, expr)
    if n in ('es', 'cs', 'ss', 'ds', 'fs', 'gs'):
        return 'set_seg(&R.%s, (uint16_t)(%s));' % (n, expr)
    raise ValueError('register ' + n)

def addr_expr(op):
    m = op.mem
    parts = []
    if m.base: parts.append(reg_read(m.base) if rname(m.base) not in ('eip',) else None)
    if m.index:
        parts.append('%s*%d' % (reg_read(m.index), m.scale) if m.scale != 1 else reg_read(m.index))
    if m.disp or not parts:
        parts.append('0x%xu' % (m.disp & 0xFFFFFFFF))
    e = '(uint32_t)(' + ' + '.join(parts) + ')'
    if ADDR16[0]:
        e = '((%s) & 0xFFFF)' % e        # 16-bit addressing wraps the offset
    seg = SEG.get(m.segment)
    if seg in ('fs', 'gs', 'es') and seg != 'ds':
        # only FS (TIB) and segment loads in low-level code use a non-flat base
        e = '(SEGBASE(%s) + %s)' % (seg, e)
    return e

ADDR16 = [False]
MEMT = {1: 'M8', 2: 'M16', 4: 'M32'}
UT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}

def opsize(op):
    return op.size

def rd(op, size=None):
    if op.type == CS_OP_REG:
        return reg_read(op.reg)
    if op.type == CS_OP_IMM:
        s = size or op.size or 4
        return '(%s)0x%xu' % (UT[s], op.imm & ((1 << (8 * s)) - 1))
    if op.type == CS_OP_MEM:
        s = size or op.size
        return '%s(%s)' % (MEMT[s], addr_expr(op))
    raise ValueError

def wr(op, expr, size=None):
    if op.type == CS_OP_REG:
        return reg_write(op.reg, expr)
    if op.type == CS_OP_MEM:
        s = size or op.size
        return '%s(%s) = (%s)(%s);' % (MEMT[s], addr_expr(op), UT[s], expr)
    raise ValueError

CC = {
    X.X86_INS_JE: 'F.zf', X.X86_INS_JNE: '!F.zf', X.X86_INS_JB: 'F.cf', X.X86_INS_JAE: '!F.cf',
    X.X86_INS_JBE: '(F.cf || F.zf)', X.X86_INS_JA: '(!F.cf && !F.zf)', X.X86_INS_JL: '(F.sf != F.of)',
    X.X86_INS_JGE: '(F.sf == F.of)', X.X86_INS_JLE: '(F.zf || F.sf != F.of)',
    X.X86_INS_JG: '(!F.zf && F.sf == F.of)', X.X86_INS_JS: 'F.sf', X.X86_INS_JNS: '!F.sf',
    X.X86_INS_JO: 'F.of', X.X86_INS_JNO: '!F.of', X.X86_INS_JP: 'parity(F.pv)', X.X86_INS_JNP: '!parity(F.pv)',
}
SETCC = {
    X.X86_INS_SETE: 'F.zf', X.X86_INS_SETNE: '!F.zf', X.X86_INS_SETB: 'F.cf', X.X86_INS_SETAE: '!F.cf',
    X.X86_INS_SETBE: '(F.cf || F.zf)', X.X86_INS_SETA: '(!F.cf && !F.zf)', X.X86_INS_SETL: '(F.sf != F.of)',
    X.X86_INS_SETGE: '(F.sf == F.of)', X.X86_INS_SETLE: '(F.zf || F.sf != F.of)',
    X.X86_INS_SETG: '(!F.zf && F.sf == F.of)', X.X86_INS_SETS: 'F.sf', X.X86_INS_SETNS: '!F.sf',
}

def lbl(va):
    return 'L_%06x' % va

def call_target(op):
    if op.type == CS_OP_IMM:
        return op.imm & 0xFFFFFFFF, None
    return None, rd(op, 4)

stats = collections.Counter()

def gen_insn(f, ins, out):
    iid = ins.id
    ops = ins.operands
    mn = ins.mnemonic
    nxt = ins.address + ins.size
    ADDR16[0] = 0x67 in ins.prefix
    rep = ins.prefix[0] in (0xF3, 0xF2)
    repne = ins.prefix[0] == 0xF2
    e = out.append

    def binop(fn):
        s = ops[0].size
        e('{ uint32_t r_ = %s%d(&F, %s, %s); %s }' % (fn, s * 8, rd(ops[0]), rd(ops[1], s), wr(ops[0], 'r_')))

    if iid == X.X86_INS_MOV:
        if ops[0].type == CS_OP_REG and rname(ops[0].reg) in ('es', 'ds', 'ss', 'fs', 'gs', 'cs'):
            e(wr(ops[0], rd(ops[1], 2)))
        elif ops[1].type == CS_OP_REG and rname(ops[1].reg) in ('es', 'ds', 'ss', 'fs', 'gs', 'cs'):
            e(wr(ops[0], rd(ops[1]), ops[0].size))
        else:
            e(wr(ops[0], rd(ops[1], ops[0].size)))
    elif iid in (X.X86_INS_MOVSX, X.X86_INS_MOVSXD):
        s = ops[1].size
        e(wr(ops[0], '(uint32_t)(int32_t)(%s)%s' % (ST[s], rd(ops[1]))))
    elif iid == X.X86_INS_MOVZX:
        e(wr(ops[0], '(uint32_t)%s' % rd(ops[1])))
    elif iid == X.X86_INS_LEA:
        ADDR = addr_expr(ops[1]).replace('(SEGBASE(fs) + ', '(').replace('(SEGBASE(es) + ', '(').replace('(SEGBASE(gs) + ', '(')
        e(wr(ops[0], ADDR))
    elif iid == X.X86_INS_PUSH:
        s = ops[0].size if ops[0].type != CS_OP_IMM else 4
        if ops[0].type == CS_OP_REG and rname(ops[0].reg) in SEG.values():
            e('PUSH32(%s);' % rd(ops[0]))
        elif s == 2:
            e('PUSH16(%s);' % rd(ops[0]))
        else:
            v = rd(ops[0], 4) if ops[0].type != CS_OP_IMM else '0x%xu' % (ops[0].imm & 0xFFFFFFFF)
            e('PUSH32(%s);' % v)
    elif iid == X.X86_INS_POP:
        if ops[0].type == CS_OP_REG and rname(ops[0].reg) in SEG.values():
            e('{ uint32_t v_ = POP32(); %s }' % wr(ops[0], 'v_'))
        elif ops[0].size == 2:
            e('{ uint16_t v_ = POP16(); %s }' % wr(ops[0], 'v_'))
        else:
            # pop into memory addressed with esp uses esp after the pop
            e('{ uint32_t v_ = POP32(); %s }' % wr(ops[0], 'v_'))
    elif iid == X.X86_INS_PUSHAL:
        e('PUSHA();')
    elif iid == X.X86_INS_POPAL:
        e('POPA();')
    elif iid in (X.X86_INS_PUSHFD, X.X86_INS_PUSHF):
        e('PUSH32(flags_get(&F));')
    elif iid in (X.X86_INS_POPFD, X.X86_INS_POPF):
        e('flags_set(&F, POP32());')
    elif iid == X.X86_INS_ADD: binop('add')
    elif iid == X.X86_INS_SUB: binop('sub')
    elif iid == X.X86_INS_ADC: binop('adc')
    elif iid == X.X86_INS_SBB: binop('sbb')
    elif iid == X.X86_INS_AND: binop('and')
    elif iid == X.X86_INS_OR: binop('or')
    elif iid == X.X86_INS_XOR:
        if ops[0].type == CS_OP_REG and ops[1].type == CS_OP_REG and ops[0].reg == ops[1].reg:
            e(wr(ops[0], '0'))
            e('F.cf = F.of = 0; F.zf = 1; F.sf = 0; F.pv = 0;')
        else:
            binop('xor')
    elif iid == X.X86_INS_CMP:
        s = ops[0].size
        e('sub%d(&F, %s, %s);' % (s * 8, rd(ops[0]), rd(ops[1], s)))
    elif iid == X.X86_INS_TEST:
        s = ops[0].size
        e('and%d(&F, %s, %s);' % (s * 8, rd(ops[0]), rd(ops[1], s)))
    elif iid in (X.X86_INS_INC, X.X86_INS_DEC):
        s = ops[0].size
        e('{ uint32_t r_ = %s%d(&F, %s); %s }' % ('inc' if iid == X.X86_INS_INC else 'dec', s * 8, rd(ops[0]), wr(ops[0], 'r_')))
    elif iid == X.X86_INS_NEG:
        s = ops[0].size
        e('{ uint32_t r_ = sub%d(&F, 0, %s); F.cf = (%s) != 0; %s }' % (s * 8, rd(ops[0]), rd(ops[0]), wr(ops[0], 'r_')))
    elif iid == X.X86_INS_NOT:
        e(wr(ops[0], '~%s' % rd(ops[0])))
    elif iid in (X.X86_INS_SHL, X.X86_INS_SAL, X.X86_INS_SHR, X.X86_INS_SAR, X.X86_INS_ROL, X.X86_INS_ROR,
                 X.X86_INS_RCL, X.X86_INS_RCR):
        s = ops[0].size
        cnt = rd(ops[1], 1) if len(ops) > 1 else '1'
        name = {X.X86_INS_SHL: 'shl', X.X86_INS_SAL: 'shl', X.X86_INS_SHR: 'shr', X.X86_INS_SAR: 'sar',
                X.X86_INS_ROL: 'rol', X.X86_INS_ROR: 'ror', X.X86_INS_RCL: 'rcl', X.X86_INS_RCR: 'rcr'}[iid]
        e('{ uint32_t r_ = %s%d(&F, %s, (%s) & 31); %s }' % (name, s * 8, rd(ops[0]), cnt, wr(ops[0], 'r_')))
    elif iid == X.X86_INS_IMUL:
        if len(ops) == 1:
            s = ops[0].size
            e('imul1_%d(&F, %s);' % (s * 8, rd(ops[0])))
        else:
            s = ops[0].size
            a = rd(ops[1], s) if len(ops) == 3 else rd(ops[0])
            b = rd(ops[2], s) if len(ops) == 3 else rd(ops[1], s)
            e('{ uint32_t r_ = imul%d(&F, %s, %s); %s }' % (s * 8, a, b, wr(ops[0], 'r_')))
    elif iid == X.X86_INS_MUL:
        e('mul1_%d(&F, %s);' % (ops[0].size * 8, rd(ops[0])))
    elif iid == X.X86_INS_IDIV:
        e('idiv%d(%s);' % (ops[0].size * 8, rd(ops[0])))
    elif iid == X.X86_INS_DIV:
        e('div%d(%s);' % (ops[0].size * 8, rd(ops[0])))
    elif iid == X.X86_INS_CDQ:
        e('R.edx = (int32_t)R.eax < 0 ? 0xFFFFFFFFu : 0;')
    elif iid == X.X86_INS_CWD:
        e('SET16(R.edx, (int16_t)R.eax < 0 ? 0xFFFF : 0);')
    elif iid == X.X86_INS_CBW:
        e('SET16(R.eax, (uint16_t)(int16_t)(int8_t)R.eax);')
    elif iid == X.X86_INS_CWDE:
        e('R.eax = (uint32_t)(int32_t)(int16_t)R.eax;')
    elif iid == X.X86_INS_XCHG:
        s = ops[0].size
        e('{ %s t_ = %s; %s %s }' % (UT[s], rd(ops[0]), wr(ops[0], rd(ops[1])), wr(ops[1], 't_')))
    elif iid == X.X86_INS_ENTER:
        e('PUSH32(R.ebp); R.ebp = R.esp; R.esp -= %du;' % ops[0].imm)
    elif iid == X.X86_INS_LEAVE:
        e('R.esp = R.ebp; R.ebp = POP32();')
    elif iid in (X.X86_INS_NOP, X.X86_INS_WAIT, X.X86_INS_BOUND):
        pass
    elif iid in SETCC:
        e(wr(ops[0], '(%s) ? 1 : 0' % SETCC[iid]))
    elif iid == X.X86_INS_CLD: e('F.df = 0;')
    elif iid == X.X86_INS_STD: e('F.df = 1;')
    elif iid == X.X86_INS_CLC: e('F.cf = 0;')
    elif iid == X.X86_INS_STC: e('F.cf = 1;')
    elif iid == X.X86_INS_CMC: e('F.cf = !F.cf;')
    elif iid == X.X86_INS_SAHF: e('sahf(&F);')
    elif iid == X.X86_INS_LAHF: e('lahf(&F);')
    elif iid == X.X86_INS_XLATB: e('SET8L(R.eax, M8(R.ebx + (uint8_t)R.eax));')
    elif iid in (X.X86_INS_MOVSB, X.X86_INS_MOVSW, X.X86_INS_MOVSD, X.X86_INS_STOSB, X.X86_INS_STOSW,
                 X.X86_INS_STOSD, X.X86_INS_LODSB, X.X86_INS_LODSW, X.X86_INS_LODSD, X.X86_INS_SCASB,
                 X.X86_INS_SCASW, X.X86_INS_SCASD, X.X86_INS_CMPSB, X.X86_INS_CMPSW, X.X86_INS_CMPSD):
        base = mn.split()[-1] if ' ' in mn else mn
        kind = base[:4]          # movs stos lods scas cmps
        sz = {'b': 1, 'w': 2, 'd': 4}[base[4]]
        mode = 'REPNE' if repne else ('REP' if rep else 'ONCE')
        e('str_%s(&F, %d, %s);' % (kind, sz, mode))
    elif iid in (X.X86_INS_LOOP, X.X86_INS_LOOPE, X.X86_INS_LOOPNE):
        t = ops[0].imm & 0xFFFFFFFF
        cond = {X.X86_INS_LOOP: '', X.X86_INS_LOOPE: ' && F.zf', X.X86_INS_LOOPNE: ' && !F.zf'}[iid]
        e('if (--R.ecx != 0%s) goto %s;' % (cond, lbl(t)))
    elif iid in (X.X86_INS_JCXZ, X.X86_INS_JECXZ):
        t = ops[0].imm & 0xFFFFFFFF
        e('if (%s == 0) goto %s;' % ('R.ecx' if iid == X.X86_INS_JECXZ else '(uint16_t)R.ecx', lbl(t)))
    elif iid in CC:
        t = ops[0].imm & 0xFFFFFFFF
        poll = 'IRQ_POLL(); ' if t <= ins.address else ''
        e('if (%s) { %sgoto %s; }' % (CC[iid], poll, lbl(t)))
    elif iid == X.X86_INS_JMP:
        op = ops[0]
        if op.type == CS_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            if t in f.tailcalls:
                e('FLAGS_OUT(); F_%06x(); return;' % t)
            else:
                e('%sgoto %s;' % ('IRQ_POLL(); ' if t <= ins.address else '', lbl(t)))
        elif ins.address in f.tables:
            reg, table, entries = f.tables[ins.address]
            e('switch (M32(%s)) {' % addr_expr(op))
            for t in sorted(set(entries)):
                e('  case 0x%xu: goto %s;' % (t, lbl(t)))
            e('  default: FLAGS_OUT(); jump_indirect(M32(%s)); return;' % addr_expr(op))
            e('}')
        else:
            stats['indirect jmp'] += 1
            e('FLAGS_OUT(); jump_indirect(%s); return;' % rd(op, 4))
    elif iid == X.X86_INS_CALL:
        op = ops[0]
        if op.type == CS_OP_IMM:
            t = op.imm & 0xFFFFFFFF
            e('PUSH32(0x%xu); RS_PUSH(0x%xu); FLAGS_OUT(); F_%06x(); FLAGS_IN();' % (nxt, nxt, t))
            CALLS.add(t)
        elif op.type == CS_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and (op.mem.disp & 0xFFFFFFFF) in iat:
            name = iat[op.mem.disp & 0xFFFFFFFF]
            e('PUSH32(0x%xu); FLAGS_OUT(); api_%s(); FLAGS_IN();' % (nxt, name))
            APIS.add(name)
        elif op.size == 6:
            e('PUSH32(R.cs); PUSH32(0x%xu); FLAGS_OUT(); call_far(%s); FLAGS_IN();' % (nxt, addr_expr(op)))
        else:
            e('{ uint32_t t_ = %s; PUSH32(0x%xu); FLAGS_OUT(); call_indirect(t_); FLAGS_IN(); }' % (rd(op, 4), nxt))
    elif iid == X.X86_INS_RET:
        n = ops[0].imm if ops else 0
        e('FLAGS_OUT(); RET(%d); return;' % n)
    elif iid == X.X86_INS_RETF:
        n = ops[0].imm if ops else 0
        e('R.esp += %du; FLAGS_OUT(); return;' % (8 + n))
    elif iid in (X.X86_INS_IRET, X.X86_INS_IRETD):
        e('R.esp += 12; FLAGS_OUT(); return;')
    elif iid == X.X86_INS_INT:
        e('FLAGS_OUT(); sys_int(0x%x); FLAGS_IN();' % (ops[0].imm & 0xFF))
    elif iid == X.X86_INS_INT3:
        e('/* int3 */')
    elif iid == X.X86_INS_IN:
        s = ops[0].size
        port = rd(ops[1], 2) if ops[1].type == CS_OP_REG else '0x%x' % ops[1].imm
        e(wr(ops[0], 'port_in(%s, %d)' % (port, s)))
    elif iid == X.X86_INS_OUT:
        s = ops[1].size
        port = rd(ops[0], 2) if ops[0].type == CS_OP_REG else '0x%x' % ops[0].imm
        e('port_out(%s, %s, %d);' % (port, rd(ops[1]), s))
    elif iid in (X.X86_INS_OUTSB, X.X86_INS_OUTSW, X.X86_INS_OUTSD, X.X86_INS_INSB, X.X86_INS_INSW, X.X86_INS_INSD):
        base = mn.split()[-1]
        sz = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
        e('str_%s(&F, %d, %s);' % (base[:4], sz, 'REP' if rep else 'ONCE'))
    elif iid == X.X86_INS_CLI: e('cpu_cli();')
    elif iid == X.X86_INS_STI: e('cpu_sti();')
    elif iid == X.X86_INS_HLT: e('cpu_hlt();')
    elif iid in (X.X86_INS_LDS, X.X86_INS_LES, X.X86_INS_LFS, X.X86_INS_LGS, X.X86_INS_LSS):
        seg = {X.X86_INS_LDS: 'ds', X.X86_INS_LES: 'es', X.X86_INS_LFS: 'fs', X.X86_INS_LGS: 'gs', X.X86_INS_LSS: 'ss'}[iid]
        a = addr_expr(ops[1])
        e('{ uint32_t a_ = %s; %s set_seg(&R.%s, M16(a_ + %d)); }' % (a, reg_write(ops[0].reg, 'M%d(a_)' % (ops[0].size * 8)), seg, ops[0].size))
    elif iid in (X.X86_INS_LAR, X.X86_INS_LSL):
        e('%s F.zf = 1;' % reg_write(ops[0].reg, '0xFFFFFu' if iid == X.X86_INS_LSL else '0x00CFF300u'))
    elif mn.startswith('f'):
        gen_fpu(ins, e)
    else:
        stats['UNSUPPORTED ' + mn] += 1
        e('unsupported(0x%xu, "%s %s");' % (ins.address, mn, ins.op_str))

def gen_fpu(ins, e):
    mn = ins.mnemonic
    ops = ins.operands
    def mem_load(op):
        s = op.size
        a = addr_expr(op)
        if mn.startswith('fi'):
            return {2: '(double)(int16_t)M16(%s)', 4: '(double)(int32_t)M32(%s)', 8: '(double)(int64_t)M64(%s)'}[s] % a
        return {4: 'f32_load(%s)', 8: 'f64_load(%s)', 10: 'f80_load(%s)'}[s] % a
    def st(i): return 'FPU_ST(%d)' % i
    def src_dst():
        if not ops: return st(0), st(1)
        if len(ops) == 1:
            if ops[0].type == CS_OP_MEM: return st(0), mem_load(ops[0])
            return st(0), 'FPU_ST(%d)' % (ops[0].reg - X.X86_REG_ST0)
        return 'FPU_ST(%d)' % (ops[0].reg - X.X86_REG_ST0), 'FPU_ST(%d)' % (ops[1].reg - X.X86_REG_ST0)
    simple = {'fadd': '+', 'fmul': '*', 'fsub': '-', 'fdiv': '/', 'fiadd': '+', 'fimul': '*', 'fisub': '-', 'fidiv': '/'}
    rev = {'fsubr': '-', 'fdivr': '/', 'fisubr': '-', 'fidivr': '/'}
    pop = {'faddp': '+', 'fmulp': '*', 'fsubp': '-', 'fdivp': '/'}
    popr = {'fsubrp': '-', 'fdivrp': '/'}
    if mn in ('fld', 'fild'):
        if ops[0].type == CS_OP_MEM: e('fpu_push(%s);' % mem_load(ops[0]))
        else: e('fpu_push(FPU_ST(%d));' % (ops[0].reg - X.X86_REG_ST0))
    elif mn == 'fld1': e('fpu_push(1.0);')
    elif mn == 'fldz': e('fpu_push(0.0);')
    elif mn in ('fst', 'fstp'):
        op = ops[0]
        if op.type == CS_OP_MEM:
            e({4: 'f32_store(%s, FPU_ST(0));', 8: 'f64_store(%s, FPU_ST(0));', 10: 'f80_store(%s, FPU_ST(0));'}[op.size] % addr_expr(op))
        else:
            e('FPU_ST(%d) = FPU_ST(0);' % (op.reg - X.X86_REG_ST0))
        if mn == 'fstp': e('fpu_pop();')
    elif mn in ('fist', 'fistp'):
        op = ops[0]
        e({2: 'M16(%s) = (uint16_t)fpu_to_int(FPU_ST(0), 16);', 4: 'M32(%s) = (uint32_t)fpu_to_int(FPU_ST(0), 32);',
           8: 'M64(%s) = (uint64_t)fpu_to_int(FPU_ST(0), 64);'}[op.size] % addr_expr(op))
        if mn == 'fistp': e('fpu_pop();')
    elif mn in simple:
        d, s = src_dst()
        e('%s = %s %s %s;' % (d, d, simple[mn], s))
    elif mn in rev:
        d, s = src_dst()
        e('%s = %s %s %s;' % (d, s, rev[mn], d))
    elif mn in pop:
        d = 'FPU_ST(%d)' % (ops[0].reg - X.X86_REG_ST0) if ops else 'FPU_ST(1)'
        e('%s = %s %s FPU_ST(0); fpu_pop();' % (d, d, pop[mn]))
    elif mn in popr:
        d = 'FPU_ST(%d)' % (ops[0].reg - X.X86_REG_ST0) if ops else 'FPU_ST(1)'
        e('%s = FPU_ST(0) %s %s; fpu_pop();' % (d, popr[mn], d))
    elif mn in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp', 'ficom', 'ficomp'):
        if ops and ops[0].type == CS_OP_MEM: other = mem_load(ops[0])
        elif ops: other = 'FPU_ST(%d)' % (ops[0].reg - X.X86_REG_ST0)
        else: other = 'FPU_ST(1)'
        e('fpu_compare(FPU_ST(0), %s);' % other)
        if mn.endswith('pp'): e('fpu_pop(); fpu_pop();')
        elif mn.endswith('p'): e('fpu_pop();')
    elif mn == 'ftst': e('fpu_compare(FPU_ST(0), 0.0);')
    elif mn == 'fxam': e('fpu_xam();')
    elif mn in ('fnstsw', 'fstsw'):
        op = ops[0]
        e(wr(op, 'fpu_status()', 2))
    elif mn in ('fnstcw', 'fstcw'):
        e('M16(%s) = fpu.cw;' % addr_expr(ops[0]))
    elif mn == 'fldcw':
        e('fpu.cw = M16(%s);' % addr_expr(ops[0]))
    elif mn == 'fchs': e('FPU_ST(0) = -FPU_ST(0);')
    elif mn == 'fabs': e('FPU_ST(0) = fabs(FPU_ST(0));')
    elif mn == 'fsqrt': e('FPU_ST(0) = sqrt(FPU_ST(0));')
    elif mn == 'fxch':
        i = ops[0].reg - X.X86_REG_ST0 if ops else 1
        e('{ double t_ = FPU_ST(0); FPU_ST(0) = FPU_ST(%d); FPU_ST(%d) = t_; }' % (i, i))
    elif mn == 'fprem': e('FPU_ST(0) = fmod(FPU_ST(0), FPU_ST(1));')
    elif mn == 'fptan': e('FPU_ST(0) = tan(FPU_ST(0)); fpu_push(1.0);')
    elif mn in ('finit', 'fninit'): e('fpu_init();')
    elif mn in ('fnclex', 'fclex'): e('fpu.sw &= 0x7F00;')
    elif mn in ('fnstenv', 'fstenv'): e('fpu_stenv(%s);' % addr_expr(ops[0]))
    elif mn in ('fwait',): pass
    else:
        stats['UNSUPPORTED ' + mn] += 1
        e('unsupported(0x%xu, "%s %s");' % (ins.address, mn, ins.op_str))

CALLS = set()
APIS = set()

def gen_function(f):
    out = []
    out.append('void R_%06x(void) {' % f.start)
    out.append('  FLAGS F; FLAGS_IN();')
    order = sorted(f.insns)
    labels = set(f.labels)
    prev_end = None
    for va in order:
        ins = f.insns[va]
        if prev_end is not None and prev_end != va:
            # previous instruction did not fall through to this one
            pass
        if va in labels or va != order[0] and prev_end != va:
            out.append('%s:;' % lbl(va))
        body = []
        try:
            gen_insn(f, ins, body)
        except Exception as ex:
            stats['codegen error'] += 1
            body = ['unsupported(0x%xu, "codegen: %s");' % (va, str(ex).replace('"', "'"))]
        out.append('  /* %06x %s %s */' % (va, ins.mnemonic, ins.op_str.replace('*/', '* /')))
        for b in body:
            out.append('  ' + b)
        prev_end = va + ins.size
        # falling off a block that is followed by a gap: jump explicitly
        nxt_in = va + ins.size in f.insns
        if not nxt_in and ins.id not in TERMINATORS and ins.id != X.X86_INS_JMP:
            ft = va + ins.size
            if ft in f.tailcalls:
                out.append('  FLAGS_OUT(); F_%06x(); return;' % ft)
            else:
                out.append('  unsupported(0x%xu, "fell off function");' % ft)
    # labels that are referenced but were not generated (should not happen)
    for l in labels:
        if l not in f.insns:
            out.append('%s:; unsupported(0x%xu, "missing label"); return;' % (lbl(l), l))
    out.append('}')
    return out

def main():
    os.makedirs(outdir, exist_ok=True)
    funcs = []
    for s in sorted(starts):
        funcs.append(explore(s))
    # extra entries referenced by calls but not known to Ghidra
    known = {f.start for f in funcs}
    extra = set()
    for f in funcs:
        for va, ins in f.insns.items():
            if ins.id == X.X86_INS_CALL and ins.operands[0].type == CS_OP_IMM:
                t = ins.operands[0].imm & 0xFFFFFFFF
                if t not in known and is_code(t):
                    extra.add(t)
            for t in f.tailcalls:
                if t not in known: extra.add(t)
    for s in sorted(extra):
        starts.add(s)
    for s in sorted(extra):
        funcs.append(explore(s))
    # Hidden entry points: code addresses stored as data (function pointer
    # tables, callbacks) or used as immediates, outside every known body.
    ranges = []
    for fi in idx:
        if fi['address'].startswith('0x') and fi['body']['available']:
            for r in fi['body']['ranges']:
                ranges.append((int(r['start'], 16), int(r['end'], 16)))
    for f in funcs:
        for va, ins in f.insns.items():
            ranges.append((va, va + ins.size - 1))
    ranges.sort()
    import bisect
    rstarts = [r[0] for r in ranges]
    def covered(a):
        i = bisect.bisect_right(rstarts, a) - 1
        while i >= 0 and ranges[i][0] > a - 64:
            if ranges[i][0] <= a <= ranges[i][1]: return True
            i -= 1
        return False
    cands = set()
    for name, s0, size, raw, rawsz in sections:
        if name in ('CODE', '.reloc', '.idata', '.edata'): continue
        blob = data[raw:raw + rawsz]
        for o in range(0, len(blob) - 3):
            v = struct.unpack_from('<I', blob, o)[0]
            if is_code(v) and v not in starts and not covered(v): cands.add(v)
    for f in funcs:
        for va, ins in f.insns.items():
            for op in ins.operands:
                if op.type == CS_OP_IMM:
                    v = op.imm & 0xFFFFFFFF
                    if is_code(v) and v not in starts and not covered(v) and ins.id not in COND_JMP \
                       and ins.id not in (X.X86_INS_JMP, X.X86_INS_CALL):
                        cands.add(v)
    added = []
    for v in sorted(cands):
        b = read(v, 2)
        if not b or b[0] in (0x00, 0xCC): continue
        g = explore_ok(v)
        if g: starts.add(v); added.append(g)
    funcs.extend(added)
    print('hidden entry points added: %d' % len(added))
    # close over calls and tail calls until every target has a function
    while True:
        known = {f.start for f in funcs}
        new = set()
        for f in funcs:
            for va, ins in f.insns.items():
                if ins.id == X.X86_INS_CALL and ins.operands[0].type == CS_OP_IMM:
                    t = ins.operands[0].imm & 0xFFFFFFFF
                    if t not in known and is_code(t): new.add(t)
            for t in f.tailcalls:
                if t not in known: new.add(t)
        if not new: break
        for t in new: starts.add(t)
        for t in sorted(new): funcs.append(explore(t))
        print('call targets added: %d' % len(new))
    funcs.sort(key=lambda f: f.start)

    # generate in chunks
    chunk = 400
    names = []
    for ci in range(0, len(funcs), chunk):
        part = funcs[ci:ci + chunk]
        fn = os.path.join(outdir, 'gen_%02d.c' % (ci // chunk))
        names.append(fn)
        with open(fn, 'w') as o:
            o.write('// Generated by recomp.py from SF2TURBO.EXE - do not edit.\n')
            o.write('#include "recomp.h"\n\n')
            for f in part:
                o.write('\n'.join(gen_function(f)))
                o.write('\n\n')
    overrides = set()
    ov_path = os.path.join(outdir, '..', '..', 'overrides.txt')
    if os.path.exists(ov_path):
        for line in open(ov_path):
            line = line.split('#')[0].strip()
            if line: overrides.add(int(line, 16))
    with open(os.path.join(outdir, 'funcs.h'), 'w') as o:
        o.write('// Generated by recomp.py - do not edit.\n#pragma once\n')
        for f in funcs:
            o.write('void F_%06x(void);\nvoid R_%06x(void);\n' % (f.start, f.start))
    # F_ = entry point used by every call; R_ = recompiled body. Functions
    # listed in overrides.txt get a hand-written F_ instead.
    with open(os.path.join(outdir, 'links.c'), 'w') as o:
        o.write('// Generated by recomp.py - do not edit.\n#include "recomp.h"\n')
        for f in funcs:
            if f.start not in overrides:
                o.write('void F_%06x(void) { R_%06x(); }\n' % (f.start, f.start))
    with open(os.path.join(outdir, 'functable.c'), 'w') as o:
        o.write('// Generated by recomp.py - do not edit.\n#include "recomp.h"\n')
        o.write('const FuncEntry g_functable[] = {\n')
        for f in funcs:
            o.write('  { 0x%xu, F_%06x },\n' % (f.start, f.start))
        o.write('};\nconst int g_functable_n = %d;\n' % len(funcs))
    with open(os.path.join(outdir, 'apis.h'), 'w') as o:
        o.write('// Generated by recomp.py - do not edit.\n#pragma once\n')
        for a in sorted(APIS):
            o.write('void api_%s(void);\n' % a)
        o.write('#define IAT_ENTRIES \\\n')
        for slot, name in sorted(iat.items()):
            o.write('  X(0x%xu, %s) \\\n' % (slot, name))
        o.write('\n')
    probs = sum(len(f.problems) for f in funcs)
    print('functions: %d (%d added from calls)  instructions: %d  problems: %d' %
          (len(funcs), len(extra), sum(len(f.insns) for f in funcs), probs))
    for k, v in stats.most_common():
        print('  %s: %d' % (k, v))

main()
