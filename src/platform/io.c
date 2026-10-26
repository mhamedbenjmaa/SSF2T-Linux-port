// PC hardware seen through I/O ports: PIC, PIT timer, keyboard controller,
// VGA DAC and status register, joystick. No sound card is present.
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t t0_ns;

// PIC
static uint8_t pic_mask[2] = { 0xB8, 0xFF };

// PIT channel 0
static uint32_t pit_div = 65536;
static uint64_t pit_epoch_ns, pit_ticks_done;
static int pit_write_lo = 1, pit_read_lo = 1;
static uint16_t pit_latch, pit_new;
static bool pit_latched;

double pit_hz(void) { return 1193182.0 / pit_div; }
static uint16_t pit_count_now(void) {
    uint64_t clocks = (now_ns() - pit_epoch_ns) * 1193182ull / 1000000000ull;
    return (uint16_t)(pit_div - clocks % pit_div);
}
static void pit_reprogram(uint32_t div) {
    pit_div = div ? div : 65536;
    pit_epoch_ns = now_ns();
    pit_ticks_done = 0;
    LOG("PIT rate %.2f Hz\n", pit_hz());
}

// keyboard: scancode queue; a key stays down at least 40 ms as seen by the game
static uint8_t kq[256];
static int kq_head, kq_tail;
static uint8_t kbd_data;
static bool kbd_full;
static uint64_t key_down_at[128];

void kbd_push(uint8_t sc) {
    int n = (kq_tail + 1) & 255;
    if (n == kq_head) return;
    kq[kq_tail] = sc;
    kq_tail = n;
}
static bool kbd_release_ready(uint64_t now) {
    uint8_t sc = kq[kq_head];
    if (sc == 0xE0 && ((kq_head + 1) & 255) != kq_tail) sc = kq[(kq_head + 1) & 255];
    if (!(sc & 0x80) || sc == 0xE0) return true;
    return now - key_down_at[sc & 0x7F] >= 40000000ull;
}

// VGA
static uint8_t dac_wi, dac_ri;
static int dac_wc, dac_rc;
static uint8_t seq_idx, seq[8], gc_idx, gc[16], crtc_idx, crtc[32];
static uint64_t retrace_frame_seen = ~0ull;

static uint8_t vga_status(void) {
    // ~70 Hz; report each vertical retrace once (a fast CPU would otherwise
    // see the same retrace many times and run the game too fast)
    uint64_t period = 14268000, t = now_ns() - t0_ns, frame = t / period, ph = t % period;
    if (ph > period - period / 20 && frame != retrace_frame_seen) { retrace_frame_seen = frame; return 0x09; }
    return ((ph / 31778) % 2) ? 0x01 : 0x00;
}

static uint8_t port61;

uint32_t port_in(uint16_t port, int size) {
    if (size == 2) return port_in(port, 1) | port_in(port + 1, 1) << 8;
    if (size == 4) return port_in(port, 2) | port_in(port + 2, 2) << 16;
    switch (port) {
    case 0x21: return pic_mask[0];
    case 0xA1: return pic_mask[1];
    case 0x40: {
        uint16_t v = pit_latched ? pit_latch : pit_count_now();
        uint8_t r = pit_read_lo ? (v & 0xFF) : (v >> 8);
        if (!pit_read_lo) pit_latched = false;
        pit_read_lo ^= 1;
        return r;
    }
    case 0x60: kbd_full = false; return kbd_data;
    case 0x61: port61 ^= 0x10; return port61;
    case 0x64: return kbd_full ? 0x1D : 0x1C;
    case 0x201: return 0xFF;                 // no joystick
    case 0x3C7: return 3;
    case 0x3C8: return dac_wi;
    case 0x3C9: { uint8_t v = g_vga_pal[(dac_ri * 3 + dac_rc) % 768]; if (++dac_rc == 3) { dac_rc = 0; dac_ri++; } return v; }
    case 0x3C4: return seq_idx;
    case 0x3C5: return seq[seq_idx & 7];
    case 0x3CE: return gc_idx;
    case 0x3CF: return gc[gc_idx & 15];
    case 0x3D4: return crtc_idx;
    case 0x3D5: return crtc[crtc_idx & 31];
    case 0x3DA: return vga_status();
    default: return 0xFF;
    }
}

