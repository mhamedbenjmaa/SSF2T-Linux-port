// Runtime support for the recompiled code (generated gen_*.c files).
// The original x86 instructions run on this CPU state, over the game memory.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "sf2t/mem.h"

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
} Cpu;
extern Cpu R;

typedef struct { uint8_t cf, zf, sf, of, df, pv; } FLAGS;
extern FLAGS G_FLAGS;
#define FLAGS_IN()  (F = G_FLAGS)
#define FLAGS_OUT() (G_FLAGS = F)

// ---- memory (unaligned, aliasing-safe) ----
typedef uint16_t __attribute__((aligned(1), may_alias)) u16_ua;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32_ua;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64_ua;
#define M8(a)  (*(uint8_t *)(g_mem + (uint32_t)(a)))
#define M16(a) (*(u16_ua *)(g_mem + (uint32_t)(a)))
#define M32(a) (*(u32_ua *)(g_mem + (uint32_t)(a)))
#define M64(a) (*(u64_ua *)(g_mem + (uint32_t)(a)))

#define SET16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))
#define SET8L(r, v) ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET8H(r, v) ((r) = ((r) & 0xFFFF00FFu) | ((uint32_t)(uint8_t)(v) << 8))

static inline void PUSH32(uint32_t v) { R.esp -= 4; M32(R.esp) = v; }
static inline void PUSH16(uint16_t v) { R.esp -= 2; M16(R.esp) = v; }
static inline uint32_t POP32(void) { uint32_t v = M32(R.esp); R.esp += 4; return v; }
static inline uint16_t POP16(void) { uint16_t v = M16(R.esp); R.esp += 2; return v; }
static inline void PUSHA(void) {
    uint32_t sp = R.esp;
    PUSH32(R.eax); PUSH32(R.ecx); PUSH32(R.edx); PUSH32(R.ebx);
    PUSH32(sp); PUSH32(R.ebp); PUSH32(R.esi); PUSH32(R.edi);
}
static inline void POPA(void) {
    R.edi = POP32(); R.esi = POP32(); R.ebp = POP32(); POP32();
    R.ebx = POP32(); R.edx = POP32(); R.ecx = POP32(); R.eax = POP32();
}

// segments: only selector bases matter (flat game selectors have base 0)
uint32_t seg_base(uint32_t sel);
#define SEGBASE(s) seg_base(R.s)
static inline void set_seg(uint32_t *s, uint16_t v) { *s = v; }

// ---- flags ----
static inline int parity(uint8_t v) { return !__builtin_parity(v); }
static inline uint32_t flags_get(FLAGS *f) {
    return (f->cf ? 1u : 0) | 2u | (parity(f->pv) ? 4u : 0) | (f->zf ? 0x40u : 0) | (f->sf ? 0x80u : 0) |
           0x200u | (f->df ? 0x400u : 0) | (f->of ? 0x800u : 0);
}
static inline void flags_set(FLAGS *f, uint32_t v) {
    f->cf = v & 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1; f->df = (v >> 10) & 1; f->of = (v >> 11) & 1;
    f->pv = (v & 4) ? 0 : 1;
}
static inline void sahf(FLAGS *f) {
    uint8_t ah = (uint8_t)(R.eax >> 8);
    f->cf = ah & 1; f->zf = (ah >> 6) & 1; f->sf = (ah >> 7) & 1; f->pv = (ah & 4) ? 0 : 1;
}
static inline void lahf(FLAGS *f) { SET8H(R.eax, flags_get(f) & 0xFF); }

#define DEF_ARITH(N, T, ST, MSB)                                                                  \
static inline uint32_t add##N(FLAGS *f, uint32_t a, uint32_t b) {                                  \
    T r = (T)((T)a + (T)b);                                                                       \
    f->cf = r < (T)a; f->zf = r == 0; f->sf = (r & MSB) != 0;                                    \
    f->of = ((~((T)a ^ (T)b) & ((T)a ^ r)) & MSB) != 0; f->pv = (uint8_t)r; return r; }         \
static inline uint32_t adc##N(FLAGS *f, uint32_t a, uint32_t b) {                                  \
    uint64_t w = (uint64_t)(T)a + (T)b + f->cf; T r = (T)w;                                      \
    f->cf = (w >> (sizeof(T) * 8)) != 0; f->zf = r == 0; f->sf = (r & MSB) != 0;                \
    f->of = ((~((T)a ^ (T)b) & ((T)a ^ r)) & MSB) != 0; f->pv = (uint8_t)r; return r; }         \
