#!/usr/bin/env python3
"""Build a per-function fact table for SF2TURBO.EXE.

Inputs: the Ghidra decompile index (function ranges) and the executable.
Output: functions.json with, for each function, its size, direct callees and
callers, referenced strings, I/O ports, software interrupts and Win32 imports.
"""
import json, re, struct, subprocess, sys, bisect, os

exe, index, out = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(exe, 'rb').read()

# --- PE sections (for VA -> file offset) ---
pe = struct.unpack_from('<I', data, 0x3C)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
base = struct.unpack_from('<I', data, pe + 24 + 28)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + optsz + i * 40
    name = data[o:o + 8].rstrip(b'\0').decode()
    vsize, va, rawsz, rawptr = struct.unpack_from('<IIII', data, o + 8)
    secs.append((name, base + va, max(vsize, rawsz), rawptr, rawsz))

def va_bytes(va, n):
    for name, s, size, raw, rawsz in secs:
        if s <= va < s + size:
            off = va - s
            if off >= rawsz: return b''
            return data[raw + off: raw + min(off + n, rawsz)]
    return b''

# --- imports: IAT address -> name ---
iat = {}
p = subprocess.run(['objdump', '-p', exe], capture_output=True, text=True).stdout
for m in re.finditer(r'^\s+([0-9a-f]{8})\s+<none>\s+[0-9a-f]+\s+(\w+)', p, re.M):
    iat[int(m[1], 16)] = m[2]

# --- strings in DATA: VA -> text ---
strings = {}
for name, s, size, raw, rawsz in secs:
    if name != 'DATA': continue
    blob = data[raw:raw + rawsz]
    for m in re.finditer(rb'[\x20-\x7e\t\r\n]{4,}\x00', blob):
        strings[s + m.start()] = m.group()[:-1].decode('latin1')

# --- functions ---
idx = json.load(open(index))['functions']
funcs = []
for f in idx:
    if not f['body']['available'] or not f['address'].startswith('0x'): continue
    rs = [(int(r['start'], 16), int(r['end'], 16)) for r in f['body']['ranges']]
    funcs.append({'addr': int(f['address'], 16), 'name': f['name'], 'ranges': rs})
funcs.sort(key=lambda f: f['addr'])
starts = [f['addr'] for f in funcs]
by_addr = {f['addr']: f for f in funcs}

def owner(va):
    i = bisect.bisect_right(starts, va) - 1
    if i < 0: return None
    f = funcs[i]
    for s, e in f['ranges']:
        if s <= va <= e: return f
    return None

# --- disassemble each function range ---
asm = subprocess.run(['objdump', '-d', '-M', 'intel', '--no-show-raw-insn', exe],
                     capture_output=True, text=True).stdout
line_re = re.compile(r'^\s+([0-9a-f]+):\s+(.*)$')
insns = []
for line in asm.splitlines():
    m = line_re.match(line)
    if m: insns.append((int(m[1], 16), m[2].strip()))

for f in funcs:
    f.update(callees=set(), strings=set(), ports=set(), ints=set(), imports=set(), size=sum(e - s + 1 for s, e in f['ranges']))

# thunks that jump through the IAT
thunk_import = {}
for va, text in insns:
    m = re.match(r'jmp\s+DWORD PTR ds:0x([0-9a-f]+)', text)
    if m and int(m[1], 16) in iat:
        thunk_import[va] = iat[int(m[1], 16)]

last_dx = None
last_ax = None
for va, text in insns:
    f = owner(va)
    if not f: continue
    m = re.match(r'call\s+0x([0-9a-f]+)', text) or re.match(r'jmp\s+0x([0-9a-f]+)$', text)
    if m:
        t = int(m[1], 16)
        if t in thunk_import: f['imports'].add(thunk_import[t])
        elif t in by_addr and text.startswith('call'): f['callees'].add(t)
    m = re.match(r'call\s+DWORD PTR ds:0x([0-9a-f]+)', text)
    if m and int(m[1], 16) in iat: f['imports'].add(iat[int(m[1], 16)])
    for m in re.finditer(r'0x([0-9a-f]{6})\b', text):
        v = int(m[1], 16)
        if v in strings: f['strings'].add(strings[v])
    m = re.match(r'mov\s+(e?dx|dx),0x([0-9a-f]+)', text)
    if m: last_dx = int(m[2], 16)
    m = re.match(r'mov\s+(e?ax|ax|ah),0x([0-9a-f]+)', text)
    if m: last_ax = (m[1], int(m[2], 16))
    m = re.match(r'(in|out)\s+(.*)', text)
    if m and not text.startswith(('ins', 'outs')) or (m and text.startswith(('ins', 'outs'))):
        ops = m[2]
        mi = re.search(r'0x([0-9a-f]+)', ops)
        if mi and ('dx' not in ops.split(',')[0 if m[1] == 'out' else 1]):
            f['ports'].add(int(mi[1], 16))
        elif last_dx is not None:
            f['ports'].add(last_dx)
    m = re.match(r'int\s+0x([0-9a-f]+)', text)
    if m:
        n = int(m[1], 16)
        f['ints'].add('%02x' % n + ('/%s=%x' % last_ax if last_ax else ''))

callers = {f['addr']: set() for f in funcs}
for f in funcs:
    for c in f['callees']: callers[c].add(f['addr'])

res = []
for f in funcs:
    res.append({
        'addr': '%06x' % f['addr'], 'name': f['name'], 'size': f['size'],
        'callees': sorted('%06x' % c for c in f['callees']),
        'callers': sorted('%06x' % c for c in callers[f['addr']]),
        'strings': sorted(f['strings'])[:20],
        'ports': sorted('%x' % p for p in f['ports']),
        'ints': sorted(f['ints']),
        'imports': sorted(f['imports']),
    })
json.dump(res, open(out, 'w'), indent=0)
print(len(res), 'functions written to', out)
