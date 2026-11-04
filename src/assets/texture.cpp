#include "assets/texture.h"

#include <string.h>

#include "mem/pool.h"

namespace tex {

static mem::Pool<Texture> s_pool;
static u32 s_vram;

static bool ensurePool() {
	return s_pool.capacity() || s_pool.init(mem::budget::MAX_TEX_SLOTS, mem::TAG_TEXTURE, true);
}

Texture* load(const void* blob, Group g) {
	const ds::TexHeader* h = (const ds::TexHeader*)blob;
	if (!blob || h->magic != DS_FOURCC('D', 'T', 'E', 'X') || !ensurePool()) return nullptr;
	Texture* t = s_pool.create();
	if (!t) return nullptr;
	if (glGenTextures(1, &t->id) != 1) {
		s_pool.destroy(t);
		return nullptr;
	}
	glBindTexture(0, t->id);
	const u8* texels = (const u8*)h + sizeof(ds::TexHeader);
	const u8* pal = texels + ((h->texelBytes + 3) & ~3);
	// glTexImage2D takes TEXTURE_SIZE_ENUM indices (8,16,32..1024), not pixels
	auto sizeIdx = [](u16 n) { int i = 0; while (n > 8) { n >>= 1; i++; } return i; };
	if (glTexImage2D(0, 0, (GL_TEXTURE_TYPE_ENUM)h->format, sizeIdx(h->width), sizeIdx(h->height), 0,
	                 TEXGEN_TEXCOORD | (h->flags & ds::TEX_COLOR0_TRANSPARENT ? GL_TEXTURE_COLOR0_TRANSPARENT : 0), texels) == 0) {
		glDeleteTextures(1, &t->id);
		s_pool.destroy(t);
		return nullptr;
	}
	if (h->numColors) {
		// palettes are stored without the alpha bit; opaque texels need it set
		u16 pal16[256];
		const int n = h->numColors < 256 ? h->numColors : 256;
		for (int i = 0; i < n; i++) pal16[i] = ((const u16*)pal)[i] | BIT(15);
		glColorTableEXT(0, 0, n, 0, 0, pal16);
	}
	t->format = (GL_TEXTURE_TYPE_ENUM)h->format;
	t->w = h->width;
	t->h = h->height;
	t->group = g;
	t->vramBytes = h->texelBytes + h->numColors * 2;
	s_vram += t->vramBytes;
	return t;
}

Texture* loadFromTxd(const void* dtxd, const char* name, Group g) {
	const ds::TxdHeader* th = (const ds::TxdHeader*)dtxd;
	if (!dtxd || th->magic != DS_FOURCC('D', 'T', 'X', 'D')) return nullptr;
	const ds::TxdEntry* e = (const ds::TxdEntry*)((const u8*)dtxd + sizeof(ds::TxdHeader));
	for (int i = 0; i < th->count; i++, e++)
		if (!strcmp(e->name, name))
			return load((const u8*)dtxd + e->offset, g);
	return nullptr;
}

Texture* loadFirst(const void* dtxd, Group g) {
	const ds::TxdHeader* th = (const ds::TxdHeader*)dtxd;
	if (!dtxd || th->magic != DS_FOURCC('D', 'T', 'X', 'D') || !th->count) return nullptr;
	const ds::TxdEntry* e = (const ds::TxdEntry*)((const u8*)dtxd + sizeof(ds::TxdHeader));
	return load((const u8*)dtxd + e->offset, g);
}

void unload(Texture* t) {
	if (!t || !t->id) return;
	glDeleteTextures(1, &t->id);
	t->id = 0;   // stale pointers held by callers bind "no texture"
	s_vram -= t->vramBytes;
	s_pool.destroy(t);
}

void clear(Group g) {
	s_pool.forEach([g](Texture& t) {
		if (t.group == g) unload(&t);
	});
}

int loadedCount() { return s_pool.count(); }
u32 vramBytes() { return s_vram; }

}