static inline uint32_t sub##N(FLAGS *f, uint32_t a, uint32_t b) {                                  \
    T r = (T)((T)a - (T)b);                                                                       \
    f->cf = (T)a < (T)b; f->zf = r == 0; f->sf = (r & MSB) != 0;                                 \
    f->of = ((((T)a ^ (T)b) & ((T)a ^ r)) & MSB) != 0; f->pv = (uint8_t)r; return r; }          \
static inline uint32_t sbb##N(FLAGS *f, uint32_t a, uint32_t b) {                                  \
    uint64_t bb = (uint64_t)(T)b + f->cf; T r = (T)((T)a - (T)bb);                               \
    f->cf = (uint64_t)(T)a < bb; f->zf = r == 0; f->sf = (r & MSB) != 0;                        \
    f->of = ((((T)a ^ (T)b) & ((T)a ^ r)) & MSB) != 0; f->pv = (uint8_t)r; return r; }          \
static inline uint32_t and##N(FLAGS *f, uint32_t a, uint32_t b) {                                  \
    T r = (T)a & (T)b; f->cf = f->of = 0; f->zf = r == 0; f->sf = (r & MSB) != 0;                \
    f->pv = (uint8_t)r; return r; }                                                               \
static inline uint32_t or##N(FLAGS *f, uint32_t a, uint32_t b) {                                   \
    T r = (T)a | (T)b; f->cf = f->of = 0; f->zf = r == 0; f->sf = (r & MSB) != 0;                \
    f->pv = (uint8_t)r; return r; }                                                               \
static inline uint32_t xor##N(FLAGS *f, uint32_t a, uint32_t b) {                                  \
    T r = (T)a ^ (T)b; f->cf = f->of = 0; f->zf = r == 0; f->sf = (r & MSB) != 0;                \
    f->pv = (uint8_t)r; return r; }                                                               \
static inline uint32_t inc##N(FLAGS *f, uint32_t a) {                                              \
    T r = (T)((T)a + 1); f->zf = r == 0; f->sf = (r & MSB) != 0; f->of = r == MSB;               \
    f->pv = (uint8_t)r; return r; }                                                               \
static inline uint32_t dec##N(FLAGS *f, uint32_t a) {                                              \
    T r = (T)((T)a - 1); f->zf = r == 0; f->sf = (r & MSB) != 0; f->of = (T)a == MSB;            \
    f->pv = (uint8_t)r; return r; }                                                               \
static inline uint32_t shl##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    if (!c) return (T)a; T r = c >= sizeof(T) * 8 ? 0 : (T)((T)a << c);                          \
    f->cf = c <= sizeof(T) * 8 ? (((T)a >> (sizeof(T) * 8 - c)) & 1) : 0;                         \
    f->zf = r == 0; f->sf = (r & MSB) != 0; f->of = ((r & MSB) != 0) ^ f->cf; f->pv = (uint8_t)r;\
    return r; }                                                                                   \
static inline uint32_t shr##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    if (!c) return (T)a; T r = c >= sizeof(T) * 8 ? 0 : (T)((T)a >> c);                          \
    f->cf = c <= sizeof(T) * 8 ? (((T)a >> (c - 1)) & 1) : 0;                                     \
    f->zf = r == 0; f->sf = (r & MSB) != 0; f->of = ((T)a & MSB) != 0; f->pv = (uint8_t)r;        \
    return r; }                                                                                   \
static inline uint32_t sar##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    if (!c) return (T)a; ST s = (ST)a; unsigned k = c >= sizeof(T) * 8 ? sizeof(T) * 8 - 1 : c;   \
    T r = (T)(s >> k); f->cf = (s >> (k - 1 < sizeof(T) * 8 ? k - 1 : 0)) & 1;                     \
    if (c >= sizeof(T) * 8) f->cf = s < 0;                                                        \
    f->zf = r == 0; f->sf = (r & MSB) != 0; f->of = 0; f->pv = (uint8_t)r; return r; }           \
