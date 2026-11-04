#include "assets/texture.h"

#include <string.h>

namespace tex {

static Texture pool[40];

Texture* load(const void* blob) {
	const ds::TexHeader* h = (const ds::TexHeader*)blob;
	if (!blob || h->magic != DS_FOURCC('D', 'T', 'E', 'X')) return nullptr;

	Texture* t = nullptr;
	for (Texture& s : pool)
		if (!s.id) { t = &s; break; }
	if (!t) return nullptr;

	if (glGenTextures(1, &t->id) != 1) return nullptr;
	glBindTexture(0, t->id);
	const u8* texels = (const u8*)h + sizeof(ds::TexHeader);
	const u8* pal = texels + ((h->texelBytes + 3) & ~3);
	// glTexImage2D takes TEXTURE_SIZE_ENUM indices (8,16,32..1024), not pixels
	auto sizeIdx = [](u16 n) { int i = 0; while (n > 8) { n >>= 1; i++; } return i; };
	if (glTexImage2D(0, 0, (GL_TEXTURE_TYPE_ENUM)h->format, sizeIdx(h->width), sizeIdx(h->height), 0,
	                 TEXGEN_TEXCOORD | (h->flags & ds::TEX_COLOR0_TRANSPARENT ? GL_TEXTURE_COLOR0_TRANSPARENT : 0), texels) == 0) {
		glDeleteTextures(1, &t->id);
		t->id = 0;
		return nullptr;
	}
	if (h->numColors)
		glColorTableEXT(0, 0, h->numColors, 0, 0, (const u16*)pal);
	t->format = (GL_TEXTURE_TYPE_ENUM)h->format;
	t->w = h->width;
	t->h = h->height;
	return t;
}

Texture* loadFromTxd(const void* dtxd, const char* name) {
	const ds::TxdHeader* th = (const ds::TxdHeader*)dtxd;
	if (!dtxd || th->magic != DS_FOURCC('D', 'T', 'X', 'D')) return nullptr;
	const ds::TxdEntry* e = (const ds::TxdEntry*)((const u8*)dtxd + sizeof(ds::TxdHeader));
	for (int i = 0; i < th->count; i++, e++)
		if (!strcmp(e->name, name))
			return load((const u8*)dtxd + e->offset);
	return nullptr;
}

void unload(Texture* t) {
	if (t && t->id) {
		glDeleteTextures(1, &t->id);
		t->id = 0;
	}
}

void clear() {
	for (Texture& t : pool) unload(&t);
}

int loadedCount() {
	int n = 0;
	for (Texture& t : pool) n += t.id != 0;
	return n;
}

}
