#pragma once

#include <stdint.h>

// Frontend SFX from the real GENRL pak (banks 59/60), IMA-ADPCM decoded to
// PCM16 once at init and played via libnds channels.
namespace sfx {

void init();              // load frontend banks (no-op if files missing)
void highlight();         // navigate
void select_();           // confirm
void back();              // cancel
void error();             // invalid action (e.g. empty save slot)
void noiseTest();         // sfx volume slider test tone
void start();             // menu opened/closed whoosh
void tuneStart();         // random loading tune, looped (bank 82)
void tuneStop();

}