void port_out(uint16_t port, uint32_t val, int size) {
    if (size == 2) { port_out(port, val & 0xFF, 1); port_out(port + 1, (val >> 8) & 0xFF, 1); return; }
    if (size == 4) { port_out(port, val & 0xFFFF, 2); port_out(port + 2, val >> 16, 2); return; }
    uint8_t v = (uint8_t)val;
    switch (port) {
    case 0x21: pic_mask[0] = v; break;
    case 0xA1: pic_mask[1] = v; break;
    case 0x43:
        if ((v & 0xC0) == 0) {
            if ((v & 0x30) == 0) { pit_latch = pit_count_now(); pit_latched = true; pit_read_lo = 1; }
            else { pit_write_lo = 1; pit_read_lo = 1; }
        }
        break;
    case 0x40:
        if (pit_write_lo) { pit_new = v; pit_write_lo = 0; }
        else { pit_new |= v << 8; pit_write_lo = 1; pit_reprogram(pit_new); }
        break;
    case 0x61: port61 = v; break;
    case 0x3C8: dac_wi = v; dac_wc = 0; break;
    case 0x3C7: dac_ri = v; dac_rc = 0; break;
    case 0x3C9: g_vga_pal[(dac_wi * 3 + dac_wc) % 768] = v & 0x3F; if (++dac_wc == 3) { dac_wc = 0; dac_wi++; } break;
    case 0x3C4: seq_idx = v; break;
    case 0x3C5: seq[seq_idx & 7] = v; break;
    case 0x3CE: gc_idx = v; break;
    case 0x3CF: gc[gc_idx & 15] = v; break;
    case 0x3D4: crtc_idx = v; break;
    case 0x3D5: crtc[crtc_idx & 31] = v; break;
    default: break;
    }
}

static uint64_t last_frame_ns;

void io_tick(void) {
    uint64_t now = now_ns();
    uint64_t due = (uint64_t)((double)(now - pit_epoch_ns) * pit_hz() / 1e9);
    if (due > pit_ticks_done) {
        pit_ticks_done = due;
        if (!(pic_mask[0] & 1)) irq_raise(0);
    }
    wr32(0x46C, (uint32_t)((double)(now - t0_ns) * 18.2065 / 1e9));
    if (!kbd_full && kq_head != kq_tail && kbd_release_ready(now)) {
        kbd_data = kq[kq_head];
        kq_head = (kq_head + 1) & 255;
        kbd_full = true;
        if (kbd_data != 0xE0 && !(kbd_data & 0x80)) key_down_at[kbd_data] = now;
        if (!(pic_mask[0] & 2)) irq_raise(1);
    }
    cd_tick();
    { void sfx_tick(void); sfx_tick(); }
    // debug: SSF2T_INPUT="seconds:scancode,..." (hex, >0xFF = E0 prefix)
    {
        static const char *inp;
        static double next_t = -1;
        static unsigned next_sc, release_sc;
        static uint64_t release_at;
        if (!inp) { inp = getenv("SSF2T_INPUT"); if (!inp) inp = ""; }
        if (release_at && now >= release_at) {
            if (release_sc > 0xFF) kbd_push(0xE0);
            kbd_push((uint8_t)((release_sc & 0x7F) | 0x80));
            release_at = 0;
        }
        if (next_t < 0 && *inp) {
            if (sscanf(inp, "%lf:%x", &next_t, &next_sc) != 2) inp = "";
            else { const char *c = strchr(inp, ','); inp = c ? c + 1 : ""; }
        }
        if (next_t >= 0 && !release_at && (double)(now - t0_ns) / 1e9 >= next_t) {
            if (next_sc > 0xFF) kbd_push(0xE0);
            kbd_push((uint8_t)(next_sc & 0x7F));
            release_sc = next_sc; release_at = now + 100000000; next_t = -1;
        }
    }
    if (now - last_frame_ns >= 14268000) {
        last_frame_ns = now;
        video_poll_events();
        video_present();
        if (video_quit_requested()) { fflush(stdout); exit(0); }
    }
}

void io_init(void) {
    t0_ns = now_ns();
    pit_epoch_ns = t0_ns;
    seq[4] = 0x0E;
}
