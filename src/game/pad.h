#pragma once

#include <stdint.h>

#include "platform/input.h"

// CPad-lite: the DS buttons mapped to the PC game's on-foot actions in
// keyboard ("solo teclado") mode. The D-pad is WASD: a digital direction is
// full stick deflection (+-128), so CJ jogs by default exactly like the PC
// keyboard; the walk key caps the move blend at a walk.
//
//   D-pad      move (GetPedWalkLeftRight / GetPedWalkUpDown, +-128)
//   L hold     walk (PC: LALT) while a direction is held
//   L tap      next weapon (PC: E / mouse wheel) - a short press with no move
//   B          sprint (PC: SPACE). Tap repeatedly to run faster.
//   X          jump (PC: LSHIFT)
//   A          attack / fire (PC: LMB / LCTRL)
//   R          target / aim (PC: RMB)
//   Y          enter / exit vehicle (PC: F / ENTER)
//   touch      look around (PC: mouse); idle stylus = camera returns behind
namespace pad {

struct Pad {
	int16_t walkLR, walkUD;   // -128..128 (right / up positive)
	bool pedWalk;             // walk modifier held (m_bPedWalk)
	bool sprint, sprintJustDown;
	bool jumpJustDown;
	bool attack, attackJustDown;
	bool target;
	bool enterExitJustDown;
	bool nextWeaponJustDown;
};

// Once per frame, before the simulation steps.
void update(const input::State& in);
const Pad& get();
// The real buttons even while an override is set.
const Pad& raw();
// A scripted stick for the simulation (CJ walking to a car door), nullptr = off.
void setOverride(const Pad* p);
// Fixed-step loops: edge flags fire on the first step only.
void consumeEdges();

}
