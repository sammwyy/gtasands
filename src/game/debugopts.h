#pragma once

// Runtime debug/test switches, edited from the bottom-screen debug UI
// (hud/debugui, REND tab) and read by the renderer, CJ and the simulation.
namespace dbg {

struct Options {
	bool drawCj = true;      // CJ mesh (display lists)
	bool skeleton = false;   // bone lines overlay (on top, outlined)
	bool collision = false;  // map collision wireframe overlay (on top, outlined)
	bool colOutline = true;  // collision overlay outline (5x the polygons)
	int colRange = 12;       // m around the player drawn by the collision overlay
	int colPolys = 900;      // polygon budget of the collision overlay (incl. outline passes)
	bool physics = true;     // collision response for CJ / camera (off = old flat z, no walls)
	bool ground = false;     // placeholder ground grid + boxes (always on without world.dwld)
	bool world = true;       // streamed map (game/world)
	int worldPolys = 1800;   // stop drawing map objects past this hardware polygon count (2048 max)
	bool fixedStep = true;   // 60 Hz fixed-step sim (off = one step per frame, old behaviour)
	int simSpeedQ = 4;       // simulation speed in quarters: 0 = paused, 4 = x1, 8 = x2
	int playerSpeedQ = 4;
	float camDist = 3.6f;    // chase camera distance
	// sky / time cycle / weather (SKY tab)
	int clockRate = 1;       // game minutes per real second: 0 = frozen, 1 = CClock default
	bool sky = true;         // sky polys (off = flat clear colour)
	bool skySprites = true;  // sun, moon, stars, low clouds, rainbow, flares
	bool fog = true;         // hardware fog from timecyc fog start / far clip
	int fogView = 140;       // DS view distance (m) the timecyc far clip is scaled to
	bool weatherFx = true;   // rain / sandstorm streaks, lightning
	bool pedLight = true;    // timecyc lighting on CJ (off = full bright)
	// vehicles (GAME / REND tabs)
	bool water = true;       // water.dat surfaces (game/waterlevel)
	bool skidmarks = true;   // CSkidmarks strips
	bool vehLights = true;   // head / tail light coronas + the headlight pool on the ground
	// peds (REND tab)
	bool peds = true;        // random ped population (spawning; live peds keep going when off)
	int pedDensity = 100;    // % of the popcycle.dat ped count (CPopulation::PedDensityMultiplier)
	int pedPolys = 500;      // polygons peds may submit per frame (nearest first)
};

extern Options opts;

}
