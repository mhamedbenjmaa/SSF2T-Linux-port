// Game memory access for the rewritten code.
//
// The game's data keeps its original layout, with 32-bit addresses inside
// structures. Rewritten code addresses it through `gaddr` values relative
// to g_mem. In the test harness g_mem is the base of the running original
// game, so original and rewritten functions share the same memory.
#pragma once
#include <stdint.h>
#include <string.h>

typedef uint32_t gaddr;

extern uint8_t *g_mem;

static inline uint8_t  rd_u8(gaddr a)  { return g_mem[a]; }
static inline int8_t   rd_s8(gaddr a)  { return (int8_t)g_mem[a]; }
static inline uint16_t rd_u16(gaddr a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline int16_t  rd_s16(gaddr a) { int16_t v;  memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t rd_u32(gaddr a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline int32_t  rd_s32(gaddr a) { int32_t v;  memcpy(&v, g_mem + a, 4); return v; }

static inline void wr_u8(gaddr a, uint8_t v)   { g_mem[a] = v; }
static inline void wr_u16(gaddr a, uint16_t v) { memcpy(g_mem + a, &v, 2); }
static inline void wr_s16(gaddr a, int16_t v)  { memcpy(g_mem + a, &v, 2); }
static inline void wr_u32(gaddr a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
static inline void wr_s32(gaddr a, int32_t v)  { memcpy(g_mem + a, &v, 4); }
