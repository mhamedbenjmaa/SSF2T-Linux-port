// Descriptors (LDT emulation), hardware interrupt delivery, and the software
// interrupt services the game used: DPMI (int 31h), DOS (int 21h), multiplex
// (int 2Fh incl. MSCDEX), video BIOS (int 10h) and the real-mode calls made
// through DPMI 0300h.
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint16_t sel_code, sel_data, sel_tib;

// ---------------------------------------------------------------- LDT --
#define NLDT 512
typedef struct { bool used; uint32_t base, limit; } LdtEnt;
static LdtEnt ldt[NLDT];
static inline int sidx(uint16_t s) { return s >> 3; }
static inline uint16_t isel(int i) { return (uint16_t)((i << 3) | 7); }
static bool valid(uint16_t s) { int i = sidx(s); return (s & 4) && i > 0 && i < NLDT && ldt[i].used; }

uint16_t ldt_alloc(int count) {
    for (int i = 1; i + count <= NLDT; i++) {
        int k;
        for (k = 0; k < count && !ldt[i + k].used; k++) {}
        if (k == count) {
            for (k = 0; k < count; k++) ldt[i + k] = (LdtEnt){ true, 0, 0 };
            return isel(i);
        }
    }
    return 0;
}
void ldt_free(uint16_t s) { if (valid(s)) ldt[sidx(s)].used = false; }
bool ldt_set_base(uint16_t s, uint32_t b) { if (!valid(s)) return false; ldt[sidx(s)].base = b; return true; }
bool ldt_set_limit(uint16_t s, uint32_t l) { if (!valid(s)) return false; ldt[sidx(s)].limit = l; return true; }
uint32_t ldt_get_base(uint16_t s) { return valid(s) ? ldt[sidx(s)].base : 0; }
uint32_t seg_base(uint32_t sel) { return ldt_get_base((uint16_t)sel); }

// ------------------------------------------------- hardware interrupts --
static uint32_t pm_vec_off[256];    // 0 = default (no handler)
static uint32_t rm_vec[256];
bool g_vif = true;
static uint32_t pending, in_service;
volatile uint32_t g_irq_tick;
uint32_t g_irq_seen;

#define VEC_STUB(v) (THUNK_BASE + 0x400 + (v) * 4)

void irq_raise(int irq) { pending |= 1u << irq; }

