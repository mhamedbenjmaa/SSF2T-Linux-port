// Runtime support for the recompiled code.
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Cpu R;
FLAGS G_FLAGS;
Fpu fpu = { .cw = 0x037F };

void divide_error(void) {
    fprintf(stderr, "sf2t: divide error (eax=%08x edx=%08x)\n", R.eax, R.edx);
    exit(1);
}

void unsupported(uint32_t addr, const char *what) {
    fprintf(stderr, "sf2t: unsupported code at %06x: %s\n", addr, what);
    exit(1);
}

// ---------------------------------------------------------- dispatch ----
static void (*find_func(uint32_t addr))(void) {
    int lo = 0, hi = g_functable_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_functable[mid].addr == addr) return g_functable[mid].fn;
        if (g_functable[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

void api_dispatch(uint32_t slot_value);   // platform/win32api.c

uint32_t g_rs[RS_MAX];
int g_rsp;

void call_indirect(uint32_t target) {
    // the return address is already pushed by the caller
    void (*fn)(void) = find_func(target);
    if (fn) { RS_PUSH(M32(R.esp)); fn(); return; }
    if ((target & 0xFFFF0000u) == 0xFFFF0000u) { api_dispatch(target); return; }
    fprintf(stderr, "sf2t: call to unknown address %08x\n", target);
    exit(1);
}

void jump_indirect(uint32_t target) {
    // a jump to another function behaves like a tail call
    void (*fn)(void) = find_func(target);
    if (fn) { fn(); return; }
    if ((target & 0xFFFF0000u) == 0xFFFF0000u) {
        // import thunk "jmp [IAT]": the API pops the caller's return address
        api_dispatch(target);
        g_rsp--;
        return;
    }
    fprintf(stderr, "sf2t: jump to unknown address %08x\n", target);
    exit(1);
}

void sys_int(int n);
void computed_return(uint32_t ra) {
    // The current function "returned" to ra; the real return address (of its
    // caller) is now on top of the stack. Run the code at ra, which ends with
    // its own ret to that caller.
    if (M8(ra) == 0xCD && M8(ra + 2) == 0xC3) {     // "int n; ret" stub
        G_FLAGS.cf = 0;
        sys_int(M8(ra + 1));
        R.esp += 4;          // its ret: the caller's entry was already popped
        return;
    }
    void (*fn)(void) = find_func(ra);
    if (fn) { g_rsp++; fn(); return; }
    fprintf(stderr, "sf2t: return to unexpected address %08x (expected %08x, depth %d, esp %08x)\n", ra, g_rsp >= 0 && g_rsp < RS_MAX ? g_rs[g_rsp] : 0, g_rsp, R.esp);
    for (int i = g_rsp; i >= 0 && i > g_rsp - 6; i--) fprintf(stderr, "  rs[%d]=%08x\n", i, g_rs[i]);
    exit(1);
}

// ---------------------------------------------------------- strings ----
static inline uint32_t rdn(uint32_t a, int sz) { return sz == 1 ? M8(a) : sz == 2 ? M16(a) : M32(a); }
static inline void wrn(uint32_t a, int sz, uint32_t v) {
    if (sz == 1) M8(a) = (uint8_t)v; else if (sz == 2) M16(a) = (uint16_t)v; else M32(a) = v;
}
static inline void acc_set(int sz, uint32_t v) {
    if (sz == 1) SET8L(R.eax, v); else if (sz == 2) SET16(R.eax, v); else R.eax = v;
}
static inline uint32_t acc_get(int sz) { return sz == 1 ? (uint8_t)R.eax : sz == 2 ? (uint16_t)R.eax : R.eax; }
static inline void cmp_sz(FLAGS *f, int sz, uint32_t a, uint32_t b) {
    if (sz == 1) sub8(f, a, b); else if (sz == 2) sub16(f, a, b); else sub32(f, a, b);
}

#define ESB seg_base(R.es)
#define DSB seg_base(R.ds)
void str_movs(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    uint32_t n = mode == ONCE ? 1 : R.ecx;
    uint32_t bytes = n * (uint32_t)sz, sb = DSB, db = ESB;
    uint32_t src = sb + R.esi, dst = db + R.edi;
    if (d > 0 && mode != ONCE && (dst <= src || dst >= src + bytes)) {
        // forward copy where a byte-by-byte copy and memmove give the same result
        memmove(g_mem + dst, g_mem + src, bytes);
        R.esi += bytes; R.edi += bytes;
    } else {
        for (uint32_t i = 0; i < n; i++) { wrn(db + R.edi, sz, rdn(sb + R.esi, sz)); R.esi += d; R.edi += d; }
    }
    if (mode != ONCE) R.ecx = 0;
}
void str_stos(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    uint32_t n = mode == ONCE ? 1 : R.ecx, v = acc_get(sz), db = ESB;
    if (sz == 1 && d > 0) { memset(g_mem + db + R.edi, (int)(uint8_t)v, n); R.edi += n; }
    else for (uint32_t i = 0; i < n; i++) { wrn(db + R.edi, sz, v); R.edi += d; }
    if (mode != ONCE) R.ecx = 0;
}
void str_lods(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    uint32_t n = mode == ONCE ? 1 : R.ecx;
    uint32_t sb = DSB;
    for (uint32_t i = 0; i < n; i++) { acc_set(sz, rdn(sb + R.esi, sz)); R.esi += d; }
    if (mode != ONCE) R.ecx = 0;
}
void str_scas(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    if (mode == ONCE) { cmp_sz(f, sz, acc_get(sz), rdn(ESB + R.edi, sz)); R.edi += d; return; }
    while (R.ecx) {
        cmp_sz(f, sz, acc_get(sz), rdn(ESB + R.edi, sz)); R.edi += d; R.ecx--;
        if (mode == REP && !f->zf) break;
        if (mode == REPNE && f->zf) break;
    }
}
void str_cmps(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    if (mode == ONCE) { cmp_sz(f, sz, rdn(DSB + R.esi, sz), rdn(ESB + R.edi, sz)); R.esi += d; R.edi += d; return; }
    while (R.ecx) {
        cmp_sz(f, sz, rdn(DSB + R.esi, sz), rdn(ESB + R.edi, sz)); R.esi += d; R.edi += d; R.ecx--;
        if (mode == REP && !f->zf) break;
        if (mode == REPNE && f->zf) break;
    }
}
void str_outs(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    uint32_t n = mode == ONCE ? 1 : R.ecx;
    for (uint32_t i = 0; i < n; i++) { port_out((uint16_t)R.edx, rdn(DSB + R.esi, sz), sz); R.esi += d; }
    if (mode != ONCE) R.ecx = 0;
}
void str_inss(FLAGS *f, int sz, int mode) {
    int d = f->df ? -sz : sz;
    uint32_t n = mode == ONCE ? 1 : R.ecx;
    for (uint32_t i = 0; i < n; i++) { wrn(ESB + R.edi, sz, port_in((uint16_t)R.edx, sz)); R.edi += d; }
    if (mode != ONCE) R.ecx = 0;
}

// --------------------------------------------------------------- x87 ----
double f80_load(uint32_t a) {
    uint64_t mant = M64(a);
    uint16_t se = M16(a + 8);
    int sign = se >> 15, exp = se & 0x7FFF;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
void f80_store(uint32_t a, double d) {
    if (d == 0) { M64(a) = 0; M16(a + 8) = signbit(d) ? 0x8000 : 0; return; }
    int sign = d < 0, e;
    double m = frexp(fabs(d), &e);                   // m in [0.5, 1)
    uint64_t mant = (uint64_t)ldexp(m, 64);
    M64(a) = mant;
    M16(a + 8) = (uint16_t)((sign ? 0x8000 : 0) | (uint16_t)(e - 1 + 16383));
}
int64_t fpu_to_int(double v, int bits) {
    double r;
    switch ((fpu.cw >> 10) & 3) {
    case 0: r = nearbyint(v); break;
    case 1: r = floor(v); break;
    case 2: r = ceil(v); break;
    default: r = trunc(v); break;
    }
    double lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return bits == 16 ? 0x8000 : bits == 32 ? (int64_t)0x80000000u : INT64_MIN;
    return (int64_t)r;
}
void fpu_compare(double a, double b) {
    fpu.sw &= ~0x4500;
    if (isnan(a) || isnan(b)) fpu.sw |= 0x4500;
    else if (a < b) fpu.sw |= 0x0100;
    else if (a == b) fpu.sw |= 0x4000;
}
void fpu_xam(void) {
    double v = FPU_ST(0);
    fpu.sw &= ~0x4700;
    if (signbit(v)) fpu.sw |= 0x0200;
    if (isnan(v)) fpu.sw |= 0x0100;
    else if (isinf(v)) fpu.sw |= 0x0500;
    else if (v == 0) fpu.sw |= 0x4000;
    else fpu.sw |= 0x0400;
}
uint16_t fpu_status(void) { return (uint16_t)((fpu.sw & ~0x3800) | ((fpu.top & 7) << 11)); }
void fpu_init(void) { fpu.top = 0; fpu.sw = 0; fpu.cw = 0x037F; }
void fpu_stenv(uint32_t a) {
    memset(g_mem + a, 0, 28);
    M16(a) = fpu.cw; M16(a + 4) = fpu_status(); M16(a + 8) = 0xFFFF;
}
