// Game object (fighters, projectiles, effects): field offsets.
//
// The layout is the arcade (CPS-2) object RAM layout, packed, so many
// fields sit at odd offsets. Names are added as fields get understood;
// unknown fields keep their offset in the name.
#pragma once
#include "mem.h"

// global "current object" pointer used by the translated arcade code
#define G_CUR_OBJ   0x2f4e80u
#define G_OTHER_OBJ 0x2f4e84u

// animation
#define OBJ_ANIM_FRAME  0x15f   // s16: index of the current frame in the animation table
#define OBJ_ANIM_TABLE  0x1bb   // u32: address of the animation table
#define OBJ_ANIM_TIMER  0x1cf   // s16: frames left before the next animation frame
#define OBJ_ANIM_VALUE  0x1d1   // s16: per-frame value copied from the table (meaning TBD)

// animation table entry (0x18 bytes)
#define ANIM_ENTRY_SIZE     0x18
#define ANIM_ENTRY_DURATION 0x02   // s16
#define ANIM_ENTRY_VALUE    0x04   // s16
#define ANIM_ENTRY_NEXT     0x06   // s16: index of the next frame
