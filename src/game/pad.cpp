#include "game/pad.h"

#include <nds.h>

namespace pad {

static Pad s_pad;
static const Pad* s_override;
static uint32_t s_lFrames;    // frames L has been held
static bool s_lMoved;         // a direction was used while L was held (walk, not a tap)

// L doubles as walk (held with a direction) and weapon switch (a tap): the
// switch fires on release, only if the press was short and nothing moved.
static constexpr uint32_t TAP_FRAMES = 15;

void update(const input::State& in) {
	Pad& p = s_pad;
	const bool l = in.held & KEY_L;
	const int dx = (in.held & KEY_RIGHT ? 1 : 0) - (in.held & KEY_LEFT ? 1 : 0);
	const int dy = (in.held & KEY_UP ? 1 : 0) - (in.held & KEY_DOWN ? 1 : 0);
	p.walkLR = (int16_t)(dx * 128);
	p.walkUD = (int16_t)(dy * 128);

	p.nextWeaponJustDown = false;
	if (l) {
		s_lFrames++;
		if (dx || dy) s_lMoved = true;
	} else {
		if (s_lFrames && s_lFrames <= TAP_FRAMES && !s_lMoved) p.nextWeaponJustDown = true;
		s_lFrames = 0;
		s_lMoved = false;
	}
	p.pedWalk = l && (dx || dy);

	p.sprint = in.held & KEY_B;
	p.sprintJustDown = in.down & KEY_B;
	p.jumpJustDown = in.down & KEY_X;
	p.attack = in.held & KEY_A;
	p.attackJustDown = in.down & KEY_A;
	p.target = in.held & KEY_R;
	p.enterExitJustDown = in.down & KEY_Y;
}

const Pad& get() { return s_override ? *s_override : s_pad; }
const Pad& raw() { return s_pad; }
void setOverride(const Pad* p) { s_override = p; }

void consumeEdges() {
	s_pad.sprintJustDown = s_pad.jumpJustDown = s_pad.attackJustDown = false;
	s_pad.enterExitJustDown = s_pad.nextWeaponJustDown = false;
}

}
