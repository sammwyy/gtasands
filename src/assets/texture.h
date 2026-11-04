#pragma once

#include <nds.h>

#include "assets/ds_formats.h"

// Texture cache: loads DTEX blobs (from DTXD in NitroFS / DSPK) into VRAM via
// the libnds allocator. A small fixed number of slots is enough for the
// frontend (2 fonts + current background + a few sprites).
namespace tex {

struct Texture {
	GL_TEXTURE_TYPE_ENUM format;
	u16 w, h;
	int id = 0;
};

// Uploads a DTEX blob. Returns id for glBindTexture(0, id).
Texture* load(const void* blob);
// Finds `name` inside a DTXD blob and uploads it.
Texture* loadFromTxd(const void* dtxd, const char* name);
// Uploads the first texture inside a DTXD blob.
Texture* loadFirst(const void* dtxd);
void unload(Texture* t);
void clear();
int loadedCount();

}
