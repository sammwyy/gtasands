#include "game/waterlevel.h"

#include <math.h>
#include <nds.h>

#include "game/debugopts.h"
#include "game/timecycle.h"
#include "game/vehicledata.h"
#include "hud/hud.h"
#include "mem/mem.h"

namespace water {

static constexpr int32_t MAP_CU = 3000 * 128;
static constexpr int WATER_POLY_ID = 22;

struct Box { int32_t x0, y0, x1, y1; };   // cu
static ds::WaterPoly* s_poly;
static Box* s_box;
static int s_count;
static bool s_tried;
static int s_last;

bool available() {
	if (s_tried) return s_poly != nullptr;
	const ds::VehHeader* h = vehdata::header();
	if (!h) return false;
	s_tried = true;
	if (!h->numWater) return false;
	s_poly = (ds::WaterPoly*)mem::alloc(sizeof(ds::WaterPoly) * h->numWater, mem::TAG_WORLD);
	s_box = (Box*)mem::alloc(sizeof(Box) * h->numWater, mem::TAG_WORLD);
	if (!s_poly || !s_box || !vehdata::readAt(h->waterOffset, s_poly, sizeof(ds::WaterPoly) * h->numWater)) {
		mem::free(s_poly);
		mem::free(s_box);
		s_poly = nullptr;
		hud::log("\x1b[33mwater table read failed\x1b[39m");
		return false;
	}
	s_count = h->numWater;
	for (int i = 0; i < s_count; i++) {
		const ds::WaterPoly& p = s_poly[i];
		Box& b = s_box[i];
		b = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
		for (int k = 0; k < p.numVerts; k++) {
			const int32_t x = p.x[k] * 128, y = p.y[k] * 128;
			if (x < b.x0) b.x0 = x;
			if (x > b.x1) b.x1 = x;
			if (y < b.y0) b.y0 = y;
			if (y > b.y1) b.y1 = y;
		}
	}
	hud::log("water: %d polygons", s_count);
	return true;
}

// z (cu) inside polygon i, or false
static bool inside(int i, int32_t x, int32_t y, int32_t& z) {
	const Box& b = s_box[i];
	if (x < b.x0 || x > b.x1 || y < b.y0 || y > b.y1) return false;
	const ds::WaterPoly& p = s_poly[i];
	if (p.numVerts == 4) {
		// grid quad: 0 (x0, y0), 1 (x1, y0), 2 (x0, y1), 3 (x1, y1): bilinear
		const int32_t w = b.x1 - b.x0, h = b.y1 - b.y0;
		const int32_t u = w ? (int32_t)(((int64_t)(x - b.x0) << 12) / w) : 0, v = h ? (int32_t)(((int64_t)(y - b.y0) << 12) / h) : 0;
		const int32_t za = p.z[0] + (((p.z[1] - p.z[0]) * u) >> 12), zb = p.z[2] + (((p.z[3] - p.z[2]) * u) >> 12);
		z = (int32_t)((((int64_t)za << 12) + (int64_t)(zb - za) * v) >> 12) * 128 / 100;
		return true;
	}
	// triangle: same side of every edge, flat at the mean height
	int sign = 0;
	for (int e = 0; e < 3; e++) {
		const int n = e == 2 ? 0 : e + 1;
		const int64_t c = (int64_t)(p.x[n] - p.x[e]) * 128 * (y - p.y[e] * 128) - (int64_t)(p.y[n] - p.y[e]) * 128 * (x - p.x[e] * 128);
		const int s = c > 0 ? 1 : c < 0 ? -1 : 0;
		if (s && sign && s != sign) return false;
		if (s) sign = s;
	}
	z = (p.z[0] + p.z[1] + p.z[2]) / 3 * 128 / 100;
	return true;
}

bool levelCu(int32_t x, int32_t y, int32_t& z) {
	if (!available()) return false;
	if (x < -MAP_CU || x > MAP_CU || y < -MAP_CU || y > MAP_CU) { z = 0; return true; }
	if (s_last < s_count && inside(s_last, x, y, z)) return true;
	for (int i = 0; i < s_count; i++)
		if (inside(i, x, y, z)) { s_last = i; return true; }
	return false;
}

bool levelAt(float x, float y, float& z) {
	int32_t zc;
	if (!levelCu((int32_t)(x * 128.0f), (int32_t)(y * 128.0f), zc)) return false;
	z = (float)zc * (1.0f / 128.0f);
	return true;
}

bool nearest(float x, float y, float& wx, float& wy, float& wz) {
	if (!available()) return false;
	float best = 1e12f;
	for (int i = 0; i < s_count; i++) {
		if (!(s_poly[i].flags & 1)) continue;
		const Box& b = s_box[i];
		// nearest point of the box, then its middle if the box is small
		const float cx = (float)(b.x0 + b.x1) * (0.5f / 128.0f), cy = (float)(b.y0 + b.y1) * (0.5f / 128.0f);
		const float px = fminf(fmaxf(x, (float)b.x0 / 128.0f + 6.0f), (float)b.x1 / 128.0f - 6.0f);
		const float py = fminf(fmaxf(y, (float)b.y0 / 128.0f + 6.0f), (float)b.y1 / 128.0f - 6.0f);
		const float qx = b.x1 - b.x0 > 12 * 128 ? px : cx, qy = b.y1 - b.y0 > 12 * 128 ? py : cy;
		const float d = (qx - x) * (qx - x) + (qy - y) * (qy - y);
		if (d < best) {
			best = d;
			wx = qx;
			wy = qy;
			wz = (float)(s_poly[i].z[0] + s_poly[i].z[1] + s_poly[i].z[2]) / 300.0f;
		}
	}
	return best < 1e12f;
}

void draw(float ox, float oy, float oz, float ex, float ey) {
	if (!dbg::opts.water || !available()) return;
	const timecycle::Colours& tc = timecycle::current();
	const int32_t view = (dbg::opts.fogView + 20) * 128;
	const int32_t cx = (int32_t)(ex * 128.0f), cy = (int32_t)(ey * 128.0f);
	int alpha = tc.waterA >> 3;
	if (alpha < 12) alpha = 12;
	if (alpha > 26) alpha = 26;
	bool begun = false;
	for (int i = 0; i < s_count; i++) {
		const ds::WaterPoly& p = s_poly[i];
		const Box& b = s_box[i];
		if (!(p.flags & 1) || b.x1 < cx - view || b.x0 > cx + view || b.y1 < cy - view || b.y0 > cy + view) continue;
		if (!begun) {
			begun = true;
			glPushMatrix();
			glScalef(32.0f, 32.0f, 32.0f);   // v16 is +-8: 1 unit = 32 m covers the view square
			glPolyFmt(POLY_ALPHA(alpha) | POLY_CULL_NONE | POLY_ID(WATER_POLY_ID) | POLY_FOG);
			glBindTexture(GL_TEXTURE_2D, 0);
			glColor3b(tc.waterR, tc.waterG, tc.waterB);
			glBegin(GL_QUADS);
		}
		// clip the polygon's box to the view square (grid quads are axis aligned)
		const float x0 = (float)(b.x0 > cx - view ? b.x0 : cx - view) / 128.0f - ox;
		const float x1 = (float)(b.x1 < cx + view ? b.x1 : cx + view) / 128.0f - ox;
		const float y0 = (float)(b.y0 > cy - view ? b.y0 : cy - view) / 128.0f - oy;
		const float y1 = (float)(b.y1 < cy + view ? b.y1 : cy + view) / 128.0f - oy;
		const float z = (float)(p.z[0] + p.z[1] + p.z[2] + (p.numVerts == 4 ? p.z[3] : p.z[0])) / 400.0f - oz;
		// SA (x, y, z) -> GL (x, z, -y), in units of the 32 m scale
		const float k = 1.0f / 32.0f;
		const v16 gz = floattov16(z * k);
		glVertex3v16(floattov16(x0 * k), gz, floattov16(-y0 * k));
		glVertex3v16(floattov16(x1 * k), gz, floattov16(-y0 * k));
		glVertex3v16(floattov16(x1 * k), gz, floattov16(-y1 * k));
		glVertex3v16(floattov16(x0 * k), gz, floattov16(-y1 * k));
	}
	if (begun) {
		glEnd();
		glPopMatrix(1);
		glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
	}
}

}
