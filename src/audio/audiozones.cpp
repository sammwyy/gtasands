#include "audio/audiozones.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hud/hud.h"
#include "mem/mem.h"

namespace audiozones {

static constexpr int MAX_ZONES = 192;   // the PC arrays hold 3000 boxes / 1000 spheres; audiozon.ipl has ~160

struct Zone {
	char name[8];
	int16_t id;
	bool active, sphere;
	float a[3], b[3];        // box min / max, or sphere centre + radius in b[0]
};

static Zone* s_zones;
static int s_n;
static int s_activeSphere = -1, s_activeBox = -1;
static float s_last[3] = { 1e9f, 1e9f, 1e9f };
static uint32_t s_frame;

bool load() {
	if (s_zones) return true;
	s_zones = (Zone*)mem::perm(MAX_ZONES * sizeof(Zone), mem::TAG_WORLD);
	if (!s_zones) return true;
	FILE* f = fopen("nitro:/data/maps/audiozon.ipl", "rb");
	if (!f) {
		hud::log("\x1b[33maudiozon.ipl missing\x1b[39m");
		return true;
	}
	char line[160];
	bool inAuzo = false;
	while (fgets(line, sizeof line, f) && s_n < MAX_ZONES) {
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
		if (!strncasecmp(p, "auzo", 4)) { inAuzo = true; continue; }
		if (!strncasecmp(p, "end", 3)) { inAuzo = false; continue; }
		if (!inAuzo) continue;
		// "name, id, enabled, x1, y1, z1, x2, y2, z2" or "name, id, enabled, x, y, z, radius"
		char* tok[10];
		int nt = 0;
		for (char* t = strtok(p, ",\r\n"); t && nt < 10; t = strtok(nullptr, ",\r\n")) tok[nt++] = t;
		if (nt != 9 && nt != 7) continue;
		Zone& z = s_zones[s_n++];
		memset(&z, 0, sizeof z);
		char* nm = tok[0];
		while (*nm == ' ') nm++;
		strncpy(z.name, nm, 7);
		for (int i = 6; i >= 0 && (z.name[i] == ' ' || !z.name[i]); i--) z.name[i] = 0;
		z.id = (int16_t)atoi(tok[1]);
		z.active = atoi(tok[2]) != 0;
		z.sphere = nt == 7;
		for (int i = 0; i < 3; i++) z.a[i] = (float)atof(tok[3 + i]);
		if (z.sphere) z.b[0] = (float)atof(tok[6]);
		else for (int i = 0; i < 3; i++) z.b[i] = (float)atof(tok[6 + i]);
	}
	fclose(f);
	hud::log("audio zones: %d", s_n);
	return true;
}

void update(bool force, float x, float y, float z) {
	const float dx = x - s_last[0], dy = y - s_last[1], dz = z - s_last[2];
	if (!force && (s_frame++ % 16) != 6 && dx * dx + dy * dy + dz * dz < 400.0f) return;
	s_last[0] = x; s_last[1] = y; s_last[2] = z;
	s_activeSphere = s_activeBox = -1;
	for (int i = 0; i < s_n; i++) {
		const Zone& q = s_zones[i];
		if (!q.active) continue;
		if (q.sphere) {
			const float ex = x - q.a[0], ey = y - q.a[1], ez = z - q.a[2];
			if (s_activeSphere < 0 && ex * ex + ey * ey + ez * ez <= q.b[0] * q.b[0]) s_activeSphere = i;
		} else if (s_activeBox < 0 && x >= q.a[0] && x <= q.b[0] && y >= q.a[1] && y <= q.b[1] && z >= q.a[2] && z <= q.b[2]) {
			s_activeBox = i;
		}
	}
}

int activeZone() {
	if (s_activeSphere >= 0) return s_zones[s_activeSphere].id;
	if (s_activeBox >= 0) return s_zones[s_activeBox].id;
	return -1;
}

bool activeSphere(float& x, float& y, float& z) {
	if (s_activeSphere < 0) return false;
	x = s_zones[s_activeSphere].a[0];
	y = s_zones[s_activeSphere].a[1];
	z = s_zones[s_activeSphere].a[2];
	return true;
}

int count() { return s_n; }

void switchZone(const char* name, bool enable) {
	for (int i = 0; i < s_n; i++)
		if (!strcasecmp(name, s_zones[i].name)) s_zones[i].active = enable;
}

}