static inline uint32_t rol##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    unsigned b = sizeof(T) * 8; c %= b; if (!c) return (T)a;                                      \
    T r = (T)(((T)a << c) | ((T)a >> (b - c))); f->cf = r & 1;                                   \
    f->of = ((r & MSB) != 0) ^ f->cf; return r; }                                                 \
static inline uint32_t ror##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    unsigned b = sizeof(T) * 8; c %= b; if (!c) return (T)a;                                      \
    T r = (T)(((T)a >> c) | ((T)a << (b - c))); f->cf = (r & MSB) != 0;                          \
    f->of = ((r ^ (T)(r << 1)) & MSB) != 0; return r; }                                           \
static inline uint32_t rcl##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    T r = (T)a; for (uint32_t i = 0; i < c; i++) { uint8_t o = (r & MSB) != 0;                    \
    r = (T)((T)(r << 1) | f->cf); f->cf = o; } if (c) f->of = ((r & MSB) != 0) ^ f->cf; return r; }\
static inline uint32_t rcr##N(FLAGS *f, uint32_t a, uint32_t c) {                                  \
    T r = (T)a; for (uint32_t i = 0; i < c; i++) { uint8_t o = r & 1;                             \
    r = (T)((r >> 1) | (f->cf ? MSB : 0)); f->cf = o; } return r; }                                \
static inline uint32_t imul##N(FLAGS *f, uint32_t a, uint32_t b) {                                 \
    int64_t w = (int64_t)(ST)a * (ST)b; T r = (T)w;                                               \
    f->cf = f->of = (w != (int64_t)(ST)r); return r; }

DEF_ARITH(8, uint8_t, int8_t, 0x80u)
DEF_ARITH(16, uint16_t, int16_t, 0x8000u)
DEF_ARITH(32, uint32_t, int32_t, 0x80000000u)

static inline void imul1_8(FLAGS *f, uint32_t b) {
    int16_t w = (int16_t)(int8_t)R.eax * (int8_t)b; SET16(R.eax, w); f->cf = f->of = w != (int8_t)w; }
static inline void imul1_16(FLAGS *f, uint32_t b) {
    int32_t w = (int32_t)(int16_t)R.eax * (int16_t)b; SET16(R.eax, w); SET16(R.edx, w >> 16);
    f->cf = f->of = w != (int16_t)w; }
static inline void imul1_32(FLAGS *f, uint32_t b) {
    int64_t w = (int64_t)(int32_t)R.eax * (int32_t)b; R.eax = (uint32_t)w; R.edx = (uint32_t)(w >> 32);
    f->cf = f->of = w != (int32_t)w; }
static inline void mul1_8(FLAGS *f, uint32_t b) {
    uint16_t w = (uint16_t)((uint8_t)R.eax * (uint8_t)b); SET16(R.eax, w); f->cf = f->of = (w >> 8) != 0; }
static inline void mul1_16(FLAGS *f, uint32_t b) {
    uint32_t w = (uint32_t)(uint16_t)R.eax * (uint16_t)b; SET16(R.eax, w); SET16(R.edx, w >> 16);
    f->cf = f->of = (w >> 16) != 0; }
static inline void mul1_32(FLAGS *f, uint32_t b) {
    uint64_t w = (uint64_t)R.eax * b; R.eax = (uint32_t)w; R.edx = (uint32_t)(w >> 32);
    f->cf = f->of = (w >> 32) != 0; }

void divide_error(void);
static inline void idiv8(uint32_t b) {
    int16_t a = (int16_t)R.eax; int8_t d = (int8_t)b; if (!d) divide_error();
    SET8L(R.eax, a / d); SET8H(R.eax, a % d); }
static inline void idiv16(uint32_t b) {
    int32_t a = (int32_t)(((uint32_t)(uint16_t)R.edx << 16) | (uint16_t)R.eax); int16_t d = (int16_t)b;
    if (!d) divide_error(); SET16(R.eax, a / d); SET16(R.edx, a % d); }
static inline void idiv32(uint32_t b) {
    int64_t a = (int64_t)(((uint64_t)R.edx << 32) | R.eax); int32_t d = (int32_t)b;
    if (!d) divide_error(); R.eax = (uint32_t)(int32_t)(a / d); R.edx = (uint32_t)(int32_t)(a % d); }
static inline void div8(uint32_t b) {
    uint16_t a = (uint16_t)R.eax; uint8_t d = (uint8_t)b; if (!d) divide_error();
    SET8L(R.eax, a / d); SET8H(R.eax, a % d); }
