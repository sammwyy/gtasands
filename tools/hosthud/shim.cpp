// Host-side implementations of the libnds/timing bits the HUD touches.
#include "nds.h"
#include <stdlib.h>
#include <string.h>
#include "platform/timing.h"

// fake sub-engine bitmap VRAM
static u16 s_gfx[256 * 256];

int bgInitSub(int layer, BgType, BgSize, int, int) { return layer; }
u16* bgGetGfxPtr(int) { return s_gfx; }
void DC_FlushRange(const void*, u32) {}
void dmaCopy(const void* src, void* dst, u32 size) { memcpy(dst, src, size); }

namespace timing {
static u32 s_vbl, s_us;
void init() {}
void frameStart() {}
void frameEnd() {}
void tick() { s_vbl++; }      // host harness: advance one emulated frame
u32 frameUs() { return s_us; }
u32 fps() { return 60; }
u32 vblanks() { return s_vbl; }
void setUs(u32 us) { s_us = us; }
}
