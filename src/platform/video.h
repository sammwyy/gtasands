#pragma once

#include <nds/ndstypes.h>

// Screen layout:
//   top    = main engine, 3D (game world render)
//   bottom = sub engine, 2D (HUD: radar, weapon, money, touch UI)
//
// VRAM plan:
//   A, B -> 3D textures (256 KB)      D -> free (more textures / capture)
//   C    -> sub BG (HUD, 128 KB)      E -> 3D texture palettes
//   F..I -> free (sprites / ext palettes)
namespace video {

// Main 3D projection (gluPerspective(60, 4:3, NEAR, FAR)).
constexpr float FOV_Y = 60.0f;
constexpr float NEAR_PLANE = 0.1f;
constexpr float FAR_PLANE = 200.0f;
constexpr float TAN_HALF_FOV_Y = 0.57735f;
constexpr float TAN_HALF_FOV_X = TAN_HALF_FOV_Y * 256.0f / 192.0f;

void init();
void beginFrame();   // resets the per-frame render environment to the defaults
void endFrame();     // flush + vblank, then applies the environment (see below)

// Per-frame render environment. The 3D engine reads DISP3DCNT, the fog table
// and the clear colour while it scans out the *previous* flush, so writing
// them mid-frame would band the image. They are latched here and written
// right after the vblank that swaps the frame they belong to. Every frame
// starts from: no fog, default clear colour, auto translucent sort.
struct Fog {
	u16 color;          // RGB15
	u8 shift;           // FOG_SHIFT
	u16 offset;         // FOG_OFFSET (15-bit depth)
	u8 table[32];       // densities 0..127
};
void setFog(const Fog* fog);          // nullptr = off
void setClearColor(u16 rgb15);
// Translucent polygons in submission order (GL_TRANS_MANUALSORT) instead of
// the hardware Y-sort: layered sky sprites need it.
void setManualSort(bool on);

// 15-bit z-buffer depth (what FOG_OFFSET compares against) of an eye-space
// distance under the main projection, and its inverse.
int depth15(float dist);
float distForDepth15(int depth);

#ifdef GTASADS_DEBUG
// Headless screenshot: the next displayed 3D frame is captured into VRAM D
// (borrowed from the texture allocator: whatever texture lives in D is
// garbage afterwards, so this is for test runs) and written as a raw dump:
// "DSCR" + u16 w, h + RGB15 pixels. tools/dsshot.py converts it to PNG.
void requestCapture(const char* path);
#endif

}
