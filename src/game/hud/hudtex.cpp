#include "game/hud/hudtex.h"

#include <string.h>

#include "mem/mem.h"

namespace hudtex {

const ds::TexHeader* find(const void* dtxd, const char* name) {
	const ds::TxdHeader* th = (const ds::TxdHeader*)dtxd;
	if (!dtxd || th->magic != DS_FOURCC('D', 'T', 'X', 'D')) return nullptr;
	const ds::TxdEntry* e = (const ds::TxdEntry*)((const u8*)dtxd + sizeof(ds::TxdHeader));
	for (int i = 0; i < th->count; i++, e++)
		if (!strcmp(e->name, name))
			return (const ds::TexHeader*)((const u8*)dtxd + e->offset);
	return nullptr;
}

struct Pix { u16 c; u8 a; };

// Palette entries are stored as plain rgb15 (no alpha bit); the opaque flag is
// part of the pixel, not the file format, so it gets set at decode time.
static inline u16 palc(const u16* pal, int i) { return (u16)(pal[i] | BIT(15)); }

// Per-texel decode of every supported DS format into (color, alpha).
static inline Pix px(const ds::TexHeader* h, const u8* tex, const u16* pal, int i) {
	switch (h->format) {
	case 7: { // DIRECT: rgb15 + bit15 opaque flag
		u16 v = ((const u16*)tex)[i];
		return { (u16)(v | BIT(15)), (u8)((v & BIT(15)) ? 255 : 0) };
	}
	case 1: { // A3I5: 5-bit palette index, 3-bit alpha
		u8 v = tex[i];
		return { palc(pal, v & 31), (u8)((v >> 5) * 255 / 7) };
	}
	case 6: { // A5I3: 3-bit palette index, 5-bit alpha
		u8 v = tex[i];
		return { palc(pal, v & 7), (u8)((v >> 3) * 255 / 31) };
	}
	case 2: { // PAL4: 2bpp, index 0 transparent
		u8 v = (tex[i / 4] >> ((i % 4) * 2)) & 3;
		return { palc(pal, v), (u8)((v == 0 && (h->flags & ds::TEX_COLOR0_TRANSPARENT)) ? 0 : 255) };
	}
	case 3: { // PAL16: 4bpp
		u8 v = (tex[i / 2] >> ((i % 2) * 4)) & 15;
		return { palc(pal, v), (u8)((v == 0 && (h->flags & ds::TEX_COLOR0_TRANSPARENT)) ? 0 : 255) };
	}
	case 4: { // PAL256: 8bpp
		u8 v = tex[i];
		return { palc(pal, v), (u8)((v == 0 && (h->flags & ds::TEX_COLOR0_TRANSPARENT)) ? 0 : 255) };
	}
	}
	return { 0, 0 };
}

bool decodeInto(const ds::TexHeader* h, u16* col, u8* alpha) {
	if (!h || h->magic != DS_FOURCC('D', 'T', 'E', 'X') || !col) return false;
	const int n = h->width * h->height;
	const u8* tex = (const u8*)h + sizeof(ds::TexHeader);
	const u16* pal = (const u16*)(tex + ((h->texelBytes + 3) & ~3));
	for (int i = 0; i < n; i++) {
		const Pix p = px(h, tex, pal, i);
		col[i] = p.c;
		if (alpha) alpha[i] = p.a;
	}
	return true;
}

bool decodeSprite(const ds::TexHeader* h, subfb::Sprite& out) {
	if (!h || h->magic != DS_FOURCC('D', 'T', 'E', 'X')) return false;
	const int n = h->width * h->height;
	u16* col = (u16*)mem::alloc(n * 3, mem::TAG_HUD);
	if (!col) return false;
	u8* alp = (u8*)(col + n);
	decodeInto(h, col, alp);
	out = { (u8)h->width, (u8)h->height, col, alp };
	return true;
}

void freeSprite(subfb::Sprite& s) {
	mem::free((void*)s.col);   // alpha plane shares the block
	s = { 0, 0, nullptr, nullptr };
}

u8* decodeCoverage(const ds::TexHeader* h) {
	if (!h || h->magic != DS_FOURCC('D', 'T', 'E', 'X')) return nullptr;
	const int n = h->width * h->height;
	u8* out = (u8*)mem::perm(n, mem::TAG_HUD);
	if (!out) return nullptr;
	const u8* tex = (const u8*)h + sizeof(ds::TexHeader);
	const u16* pal = (const u16*)(tex + ((h->texelBytes + 3) & ~3));
	for (int i = 0; i < n; i++) {
		const Pix p = px(h, tex, pal, i);
		// coverage = alpha * color luminance (font palettes are greyscale)
		const int lum = ((p.c & 31) + ((p.c >> 5) & 31) + ((p.c >> 10) & 31)) / 3;
		out[i] = (u8)(p.a * lum / 31);
	}
	return out;
}

}
