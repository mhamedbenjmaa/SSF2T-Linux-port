// Hand-written replacements for recompiled functions (listed in
// port/overrides.txt). Each F_xxxxxx is entered with the return address on
// the stack and must end like the original "ret".
#include "platform.h"
#include "sf2t/game.h"

// 16eee6: obj_anim_step(obj) - rewritten in src/game/anim.c
void F_16eee6(void) {
    uint32_t obj = rd32(R.esp + 4);
    obj_anim_step(obj);
    R.eax = obj;
    HW_RET(0);
}

// 16b5e4: fight_frame. It re-reads the keyboard on every call, and the fight
// loop calls it continuously; on a fast CPU new button presses were
// overwritten before the game logic saw them. Wait for the next game tick
// (as a 1995 CPU effectively did), then run the original.
void F_16b5e4(void) {
    while (rd32(0x22f2f0) == 0 && rd16(0x2f02c8) == rd16(0x2f02bc)) {
        sleep_ns(300000);
        irq_poll_slow();
    }
    R_16b5e4();
}

// ---- sound effects: the HMI SOS digital driver is replaced by sfx.c ----
// 22f2e0: sound effects enabled (from the configuration)
// 2d6d80: handle of the last started sample (the round announcer is waited on)
void sfx_init(void);
int  sfx_start(uint32_t addr, uint32_t len, uint32_t rate, int vol);
bool sfx_playing(int h);
void sfx_stop(int h);
void sfx_stop_all(void);
void sfx_volume(int h, int vol);
static uint32_t arg(int i) { return rd32(R.esp + 4 + 4 * i); }

// 140d29: sound_init
void F_140d29(void) {
    if (rd32(0x22f2e0)) sfx_init();
    HW_RET(0);
}

// 1410d1: start a sample (address, length, sample rate in Hz, volume 0..255)
void F_1410d1(void) {
    if (rd32(0x22f2e0)) wr32(0x2d6d80, (uint32_t)sfx_start(arg(0), arg(1), arg(2), (int)(arg(3) << 7)));
    LOG("sfx start %08x len %u rate %u vol %u -> %d\n", arg(0), arg(1), arg(2), arg(3), (int)rd32(0x2d6d80));
    HW_RET(0);
}

// 1411f1: is the last started sample (the round announcer) still playing?
void F_1411f1(void) {
    R.eax = rd32(0x22f2e0) && sfx_playing((int)rd32(0x2d6d80)) ? 1 : 0;
    HW_RET(0);
}

// 141224: stop the last started sample
void F_141224(void) {
    if (rd32(0x22f2e0)) sfx_stop((int)rd32(0x2d6d80));
    HW_RET(0);
}

// 141278: set the volume of the last started sample (0..255)
void F_141278(void) {
    if (rd32(0x22f2e0)) sfx_volume((int)rd32(0x2d6d80), (int)(arg(0) << 7));
    HW_RET(0);
}

// 1412a1: sound shutdown
void F_1412a1(void) {
    sfx_stop_all();
    HW_RET(0);
}
