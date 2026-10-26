// Upscaling filter (filter.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

// 320x200 palette indices -> 1280x800 RGB (0x00RRGGBB)
void filter_xbr4(const uint8_t *src, const uint32_t *pal, uint32_t *dst);