static void (*handler_for(uint32_t off))(void) {
    int lo = 0, hi = g_functable_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_functable[mid].addr == off) return g_functable[mid].fn;
        if (g_functable[mid].addr < off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void deliver_pending(void) {
    for (int irq = 0; irq < 16 && g_vif; irq++) {
        uint32_t bit = 1u << irq;
        if (!(pending & bit) || (in_service & bit)) continue;
        pending &= ~bit;
        int v = irq < 8 ? 8 + irq : 0x70 + irq - 8;
        void (*fn)(void) = pm_vec_off[v] ? handler_for(pm_vec_off[v]) : NULL;
        if (!fn) continue;
        // Like the DPMI host: the handler runs with an IRET frame on the
        // stack, and the whole register state is restored afterwards.
        Cpu saved = R;
        FLAGS sflags = G_FLAGS;
        bool svif = g_vif;
        PUSH32(flags_get(&G_FLAGS));
        PUSH32(R.cs);
        PUSH32(0xFFFFFFF0u);
        g_vif = false;
        in_service |= bit;
        fn();
        in_service &= ~bit;
        R = saved;
        G_FLAGS = sflags;
        g_vif = svif;
    }
}

void irq_poll_slow(void) {
    // debug: SSF2T_STALLS=1 reports gaps of more than 20 ms between two
    // service passes (the game would look frozen)
    static int stalls = -1;
    static uint64_t last;
    if (stalls < 0) stalls = getenv("SSF2T_STALLS") != NULL;
    uint64_t now = now_ns();
    if (stalls && last && now - last > 20000000ull)
        fprintf(stderr, "STALL %.1f ms (heartbeat %u->%u, esp %08x)\n", (now - last) / 1e6, g_irq_seen, g_irq_tick, R.esp);
    last = now;
    g_irq_seen = g_irq_tick;
    io_tick();
    deliver_pending();
}

void cpu_cli(void) { g_vif = false; }
void cpu_sti(void) { g_vif = true; IRQ_POLL(); }
void cpu_hlt(void) {
    uint32_t t = g_irq_tick;
    while (g_irq_tick == t) sleep_ns(200000);
    irq_poll_slow();
}

// ------------------------------------------------------------- MSCDEX --
#define CD_DRIVE 3
static uint32_t rb_from_lba(uint32_t lba) {
    lba += 150;
    return (lba / 4500) << 16 | ((lba / 75) % 60) << 8 | (lba % 75);
}
static uint32_t lba_from_rb(uint32_t rb) {
    uint32_t v = ((rb >> 16) & 0xFF) * 4500 + ((rb >> 8) & 0xFF) * 75 + (rb & 0xFF);
    return v >= 150 ? v - 150 : 0;
}
static uint32_t cd_play_start, cd_play_end;
static bool cd_paused;

static void mscdex_request(uint32_t req) {
    uint8_t cmd = rd8(req + 2);
    uint16_t status = 0x0100;
    switch (cmd) {
    case 3: {
        uint32_t fp = rd32(req + 14);
        uint32_t buf = ((fp >> 16) << 4) + (fp & 0xFFFF);
        switch (rd8(buf)) {
        case 0: wr32(buf + 1, 0); break;
        case 1: wr8(buf + 1, 0); wr32(buf + 2, cdaudio_playing() ? cdaudio_position() : 0); break;
        case 4: for (int i = 0; i < 4; i++) { wr8(buf + 1 + i * 2, (uint8_t)i); wr8(buf + 2 + i * 2, 0xFF); } break;
        case 6: wr32(buf + 1, 0x0310 | (fs_cd_present() ? 0 : 0x800)); break;
        case 7: wr8(buf + 1, 0); wr16(buf + 2, 2048); break;
        case 8: wr32(buf + 1, cd_leadout()); break;
        case 9: wr8(buf + 1, 1); break;
        case 10: wr8(buf + 1, 1); wr8(buf + 2, (uint8_t)cd_track_count()); wr32(buf + 3, rb_from_lba(cd_leadout())); break;
        case 11: {
            uint32_t st, len; bool audio;
            if (cd_track_info(rd8(buf + 1), &st, &len, &audio)) { wr32(buf + 2, rb_from_lba(st)); wr8(buf + 6, audio ? 0 : 0x40); }
            else status = 0x8000 | 8;
            break;
        }
        case 12: {
            uint32_t pos = cdaudio_playing() ? cdaudio_position() : cd_play_start;
            int track = 1;
            for (int t = 1; t <= cd_track_count(); t++) { uint32_t st, len; bool a; cd_track_info(t, &st, &len, &a); if (pos >= st) track = t; }
            uint32_t st, len; bool a;
            cd_track_info(track, &st, &len, &a);
            uint32_t r = pos - st, abs = pos + 150;
            wr8(buf + 1, a ? 0x01 : 0x41);
            wr8(buf + 2, (uint8_t)((track / 10) << 4 | (track % 10)));
            wr8(buf + 3, 1);
            wr8(buf + 4, (uint8_t)(r / 4500)); wr8(buf + 5, (uint8_t)((r / 75) % 60)); wr8(buf + 6, (uint8_t)(r % 75));
            wr8(buf + 7, 0);
            wr8(buf + 8, (uint8_t)(abs / 4500)); wr8(buf + 9, (uint8_t)((abs / 75) % 60)); wr8(buf + 10, (uint8_t)(abs % 75));
            break;
        }
        case 15:
            wr16(buf + 1, cd_paused ? 1 : 0);
            wr32(buf + 3, rb_from_lba(cd_play_start));
            wr32(buf + 7, rb_from_lba(cd_play_end));
            break;
        default: break;
        }
        break;
    }
    case 132: {
        uint8_t mode = rd8(req + 13);
        uint32_t start = rd32(req + 14), n = rd32(req + 18);
        uint32_t lba = mode == 1 ? lba_from_rb(start) : start;
        cd_play_start = lba; cd_play_end = lba + n; cd_paused = false;
        LOG("CD play lba %u, %u frames\n", lba, n);
        cdaudio_play(lba, n);
        break;
    }
    case 133:
        if (cdaudio_playing()) { cdaudio_stop(); cd_paused = true; } else cd_paused = false;
        break;
    case 136:
        if (cd_paused) { cdaudio_resume(); cd_paused = false; }
        break;
    default: break;
    }
    if (cdaudio_playing()) status |= 0x0200;
    wr16(req + 3, status);
}

static void mscdex(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t es_base, bool *cf) {
    *cf = false;
    switch (*eax & 0xFF) {
    case 0x00: *ebx = (*ebx & ~0xFFFFu) | 1; *ecx = (*ecx & ~0xFFFFu) | CD_DRIVE; break;
    case 0x0B: *eax = (*eax & ~0xFFFFu) | ((*ecx & 0xFFFF) == CD_DRIVE ? 0x5AD8 : 0); *ebx = (*ebx & ~0xFFFFu) | 0xADAD; break;
    case 0x0C: *ebx = (*ebx & ~0xFFFFu) | 0x0215; break;
    case 0x0D: wr8(es_base + (*ebx & 0xFFFF), CD_DRIVE); break;
    case 0x10: mscdex_request(es_base + (*ebx & 0xFFFF)); break;
    default: break;
    }
}

// ---------------------------------------------------------- video BIOS --
static void bios_int10(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx, uint32_t es_base) {
    uint8_t ah = (*eax >> 8) & 0xFF, al = *eax & 0xFF;
    switch (ah) {
    case 0x00: video_set_mode(al & 0x7F); break;
    case 0x0F: *eax = (*eax & ~0xFFFFu) | (g_video_mode == 3 ? 0x5003 : 0x2800 | (uint8_t)g_video_mode); *ebx &= ~0xFF00u; break;
    case 0x10:
        if (al == 0x12) {
            uint32_t first = *ebx & 0xFFFF, n = *ecx & 0xFFFF, src = es_base + (*edx & 0xFFFF);
            for (uint32_t i = 0; i < n * 3 && first * 3 + i < 768; i++) g_vga_pal[first * 3 + i] = rd8(src + i) & 0x3F;
        } else if (al == 0x17) {
            uint32_t first = *ebx & 0xFFFF, n = *ecx & 0xFFFF, dst = es_base + (*edx & 0xFFFF);
            for (uint32_t i = 0; i < n * 3 && first * 3 + i < 768; i++) wr8(dst + i, g_vga_pal[first * 3 + i]);
        }
        break;
    case 0x12: if ((*ebx & 0xFF) == 0x10) *ebx = (*ebx & ~0xFFFFu) | 3; break;
    case 0x1A: if (al == 0) { *eax = (*eax & ~0xFFu) | 0x1A; *ebx = (*ebx & ~0xFFFFu) | 8; } break;
    case 0x4F: *eax = (*eax & ~0xFFFFu) | 0x014F; break;
    default: break;
    }
}

static void real_mode_int(int n, uint32_t rs) {
    if (g_verbose > 1) logf_("  real-mode int %02x ax=%04x\n", n, rd32(rs + 28) & 0xFFFF);
    uint32_t ebx = rd32(rs + 16), edx = rd32(rs + 20), ecx = rd32(rs + 24), eax = rd32(rs + 28);
    uint16_t flags = rd16(rs + 32), es = rd16(rs + 34);
    uint32_t es_base = (uint32_t)es << 4;
    bool cf = false;
    switch (n) {
    case 0x10: bios_int10(&eax, &ebx, &ecx, &edx, es_base); break;
    case 0x2F:
        if (((eax >> 8) & 0xFF) == 0x15) mscdex(&eax, &ebx, &ecx, es_base, &cf);
        else if ((eax & 0xFFFF) == 0x1600) eax &= ~0xFFu;
        break;
    case 0x33: eax &= ~0xFFFFu; break;
    case 0x16: flags |= 0x40; break;
    default: LOG("real-mode int %02xh ax=%04x\n", n, eax & 0xFFFF); break;
    }
    if (cf) flags |= 1; else flags &= ~1;
    wr32(rs + 16, ebx); wr32(rs + 20, edx); wr32(rs + 24, ecx); wr32(rs + 28, eax);
    wr16(rs + 32, flags);
}

// --------------------------------------------------------------- DPMI --
static uint32_t sel_base_flat(uint16_t s) { return (s == sel_data || s == sel_code) ? 0 : ldt_get_base(s); }

static void dpmi(void) {
    uint16_t ax = (uint16_t)R.eax, bx = (uint16_t)R.ebx;
    uint32_t cx = R.ecx & 0xFFFF, dx = R.edx & 0xFFFF;
    set_cf(false);
    switch (ax) {
    case 0x0000: { uint16_t s = ldt_alloc(cx ? cx : 1); if (!s) { set_cf(true); set_ax(0x8011); } else set_ax(s); break; }
    case 0x0001: ldt_free(bx); break;
    case 0x0002: { uint16_t s = ldt_alloc(1); ldt_set_base(s, (uint32_t)bx << 4); ldt_set_limit(s, 0xFFFF); set_ax(s); break; }
    case 0x0003: set_ax(8); break;
    case 0x0006: { uint32_t b = sel_base_flat(bx); SET16(R.ecx, b >> 16); SET16(R.edx, b); break; }
    case 0x0007: if (!ldt_set_base(bx, cx << 16 | dx)) set_cf(true); break;
    case 0x0008: if (!ldt_set_limit(bx, cx << 16 | dx)) set_cf(true); break;
    case 0x0009: break;
    case 0x000A: { uint16_t s = ldt_alloc(1); ldt_set_base(s, sel_base_flat(bx)); set_ax(s); break; }
    case 0x0100: {
        uint32_t maxp = 0, seg = dosmem_alloc(bx, &maxp);
        if (!seg) { set_cf(true); set_ax(8); SET16(R.ebx, maxp); break; }
        uint16_t s = ldt_alloc(1);
        ldt_set_base(s, seg << 4); ldt_set_limit(s, ((uint32_t)bx << 4) - 1);
        set_ax((uint16_t)seg); SET16(R.edx, s);
        break;
    }
    case 0x0101: dosmem_free((uint16_t)(sel_base_flat((uint16_t)dx) >> 4)); ldt_free((uint16_t)dx); break;
    case 0x0102: set_cf(true); set_ax(8); break;
    case 0x0200: { uint32_t v = rm_vec[bx & 0xFF]; SET16(R.ecx, v >> 16); SET16(R.edx, v); break; }
    case 0x0201: rm_vec[bx & 0xFF] = cx << 16 | dx; break;
    case 0x0204: {
        int v = bx & 0xFF;
        SET16(R.ecx, sel_code);
        R.edx = pm_vec_off[v] ? pm_vec_off[v] : VEC_STUB(v);
        break;
    }
    case 0x0205: {
        int v = bx & 0xFF;
        pm_vec_off[v] = R.edx == VEC_STUB(v) ? 0 : R.edx;
        LOG("set PM vector %02x = %08x\n", v, R.edx);
        break;
    }
    case 0x0300: real_mode_int(bx & 0xFF, sel_base_flat((uint16_t)R.es) + R.edi); break;
    case 0x0400: set_ax(0x005A); SET16(R.ebx, 3); SET8L(R.ecx, 4); SET16(R.edx, 0x0870); break;
    case 0x0500: {
        uint32_t p = sel_base_flat((uint16_t)R.es) + R.edi, big = mem_largest_free();
        memset(G(p), 0xFF, 0x30);
        wr32(p, big); wr32(p + 4, big / 4096); wr32(p + 8, big / 4096); wr32(p + 0x0C, GAME_SIZE / 4096);
        wr32(p + 0x10, big / 4096); wr32(p + 0x14, big / 4096); wr32(p + 0x18, GAME_SIZE / 4096);
        wr32(p + 0x1C, big / 4096); wr32(p + 0x20, 0);
        break;
    }
    case 0x0501: {
        uint32_t a = mem_alloc((uint32_t)bx << 16 | cx, 0x1000);
        if (!a) { set_cf(true); set_ax(0x8013); break; }
        SET16(R.ebx, a >> 16); SET16(R.ecx, a); SET16(R.esi, a >> 16); SET16(R.edi, a);
        break;
    }
    case 0x0502: mem_free((R.esi & 0xFFFF) << 16 | (R.edi & 0xFFFF)); break;
    case 0x0506: { uint32_t p = sel_base_flat((uint16_t)R.es) + R.edx; for (uint32_t i = 0; i < R.ecx && i < 65536; i++) wr16(p + i * 2, 9); break; }
    case 0x0507: case 0x0600: case 0x0601: case 0x0602: case 0x0603: case 0x0702: case 0x0703: case 0x0801: break;
    case 0x0800: if (((uint32_t)bx << 16 | cx) >= 0x100000) { set_cf(true); set_ax(0x8021); } break;
    case 0x0900: set_al(g_vif); g_vif = false; break;
    case 0x0901: set_al(g_vif); g_vif = true; break;
    case 0x0902: set_al(g_vif); break;
    default: LOG("DPMI %04x unsupported\n", ax); set_cf(true); set_ax(0x8001); break;
    }
}

// ------------------------------------------------------------ int 21h --
static uint16_t psp_sel;

static void dos21(void) {
    uint8_t ah = (R.eax >> 8) & 0xFF;
    set_cf(false);
    switch (ah) {
    case 0x02: putchar((uint8_t)R.edx); fflush(stdout); break;
    case 0x09: for (uint32_t p = R.edx; rd8(p) != '$'; p++) putchar(rd8(p)); fflush(stdout); break;
    case 0x25: pm_vec_off[R.eax & 0xFF] = R.edx; LOG("int 21h set vector %02x = %08x\n", R.eax & 0xFF, R.edx); break;
    case 0x35: {
        int v = R.eax & 0xFF;
        R.ebx = pm_vec_off[v] ? pm_vec_off[v] : VEC_STUB(v);
        R.es = sel_code;
        break;
    }
    case 0x2A: {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        SET16(R.ecx, tm->tm_year + 1900); SET16(R.edx, (tm->tm_mon + 1) << 8 | tm->tm_mday); set_al((uint8_t)tm->tm_wday);
        break;
    }
    case 0x2C: {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        SET16(R.ecx, tm->tm_hour << 8 | tm->tm_min); SET16(R.edx, tm->tm_sec << 8);
        break;
    }
    case 0x30: set_ax(5); break;
    case 0x4C: fflush(stdout); exit(R.eax & 0xFF);
    case 0x62: SET16(R.ebx, psp_sel); break;
    default: LOG("int 21h ah=%02x unsupported\n", ah); set_cf(true); set_ax(1); break;
    }
}

void sys_int(int n) {
    if (g_verbose > 1) logf_("int %02x ax=%04x bx=%04x es=%04x edi=%08x\n", n, R.eax & 0xFFFF, R.ebx & 0xFFFF, R.es, R.edi);
    switch (n) {
    case 0x31: dpmi(); break;
    case 0x21: dos21(); break;
    case 0x2F: {
        uint16_t ax = (uint16_t)R.eax;
        if ((ax >> 8) == 0x15) { bool cf; mscdex(&R.eax, &R.ebx, &R.ecx, sel_base_flat((uint16_t)R.es), &cf); set_cf(cf); }
        else if (ax == 0x1600) set_al(0);
        else if (ax == 0x1686) set_ax(0);
        break;
    }
    case 0x4B: set_cf(true); break;
    case 0x10: bios_int10(&R.eax, &R.ebx, &R.ecx, &R.edx, sel_base_flat((uint16_t)R.es)); break;
    case 0x33: set_ax(0); break;
    case 0x1A: { uint32_t t = rd32(0x46C); SET16(R.ecx, t >> 16); SET16(R.edx, t); break; }
    default: LOG("int %02xh ax=%04x ignored\n", n, R.eax & 0xFFFF); break;
    }
}

// Far call to a driver or to a default interrupt vector stub.
void call_far(uint32_t ptr) {
    uint32_t off = rd32(ptr);
    if (off >= VEC_STUB(0) && off < VEC_STUB(256)) {
        // chaining to the previous handler: behave like "int n; iretd"
        sys_int((off - VEC_STUB(0)) / 4);
        R.esp += 12;    // return address, cs and the flags pushed by the caller
        return;
    }
    LOG("far call to %04x:%08x ignored (sound driver)\n", rd16(ptr + 4), off);
    R.esp += 8;         // return address and cs
    R.eax = 0;
}

void sys_init(void) {
    sel_code = ldt_alloc(1); ldt_set_limit(sel_code, GAME_SIZE - 1);
    sel_data = ldt_alloc(1); ldt_set_limit(sel_data, GAME_SIZE - 1);
    sel_tib = ldt_alloc(1); ldt_set_base(sel_tib, g_tib); ldt_set_limit(sel_tib, 0xFFF);
    R.cs = sel_code; R.ds = R.es = R.ss = R.gs = sel_data; R.fs = sel_tib;
    for (int v = 0; v < 256; v++) { rm_vec[v] = 0xF0000000u | (v * 4); wr32(v * 4, rm_vec[v]); }
    // BIOS data area
    wr16(0x410, 0x0021); wr16(0x413, 640); wr8(0x449, 3); wr16(0x44A, 80); wr16(0x463, 0x3D4); wr8(0x484, 24);
    // PSP
    uint16_t psp_seg = (uint16_t)dosmem_alloc(0x10, NULL);
    psp_sel = ldt_alloc(1); ldt_set_base(psp_sel, (uint32_t)psp_seg << 4); ldt_set_limit(psp_sel, 0xFF);
    uint32_t psp = (uint32_t)psp_seg << 4;
    wr16(psp, 0x20CD); wr16(psp + 2, 0xA000); wr8(psp + 0x81, 0x0D);
}
