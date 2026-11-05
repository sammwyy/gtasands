#include "game/skidmarks.h"

#include <math.h>
#include <nds.h>

#include "game/debugopts.h"
#include "game/timecycle.h"
#include "platform/timing.h"

namespace skid {

static constexpr int MAX_MARKS = 24, PARTS = 16;
static constexpr int MAX_POLYS = 140;      // per frame: the map keeps its budget
static constexpr float DRAW_RANGE = 60.0f;
static constexpr int SKID_POLY_ID = 23;

enum State : uint8_t { INACTIVE, JUST_UPDATED, DISAPPEARING };

struct Mark {
	float pos[PARTS][3];
	float dir[PARTS][2];        // half-width side vector per part
	uint32_t id, lastMs, fadeMs, goneMs;
	uint8_t type, numParts, state;
	bool active;
};
static Mark s_marks[MAX_MARKS];
static int s_drawn;

static uint32_t nowMs() { return timing::vblanks() * 50 / 3; }

void clear() {
	for (Mark& m : s_marks) { m.state = INACTIVE; m.active = false; }
}

int active() {
	int n = 0;
	for (const Mark& m : s_marks) n += m.state != INACTIVE;
	return n;
}
int drawnPolys() { return s_drawn; }

// CSkidmark::RegisterNewPart
static void newPart(Mark& m, const float p[3], float dx, float dy, float width) {
	m.active = true;
	const uint32_t t = nowMs();
	if (t - m.lastMs <= 100) {
		for (int k = 0; k < 3; k++) m.pos[m.numParts][k] = p[k];
		return;
	}
	m.lastMs = t;
	if (m.numParts >= PARTS - 1) {
		m.state = DISAPPEARING;
		m.fadeMs = t + 10000;
		m.goneMs = t + 20000;
		return;
	}
	m.numParts++;
	for (int k = 0; k < 3; k++) m.pos[m.numParts][k] = p[k];
	const float* q = m.pos[m.numParts - 1];
	float sx = p[1] - q[1], sy = q[0] - p[0];   // perpendicular to the travel
	float l = sqrtf(sx * sx + sy * sy);
	if (l < 1e-4f) { sx = -dy; sy = dx; l = sqrtf(sx * sx + sy * sy); }
	if (l > 1e-4f) { sx /= l; sy /= l; }
	const float dl = sqrtf(dx * dx + dy * dy);
	const float d = 1.0f + (dl > 1e-4f ? fabsf((sx * dx + sy * dy) / dl) : 0.0f);
	m.dir[m.numParts][0] = d * sx * width * 0.5f;
	m.dir[m.numParts][1] = d * sy * width * 0.5f;
	if (m.numParts == 1) { m.dir[0][0] = m.dir[1][0]; m.dir[0][1] = m.dir[1][1]; }
}

void registerOne(uint32_t id, float x, float y, float z, float dx, float dy, float width, Type type) {
	if (!dbg::opts.skidmarks) return;
	const float p[3] = { x, y, z };
	for (Mark& m : s_marks)
		if (m.state == JUST_UPDATED && m.id == id) { newPart(m, p, dx, dy, width); return; }
	// CSkidmarks::GetNextFree: a free one, else the oldest finished one
	Mark* f = nullptr;
	for (Mark& m : s_marks)
		if (m.state == INACTIVE) { f = &m; break; }
	if (!f)
		for (Mark& m : s_marks)
			if (!m.active && m.state == DISAPPEARING && (!f || m.lastMs < f->lastMs)) f = &m;
	if (!f) return;
	// CSkidmark::Init
	*f = {};
	f->id = id;
	for (int k = 0; k < 3; k++) f->pos[0][k] = p[k];
	f->active = true;
	f->state = JUST_UPDATED;
	f->type = type;
	f->lastMs = nowMs() - 1000;
}

// CSkidmark::Update
void update() {
	const uint32_t t = nowMs();
	for (Mark& m : s_marks) {
		if (m.state == JUST_UPDATED && !m.active) {
			m.state = DISAPPEARING;
			const uint32_t lo = m.numParts < 4 ? 2500 : m.numParts >= 9 ? 10000 : 5000;
			m.fadeMs = t + lo;
			m.goneMs = t + lo * 2;
		} else if (m.state == DISAPPEARING && t > m.goneMs) {
			m.state = INACTIVE;
		}
		m.active = false;
	}
}

// CSkidmark::Render: one quad strip, alpha = half the mark's fade
void draw(float ox, float oy, float oz) {
	s_drawn = 0;
	if (!dbg::opts.skidmarks) return;
	const uint32_t t = nowMs();
	// CSkidmark::GetColor (the day / night balance darkens them a little at night)
	static const u8 k_col[4][3] = { { 0, 0, 0 }, { 45, 31, 4 }, { 69, 69, 61 }, { 32, 0, 0 } };
	bool begun = false;
	for (Mark& m : s_marks) {
		if (m.state == INACTIVE || !m.numParts || s_drawn + m.numParts > MAX_POLYS) continue;
		const float cx = m.pos[m.numParts / 2][0] - ox, cy = m.pos[m.numParts / 2][1] - oy;
		if (cx * cx + cy * cy > DRAW_RANGE * DRAW_RANGE) continue;
		int a = 255;
		if (m.state == DISAPPEARING && t >= m.fadeMs) a = m.goneMs > m.fadeMs ? (int)(255u * (m.goneMs - t) / (m.goneMs - m.fadeMs)) : 0;
		a = (a >> 4);   // 128/256 of the colour alpha, 5-bit
		if (a < 1) continue;
		if (!begun) {
			begun = true;
			glPushMatrix();
			glScalef(16.0f, 16.0f, 16.0f);
			glBindTexture(GL_TEXTURE_2D, 0);
		}
		glPolyFmt(POLY_ALPHA(a) | POLY_CULL_NONE | POLY_ID(SKID_POLY_ID) | POLY_FOG);
		const u8* c = k_col[m.type & 3];
		glColor3b(c[0], c[1], c[2]);
		glBegin(GL_QUAD_STRIP);
		const float k = 1.0f / 16.0f;
		for (int i = 0; i <= m.numParts; i++) {
			const float* p = m.pos[i];
			const float x = p[0] - ox, y = p[1] - oy, z = p[2] - oz + 0.06f;
			const float sx = m.dir[i][0], sy = m.dir[i][1];
			glVertex3v16(floattov16((x + sx) * k), floattov16(z * k), floattov16(-(y + sy) * k));
			glVertex3v16(floattov16((x - sx) * k), floattov16(z * k), floattov16(-(y - sy) * k));
		}
		glEnd();
		s_drawn += m.numParts;
	}
	if (begun) {
		glPopMatrix(1);
		glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
	}
}

}
