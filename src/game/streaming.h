#pragma once

#include "assets/pack.h"

// CStreaming placeholder: the base archives stay open so model/world streaming
// can hit NitroFS directly once that milestone lands. Opened by the loading
// screen; safe to call find() on them afterwards.
namespace streaming {

extern Pack g_models;   // models/gta3.dspk
extern Pack g_player;   // models/player.dspk
extern Pack g_anim;     // anim/anim.dspk
extern void* g_pedIfp;  // anim/ped.ifp DANM blob, always resident

}
