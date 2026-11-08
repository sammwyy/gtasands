#include "game/zones.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "mem/mem.h"

namespace zones {

struct Zone { int32_t x1, y1, x2, y2; char gxt[10]; };   // 1/16 m: integer lookups
static Zone* s_zones;
static int s_count;
static bool s_loaded;

// info.zon lines: NAME, type, x1,y1,z1, x2,y2,z2, island, GXTKEY
static bool parse(const char* line, Zone& z) {
	float x1, y1, x2, y2, z1, z2;
	char name[12], gkey[12];
	int type, island;
	// fields: skip name, type; grab x1 y1 z1 x2 y2 z2 island gxtkey
	if (sscanf(line, "%11[^,], %d, %f, %f, %f, %f, %f, %f, %d, %11[^,\r\n]",
	           name, &type, &x1, &y1, &z1, &x2, &y2, &z2, &island, gkey) != 10 || type != 0)
		return false;   // only info zones (not gang/navig fields)
	z.x1 = (int32_t)(x1 * 16.0f); z.y1 = (int32_t)(y1 * 16.0f);
	z.x2 = (int32_t)(x2 * 16.0f); z.y2 = (int32_t)(y2 * 16.0f);
	strncpy(z.gxt, gkey, sizeof z.gxt - 1);
	z.gxt[sizeof z.gxt - 1] = 0;
	return true;
}

// Two passes over the file (count, then fill) so the table is one exact PERM
// block instead of a growing realloc'd array.
static void load() {
	s_loaded = true;
	FILE* f = fopen("nitro:/data/info.zon", "r");
	if (!f) return;
	char line[256];
	Zone z;
	int n = 0;
	while (fgets(line, sizeof line, f))
		if (strchr(line, ',') && parse(line, z)) n++;
	s_zones = n ? (Zone*)mem::perm(n * sizeof(Zone), mem::TAG_WORLD) : nullptr;
	fseek(f, 0, SEEK_SET);
	while (s_zones && s_count < n && fgets(line, sizeof line, f))
		if (strchr(line, ',') && parse(line, s_zones[s_count])) s_count++;
	fclose(f);
}

const GxtChar* nameAt(float x, float y) {
	if (!s_loaded) load();
	const Zone* best = nullptr;
	int64_t bestArea = INT64_MAX;
	const int32_t ix = (int32_t)(x * 16.0f), iy = (int32_t)(y * 16.0f);
	for (int i = 0; i < s_count; i++) {
		const Zone& z = s_zones[i];
		if (ix < z.x1 || ix > z.x2 || iy < z.y1 || iy > z.y2) continue;
		const int64_t a = (int64_t)(z.x2 - z.x1) * (z.y2 - z.y1);
		if (a < bestArea) { bestArea = a; best = &z; }
	}
	return best ? TheText.get(best->gxt) : nullptr;
}

void preload() {
	if (!s_loaded) load();
}

}
