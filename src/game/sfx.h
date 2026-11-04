#pragma once

#include <stdint.h>

// Frontend SFX from the real GENRL pak (banks 59/60), kept as IMA-ADPCM and
// played natively by the DS mixer (no PCM decode).
namespace sfx {

void init();              // load frontend banks (no-op if files missing)
void highlight();         // navigate
void select_();           // confirm
void back();              // cancel
void error();             // invalid action (e.g. empty save slot)
void noiseTest();         // sfx volume slider test tone
void start();             // menu opened/closed whoosh
bool tuneStep();          // random loading tune (bank 82): one channel read per call, true once playing
void tuneStart();         // same, all at once
void tuneStop();

}
