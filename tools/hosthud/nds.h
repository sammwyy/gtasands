#pragma once
// Minimal nds.h shim for host-compiling the bottom-screen HUD renderer.
// Provides just what src/game/hud/* + friends touch.
#include <stdint.h>
#include <math.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

#define BIT(n) (1 << (n))
#define RGB15(r, g, b) (((b) << 10) | ((g) << 5) | (r))

// libnds brads: 32768 = 360 deg; returns 4.12 fixed
static inline s16 sinLerp(s16 a) { return (s16)(sinf((float)a * (2.0f * 3.14159265f) / 32768.0f) * 4096.0f); }
static inline s16 cosLerp(s16 a) { return (s16)(cosf((float)a * (2.0f * 3.14159265f) / 32768.0f) * 4096.0f); }

enum { MODE_0_2D = 0x10000, MODE_5_2D = 0x10005 };
enum BgType { BgType_Text4bpp = 0, BgType_Bmp8 = 6, BgType_Bmp16 = 7 };
enum BgSize { BgSize_T_256x256 = 0, BgSize_B16_256x256 = 0xC0 };

struct PrintConsole;
static inline void videoSetModeSub(u32) {}
int bgInitSub(int layer, BgType type, BgSize size, int mapBase, int tileBase);
static inline void bgSetPriority(int, int) {}
u16* bgGetGfxPtr(int id);
void DC_FlushRange(const void* p, u32 size);
void dmaCopy(const void* src, void* dst, u32 size);
