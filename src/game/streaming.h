#pragma once

#include "assets/pack.h"

// CStreaming placeholder: the base archives stay open so model/world streaming
// can hit NitroFS directly. Opened by the loading screen (gta3's sparse TOC
// index is built incrementally there); safe to call find() afterwards.
// Anim clips stream through game/animstore, radar tiles through radar.cpp.
namespace streaming {

extern Pack g_models;   // models/gta3.dspk (paged TOC)
extern Pack g_interiors;
extern Pack g_player;   // models/player.dspk
extern Pack g_anim;     // anim/anim.dspk

}
