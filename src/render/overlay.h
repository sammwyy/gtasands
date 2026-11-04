#pragma once

#include <nds/ndstypes.h>

// Debug geometry (skeleton, collision) drawn on top of everything with a dark
// 1-pixel outline. The emitter runs five times under projections with a
// constant near depth (the sky's constant-depth trick, at the near end): four
// black passes shifted one pixel left/right/up/down, then the colour pass at
// a nearer constant depth, so it always wins over its own outline and over
// the scene. Costs 5x the polygons of what the emitter draws.
// Emitters use overlay::color (black during the outline passes) and
// overlay::polyFmt, set their own matrices (the modelview is left as is) and
// never touch the projection.
namespace overlay {

using EmitFn = void (*)(void* ctx);
// outline = false: only the colour pass (1x the polygons)
void draw(EmitFn fn, void* ctx, bool outline = true);
void color(u16 rgb15);
// glPolyFmt with fog off and 1-dot polygons kept; alpha 0 = wireframe.
void polyFmt(int alpha);
bool outlinePass();

}
