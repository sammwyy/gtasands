// Host render harness: compiles the real HUD .cpps and dumps the software
// framebuffer to a PPM so the layout can be checked without a DS.
//   hosthud <frames> <out.ppm>   — run N frames then dump (one shot)
//   hosthud shots <prefix>       — dump several interesting frames
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game/hud/chud.h"
#include "game/hud/subfb.h"
#include "game/player.h"
#include "game/streaming.h"
#include "game/text/gxt.h"
#include "platform/timing.h"

namespace timing { void tick(); }

static void dump(const char* path) {
	const u16* fb = subfb::frame();
	u8* buf = (u8*)malloc(256 * 192 * 3);
	for (int i = 0; i < 256 * 192; i++) {
		u16 c = fb[i];
		buf[i * 3 + 0] = (u8)((c & 31) << 3);
		buf[i * 3 + 1] = (u8)(((c >> 5) & 31) << 3);
		buf[i * 3 + 2] = (u8)(((c >> 10) & 31) << 3);
	}
	FILE* f = fopen(path, "wb");
	fprintf(f, "P6\n256 192\n255\n");
	fwrite(buf, 1, 256 * 192 * 3, f);
	fclose(f);
	free(buf);
	printf("wrote %s\n", path);
}

int main(int argc, char** argv) {
	// the game opens "game:/..." paths: resolve them to nitrofs/ via a local
	// symlink (gitignored — recreated here so it never needs committing)
	if (symlink("nitrofs", "game:") != 0 && errno != EEXIST)
		printf("WARN: can't create game: symlink (%s)\n", strerror(errno));
	if (!TheText.load("game:/text/american.gxt")) printf("WARN: gxt missing\n");
	if (!streaming::g_models.open("game:/models/gta3.dspk")) printf("WARN: dspk missing\n");
	subfb::init();
	player::init();
	if (!chud::init()) { printf("chud init failed\n"); return 1; }

	if (argc >= 2 && !strcmp(argv[1], "shots")) {
		const int frames[] = { 100, 260, 400, 700, 1100, 1500 };
		for (unsigned k = 0; k < sizeof frames / sizeof frames[0]; k++) {
			while (timing::vblanks() < (u32)frames[k]) {
				timing::tick();
				player::update();
				chud::draw();
			}
			char name[64];
			snprintf(name, sizeof name, "%s%04d.ppm", argv[2] ? argv[2] : "shot", frames[k]);
			dump(name);
		}
		return 0;
	}

	int n = argc > 1 ? atoi(argv[1]) : 60;
	for (int i = 0; i < n; i++) { timing::tick(); player::update(); chud::draw(); }
	char buf[160];
	chud::stats(buf, sizeof buf);
	printf("%s\n", buf);
	dump(argc > 2 ? argv[2] : "hud.ppm");
	return 0;
}
