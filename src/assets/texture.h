#pragma once

#include <nds.h>

#include "assets/ds_formats.h"

// VRAM texture manager: DTEX blobs (from DTXD in NitroFS / DSPK) uploaded via
// the libnds texture allocator (banks A+B+D, palettes in E). Slots live in a
// fixed mem::Pool (budget::MAX_TEX_SLOTS); every texture belongs to a group so
// a state change frees exactly what it owns (the frontend art on Start Game
// must not take CJ's textures with it).
namespace tex {

enum Group : u8 { GROUP_FRONTEND, GROUP_GAME, GROUP_COUNT };

struct Texture {
	GL_TEXTURE_TYPE_ENUM format;
	u16 w, h;
	int id = 0;
	u32 vramBytes = 0;   // texels + palette
	Group group = GROUP_FRONTEND;
};

// Uploads a DTEX blob. Returns id for glBindTexture(0, id).
Texture* load(const void* blob, Group g = GROUP_FRONTEND);
// Finds `name` inside a DTXD blob and uploads it.
Texture* loadFromTxd(const void* dtxd, const char* name, Group g = GROUP_FRONTEND);
// Uploads the first texture inside a DTXD blob.
Texture* loadFirst(const void* dtxd, Group g = GROUP_FRONTEND);
void unload(Texture* t);
void clear(Group g);
int loadedCount();
u32 vramBytes();     // live texture VRAM (texels + palettes)

}
