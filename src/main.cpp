#include <nds.h>
#include <stdlib.h>

#include "assets/pack.h"
#include "hud/hud.h"
#include "platform/fs.h"
#include "platform/input.h"
#include "platform/timing.h"
#include "platform/video.h"
#include "render/renderer.h"

static void probeGameData() {
	if (!fs::init()) {
		hud::log("\x1b[31mnitroFS init failed\x1b[39m");
		return;
	}
	static char buf[512];
	long n = fs::readText("data/gta.dat", buf, sizeof buf);
	if (n < 0) {
		hud::log("data/gta.dat missing (make assets)");
		return;
	}
	hud::log("gta.dat: %ld bytes read", n);

	Pack gta3;
	if (!gta3.open("nitro:/models/gta3.dspk")) {
		hud::log("models/gta3.dspk missing");
		return;
	}
	hud::log("gta3.dspk: %lu entries", gta3.count());
	const ds::PackEntry* e = gta3.find("infernus.dff");
	if (void* blob = gta3.load(e)) {
		const ds::ModelHeader* m = (const ds::ModelHeader*)blob;
		hud::log("infernus: %lu B, %u geoms %u frames", e->size, m->numGeometries, m->numFrames);
		free(blob);
	}
	gta3.close();
}

int main() {
	video::init();
	hud::init();
	timing::init();
	renderer::init();

	hud::log("boot ok");
	probeGameData();

	float yaw = 30, pitch = 25, dist = 22;

	while (pmMainLoop()) {
		timing::frameStart();
		input::poll();
		const input::State& in = input::get();
		if (in.down & KEY_START) break;
		if (in.held & KEY_LEFT) yaw -= 2;
		if (in.held & KEY_RIGHT) yaw += 2;
		if (in.held & KEY_UP && pitch < 85) pitch += 1;
		if (in.held & KEY_DOWN && pitch > 5) pitch -= 1;
		if (in.held & KEY_L && dist > 5) dist -= 0.5f;
		if (in.held & KEY_R && dist < 60) dist += 0.5f;

		video::beginFrame();
		renderer::draw(yaw, pitch, dist);
		timing::frameEnd();
		hud::drawStats();
		video::endFrame();
	}
	return 0;
}
