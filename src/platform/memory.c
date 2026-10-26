// Game address space and its allocators: extended memory (DPMI and
// VirtualAlloc) above 4 MB, DOS conventional memory below 640 KB.
#include "platform.h"
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

uint8_t *g_mem;

#define HEAP_START 0x00400000u
#define DOS_START  0x00020000u
#define DOS_END    0x0009F000u

typedef struct { uint32_t addr, size; bool used; } Block;
#define MAX_BLOCKS 4096
static Block blocks[MAX_BLOCKS];
static int nblocks;

void mem_init(void) {
#ifdef _WIN32
    g_mem = VirtualAlloc(NULL, GAME_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    g_mem = mmap(NULL, GAME_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem == MAP_FAILED) g_mem = NULL;
#endif
    if (!g_mem) die("cannot reserve game memory");
    blocks[0] = (Block){ HEAP_START, GAME_SIZE - HEAP_START, false };
    nblocks = 1;
}

uint32_t mem_alloc(uint32_t size, uint32_t align) {
    if (size == 0) size = 1;
    size = (size + 0xFFF) & ~0xFFFu;
    if (align < 0x1000) align = 0x1000;
    for (int i = 0; i < nblocks; i++) {
        Block *b = &blocks[i];
        if (b->used) continue;
        uint32_t start = (b->addr + align - 1) & ~(align - 1);
        uint32_t pad = start - b->addr;
        if (b->size < pad + size) continue;
        if (nblocks + 2 >= MAX_BLOCKS) return 0;
        uint32_t rest = b->size - pad - size;
        int at = i;
        if (pad) {
            memmove(&blocks[at + 1], &blocks[at], (nblocks - at) * sizeof(Block));
            nblocks++;
            blocks[at] = (Block){ b->addr, pad, false };
            at++;
        }
        blocks[at] = (Block){ start, size, true };
        if (rest) {
            memmove(&blocks[at + 2], &blocks[at + 1], (nblocks - at - 1) * sizeof(Block));
            nblocks++;
            blocks[at + 1] = (Block){ start + size, rest, false };
        }
        memset(g_mem + start, 0, size);
        return start;
    }
    return 0;
}

void mem_free(uint32_t addr) {
    for (int i = 0; i < nblocks; i++) {
        if (blocks[i].addr != addr || !blocks[i].used) continue;
        blocks[i].used = false;
        if (i + 1 < nblocks && !blocks[i + 1].used) {
            blocks[i].size += blocks[i + 1].size;
            memmove(&blocks[i + 1], &blocks[i + 2], (nblocks - i - 2) * sizeof(Block));
            nblocks--;
        }
        if (i > 0 && !blocks[i - 1].used) {
            blocks[i - 1].size += blocks[i].size;
            memmove(&blocks[i], &blocks[i + 1], (nblocks - i - 1) * sizeof(Block));
            nblocks--;
        }
        return;
    }
}

uint32_t mem_largest_free(void) {
    uint32_t best = 0;
    for (int i = 0; i < nblocks; i++)
        if (!blocks[i].used && blocks[i].size > best) best = blocks[i].size;
    return best;
}

typedef struct { uint16_t seg, paras; } DosBlk;
static DosBlk dos[64];
static int ndos;

uint32_t dosmem_alloc(uint32_t paras, uint32_t *max_avail) {
    uint32_t cur = DOS_START >> 4, best_gap = 0;
    for (int i = 0; i <= ndos; i++) {
        uint32_t next = i < ndos ? dos[i].seg : DOS_END >> 4;
        uint32_t gap = next - cur;
        if (gap > best_gap) best_gap = gap;
        if (gap >= paras && paras && ndos < 64) {
            memmove(&dos[i + 1], &dos[i], (ndos - i) * sizeof(DosBlk));
            dos[i] = (DosBlk){ (uint16_t)cur, (uint16_t)paras };
            ndos++;
            memset(g_mem + (cur << 4), 0, paras << 4);
            return cur;
        }
        if (i < ndos) cur = dos[i].seg + dos[i].paras;
    }
    if (max_avail) *max_avail = best_gap;
    return 0;
}

void dosmem_free(uint16_t seg) {
    for (int i = 0; i < ndos; i++)
        if (dos[i].seg == seg) {
            memmove(&dos[i], &dos[i + 1], (ndos - i - 1) * sizeof(DosBlk));
            ndos--;
            return;
        }
}
