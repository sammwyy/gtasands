#pragma once

// Runtime debug/test switches, edited from the bottom-screen debug UI
// (hud/debugui, REND tab) and read by the renderer, CJ and the simulation.
namespace dbg {

struct Options {
	bool drawCj = true;      // CJ mesh (display lists)
	bool skeleton = false;   // bone lines overlay
	bool ground = true;      // placeholder ground grid + boxes
	bool fixedStep = true;   // 60 Hz fixed-step sim (off = one step per frame, old behaviour)
	int simSpeedQ = 4;       // simulation speed in quarters: 0 = paused, 4 = x1, 8 = x2
	float camDist = 3.6f;    // chase camera distance
	// sky / time cycle / weather (SKY tab)
	int clockRate = 1;       // game minutes per real second: 0 = frozen, 1 = CClock default
	bool sky = true;         // sky polys (off = flat clear colour)
	bool skySprites = true;  // sun, moon, stars, low clouds, rainbow, flares
	bool fog = true;         // hardware fog from timecyc fog start / far clip
	int fogView = 140;       // DS view distance (m) the timecyc far clip is scaled to
	bool weatherFx = true;   // rain / sandstorm streaks, lightning
	bool pedLight = true;    // timecyc lighting on CJ (off = full bright)
};

extern Options opts;

}
