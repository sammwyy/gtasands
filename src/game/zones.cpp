#include "game/zones.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

namespace zones {

struct Zone { float x1, y1, x2, y2; char gxt[10]; };
static Zone* s_zones;
static int s_count, s_cap;
static bool s_loaded;

// info.zon lines: NAME, type, x1,y1,z1, x2,y2,z2, island, GXTKEY
static void load() {
	s_loaded = true;
	FILE* f = fopen("nitro:/data/info.zon", "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		char* p = strchr(line, ',');
		if (!p) continue;
		float x1, y1, x2, y2;
		// fields: skip name, type; grab x1 y1 z1 x2 y2 z2 island gxtkey
		char name[12], gkey[12];
		int type, island;
		float z1, z2;
		if (sscanf(line, "%11[^,], %d, %f, %f, %f, %f, %f, %f, %d, %11[^,\r\n]",
		           name, &type, &x1, &y1, &z1, &x2, &y2, &z2, &island, gkey) != 10)
			continue;
		if (type != 0) continue; // only info zones (not gang/navig fields)
		if (s_count == s_cap) {
			s_cap = s_cap ? s_cap * 2 : 512;
			s_zones = (Zone*)realloc(s_zones, s_cap * sizeof(Zone));
		}
		Zone& z = s_zones[s_count++];
		z.x1 = x1; z.y1 = y1; z.x2 = x2; z.y2 = y2;
		strncpy(z.gxt, gkey, sizeof z.gxt - 1);
		z.gxt[sizeof z.gxt - 1] = 0;
	}
	fclose(f);
}

const GxtChar* nameAt(float x, float y) {
	if (!s_loaded) load();
	const Zone* best = nullptr;
	float bestArea = 1e30f;
	for (int i = 0; i < s_count; i++) {
		const Zone& z = s_zones[i];
		if (x < z.x1 || x > z.x2 || y < z.y1 || y > z.y2) continue;
		float a = (z.x2 - z.x1) * (z.y2 - z.y1);
		if (a < bestArea) { bestArea = a; best = &z; }
	}
	return best ? TheText.get(best->gxt) : nullptr;
}

}
