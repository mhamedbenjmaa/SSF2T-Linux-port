// Object animation.
#include "sf2t/obj.h"
#include "sf2t/game.h"

// Original: 0x16eee6 (1498 callers).
// Counts down the current frame; when it runs out, moves to the next frame
// of the animation table and loads its duration and value.
void obj_anim_step(gaddr obj) {
    int16_t timer = (int16_t)(rd_s16(obj + OBJ_ANIM_TIMER) - 1);
    wr_s16(obj + OBJ_ANIM_TIMER, timer);
    if (timer > 0) return;

    gaddr table = rd_u32(obj + OBJ_ANIM_TABLE);
    int16_t frame = rd_s16(table + (uint32_t)(int32_t)rd_s16(obj + OBJ_ANIM_FRAME) * ANIM_ENTRY_SIZE + ANIM_ENTRY_NEXT);
    wr_s16(obj + OBJ_ANIM_FRAME, frame);

    gaddr entry = table + (uint32_t)(int32_t)frame * ANIM_ENTRY_SIZE;
    wr_s16(obj + OBJ_ANIM_TIMER, rd_s16(entry + ANIM_ENTRY_DURATION));
    wr_s16(obj + OBJ_ANIM_VALUE, rd_s16(entry + ANIM_ENTRY_VALUE));
}