static inline void div16(uint32_t b) {
    uint32_t a = ((uint32_t)(uint16_t)R.edx << 16) | (uint16_t)R.eax; uint16_t d = (uint16_t)b;
    if (!d) divide_error(); SET16(R.eax, a / d); SET16(R.edx, a % d); }
static inline void div32(uint32_t b) {
    uint64_t a = ((uint64_t)R.edx << 32) | R.eax; uint32_t d = b;
    if (!d) divide_error(); R.eax = (uint32_t)(a / d); R.edx = (uint32_t)(a % d); }

// ---- string instructions (flat ES/DS) ----
enum { ONCE, REP, REPNE };
void str_movs(FLAGS *f, int sz, int mode);
void str_stos(FLAGS *f, int sz, int mode);
void str_lods(FLAGS *f, int sz, int mode);
void str_scas(FLAGS *f, int sz, int mode);
void str_cmps(FLAGS *f, int sz, int mode);
void str_outs(FLAGS *f, int sz, int mode);
void str_inss(FLAGS *f, int sz, int mode);
#define str_insb str_inss
#define str_insw str_inss
#define str_insd str_inss

// ---- x87 ----
typedef struct { double st[8]; int top; uint16_t sw, cw; } Fpu;
extern Fpu fpu;
#define FPU_ST(i) (fpu.st[(fpu.top + (i)) & 7])
static inline void fpu_push(double v) { fpu.top = (fpu.top - 1) & 7; fpu.st[fpu.top] = v; }
static inline void fpu_pop(void) { fpu.top = (fpu.top + 1) & 7; }
static inline double f32_load(uint32_t a) { float v; uint32_t u = M32(a); __builtin_memcpy(&v, &u, 4); return v; }
static inline double f64_load(uint32_t a) { double v; uint64_t u = M64(a); __builtin_memcpy(&v, &u, 8); return v; }
static inline void f32_store(uint32_t a, double d) { float v = (float)d; uint32_t u; __builtin_memcpy(&u, &v, 4); M32(a) = u; }
static inline void f64_store(uint32_t a, double d) { uint64_t u; __builtin_memcpy(&u, &d, 8); M64(a) = u; }
double f80_load(uint32_t a);
void f80_store(uint32_t a, double d);
int64_t fpu_to_int(double v, int bits);
void fpu_compare(double a, double b);
void fpu_xam(void);
uint16_t fpu_status(void);
void fpu_init(void);
void fpu_stenv(uint32_t a);

// ---- system ----
uint32_t port_in(uint16_t port, int size);
void port_out(uint16_t port, uint32_t val, int size);
void sys_int(int n);
void cpu_cli(void);
void cpu_sti(void);
void cpu_hlt(void);
void call_indirect(uint32_t target);
void jump_indirect(uint32_t target);
void call_far(uint32_t ptr_addr);
void unsupported(uint32_t addr, const char *what);

// Return-address shadow stack: a "ret" normally returns to the address its
// caller pushed. When code replaced that address (computed jumps through
// "ret"), the CPU would continue there instead: computed_return does that.
extern uint32_t g_rs[];
extern int g_rsp;
#define RS_MAX 8192
static inline void RS_PUSH(uint32_t a) { if (g_rsp < RS_MAX) g_rs[g_rsp] = a; g_rsp++; }
void computed_return(uint32_t ra);
#define RET(n) do { uint32_t ra_ = M32(R.esp); R.esp += 4u + (n);                     \
    g_rsp--; if (g_rsp >= 0 && g_rsp < RS_MAX && g_rs[g_rsp] != ra_) computed_return(ra_); } while (0)
// hand-written replacements end with this instead of the original "ret n"
#define HW_RET(n) do { R.esp += 4u + (n); g_rsp--; } while (0)

// interrupt check at loop back-edges (cheap test, rare work)
extern volatile uint32_t g_irq_tick;
extern uint32_t g_irq_seen;
void irq_poll_slow(void);
#define IRQ_POLL() do { if (g_irq_tick != g_irq_seen) irq_poll_slow(); } while (0)

typedef struct { uint32_t addr; void (*fn)(void); } FuncEntry;
extern const FuncEntry g_functable[];
extern const int g_functable_n;

#include "funcs.h"
#include "apis.h"
