#include "render/sky.h"

#include <math.h>
#include <nds.h>
#include <stdlib.h>
#include <string.h>

#include "assets/texture.h"
#include "assets/txdfile.h"
#include "game/debugopts.h"
#include "game/player.h"
#include "game/timecycle.h"
#include "game/weather.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "platform/video.h"

namespace sky {

// ---- constants ------------------------------------------------------------------

static constexpr float SKYP_WIDTH_MULT = 1.4f;       // Clouds.h, distances scaled to 2 units
static constexpr float SKYP_CAM_DIST = 2.0f;
static constexpr float SKYP_ABOVE_HORIZON_Z = 0.5f;
static constexpr float SKYP_HORIZON_Z = 0.0f;
static constexpr float SKYP_SEA_HORIZON_Z = -0.1f;
static constexpr float SKYP_BELOW_HORIZON_Z = -0.3f;

// Sprites sit SPRITE_DIST units from the eye. CSprite::CalcScreenCoors sizes
// are in SCREEN_WIDTH/z units, i.e. a sprite of size s at distance z spans
// 2s/z of the screen width; on our frustum that is s/z * 2*tan(hfov/2).
static constexpr float SPRITE_DIST = 2.0f;
static constexpr float SPRITE_K = SPRITE_DIST * 2.0f * video::TAN_HALF_FOV_X;
static constexpr float SA_SCREEN_W = 640.0f;          // 2D flare sizes are in 640-wide pixels

// Constant-depth projections: z_clip = -k*z_eye, w = -z_eye -> z/w = k.
// Sky polys at 4095/4096, sprites a step nearer so they win over the sky.
static constexpr s32 DEPTH_SKY = 4095;
static constexpr s32 DEPTH_SPRITES = 4094;

static tex::Texture* s_star;    // coronastar (shiny star: sun, stars, rainbow)
static tex::Texture* s_moon;    // coronamoon
static tex::Texture* s_mask;    // lunar (moon phase mask)
static tex::Texture* s_cloud;   // cloud1

// Camera basis in GL world-rotated space (x east, y up, z south).
static float s_r[3], s_u[3], s_f[3];
static int s_polyId;

// ---- helpers ------------------------------------------------------------------

static inline float rnd(float a, float b) { return a + (b - a) * (float)(rand() & 1023) * (1.0f / 1023.0f); }
static inline u16 rgb(int r, int g, int b) { return RGB15(r >> 3, g >> 3, b >> 3); }
static inline void saToGl(const float* sa, float* gl) { gl[0] = sa[0]; gl[1] = sa[2]; gl[2] = -sa[1]; }
static inline void vtx(float x, float y, float z) { glVertex3v16(floattov16(x), floattov16(y), floattov16(z)); }
static inline float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static inline int nextId() {
	s_polyId = s_polyId >= 62 ? 1 : s_polyId + 1;
	return s_polyId;
}

static void projection(s32 depthMul) {
	const s32 fy = floattof32(1.0f / video::TAN_HALF_FOV_Y), fx = floattof32(1.0f / video::TAN_HALF_FOV_X);
	const m4x4 m = { { fx, 0, 0, 0, 0, fy, 0, 0, 0, 0, -depthMul, -4096, 0, 0, 0, 0 } };
	glMatrixMode(GL_PROJECTION);
	glLoadMatrix4x4(&m);
	glMatrixMode(GL_MODELVIEW);
}

static void cameraRotation(const View& v) {
	glLoadIdentity();
	glRotatef(v.pitch * 57.2958f, 1.0f, 0.0f, 0.0f);
	glRotatef(-v.yaw * 57.2958f, 0.0f, 1.0f, 0.0f);
}

// Billboard at GL direction `d` (normalised): centre d*SPRITE_DIST, half
// extents hw/hh in GL units along the camera right/up axes.
static void quad(const float* d, float hw, float hh, u16 color, int alpha, const tex::Texture* t) {
	if (!t || alpha <= 0 || dot(d, s_f) <= 0.02f) return;
	if (hw > 3.0f) hw = 3.0f;
	if (hh > 3.0f) hh = 3.0f;
	const float c[3] = { d[0] * SPRITE_DIST, d[1] * SPRITE_DIST, d[2] * SPRITE_DIST };
	glPolyFmt(POLY_ALPHA(alpha > 31 ? 31 : alpha) | POLY_CULL_NONE | POLY_ID(nextId()));
	glBindTexture(GL_TEXTURE_2D, t->id);
	glColor(color);
	const t16 tw = inttot16(t->w), th = inttot16(t->h);
	static const s8 k_corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	glBegin(GL_QUADS);
	for (int i = 0; i < 4; i++) {
		const float sx = k_corner[i][0] * hw, sy = k_corner[i][1] * hh;
		glTexCoord2t16(k_corner[i][0] > 0 ? tw : 0, k_corner[i][1] > 0 ? 0 : th);
		vtx(c[0] + s_r[0] * sx + s_u[0] * sy, c[1] + s_r[1] * sx + s_u[1] * sy, c[2] + s_r[2] * sx + s_u[2] * sy);
	}
	glEnd();
}

// RenderOneXLUSprite with blend ONE/ONE: colour r,g,b (0..255) is added to
// the frame. Here: alpha = brightest channel, colour = normalised. `dirSA` is
// the SA direction (any length), w/h are CalcScreenCoors size / distance.
static void additive(const float* dirSA, float w, float h, int r, int g, int b, const tex::Texture* t) {
	const int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
	if (m < 8) return;
	float d[3];
	saToGl(dirSA, d);
	const float inv = 1.0f / sqrtf(dot(d, d));
	for (float& x : d) x *= inv;
	quad(d, w * SPRITE_K, h * SPRITE_K, RGB15(r * 31 / m, g * 31 / m, b * 31 / m), (m * 31 + 127) / 255, t);
}

static inline float length3(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// CalculateColorWithBalance
static inline int balance(int c, float t) { return (int)((float)c * (1.0f - t)); }

// ---- loading --------------------------------------------------------------------

const tex::Texture* glowTexture() { return s_star; }
const tex::Texture* puffTexture() { return s_cloud; }

bool loadStep() {
	if (s_star) return true;
	TxdFile txd;
	if (!txd.open("nitro:/models/particle.txd")) {
		hud::log("\x1b[31msky: particle.txd missing\x1b[39m");
		return true;
	}
	auto load = [&](const char* name) {
		mem::Scratch scratch;
		return tex::load(txd.load(name, scratch), tex::GROUP_GAME);
	};
	s_star = load("coronastar");
	s_moon = load("coronamoon");
	s_mask = load("lunar");
	s_cloud = load("cloud1");
	hud::log("sky: star %d moon %d mask %d cloud %d", !!s_star, !!s_moon, !!s_mask, !!s_cloud);
	return true;
}

// ---- CClouds::RenderSkyPolys ----------------------------------------------------

static void skyPoly(const float* v1, const float* v2, const float* v3, const float* v4, u16 top, u16 bottom) {
	// SetUpOneSkyPoly: v1 v2 top colour, v3 v4 bottom; triangles (0,2,1) (1,2,3)
	glColor(top);    vtx(v1[0], v1[2], -v1[1]);
	glColor(top);    vtx(v2[0], v2[2], -v2[1]);
	glColor(bottom); vtx(v4[0], v4[2], -v4[1]);
	glColor(bottom); vtx(v3[0], v3[2], -v3[1]);
}

static void renderSkyPolys(const View& v, const timecycle::Colours& c) {
	const float norm[2] = { -sinf(v.yaw), cosf(v.yaw) };   // camera forward, horizontal
	float blendF = (v.camZ - 25.0f) * (1.0f / 80.0f);
	blendF = blendF < 0.0f ? 0.0f : blendF > 1.0f ? 1.0f : blendF;
	if (blendF < weather::g.foggyness) blendF = weather::g.foggyness;
	u8 gr, gg, gb;
	timecycle::belowHorizonGrey(gr, gg, gb);
	gr = (u8)(gr + blendF * (c.skyBotR - gr));
	gg = (u8)(gg + blendF * (c.skyBotG - gg));
	gb = (u8)(gb + blendF * (c.skyBotB - gb));

	const u16 top = rgb(c.skyTopR, c.skyTopG, c.skyTopB);
	const u16 bot = rgb(c.skyBotR, c.skyBotG, c.skyBotB);
	const u16 grey = rgb(gr, gg, gb);

	const float sx = norm[1] * SKYP_WIDTH_MULT, sy = -norm[0] * SKYP_WIDTH_MULT;
	const float D = SKYP_CAM_DIST;
	const float p0[2] = { (-norm[0] - sx) * D, (-norm[1] - sy) * D };   // behind
	const float p1[2] = { (-norm[0] + sx) * D, (-norm[1] + sy) * D };
	const float p2[2] = { (norm[0] - sx) * D, (norm[1] - sy) * D };     // in front
	const float p3[2] = { (norm[0] + sx) * D, (norm[1] + sy) * D };
	const float z[6] = { SKYP_ABOVE_HORIZON_Z * D, SKYP_ABOVE_HORIZON_Z * D, SKYP_HORIZON_Z * D,
	                     SKYP_SEA_HORIZON_Z * D, SKYP_BELOW_HORIZON_Z * D, SKYP_BELOW_HORIZON_Z * D };

	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(0));
	glBindTexture(GL_TEXTURE_2D, 0);
	glBegin(GL_QUADS);
	{
		const float a[3] = { p2[0], p2[1], z[1] }, b[3] = { p3[0], p3[1], z[1] };
		const float cc[3] = { p2[0], p2[1], z[2] }, d[3] = { p3[0], p3[1], z[2] };
		skyPoly(a, b, cc, d, top, bot);                       // sky top -> horizon
	}
	{
		const float a[3] = { p2[0], p2[1], z[2] }, b[3] = { p3[0], p3[1], z[2] };
		const float cc[3] = { p2[0], p2[1], z[3] }, d[3] = { p3[0], p3[1], z[3] };
		skyPoly(a, b, cc, d, bot, bot);                       // horizon band
	}
	{
		const float a[3] = { p2[0], p2[1], z[3] }, b[3] = { p3[0], p3[1], z[3] };
		const float cc[3] = { p2[0], p2[1], z[4] }, d[3] = { p3[0], p3[1], z[4] };
		skyPoly(a, b, cc, d, bot, grey);                      // horizon -> below
	}
	{
		const float a[3] = { p2[0], p2[1], z[1] }, b[3] = { p3[0], p3[1], z[1] };
		const float cc[3] = { p0[0], p0[1], z[0] }, d[3] = { p1[0], p1[1], z[0] };
		skyPoly(a, b, cc, d, top, top);                       // ceiling
	}
	{
		const float a[3] = { p2[0], p2[1], z[4] }, b[3] = { p3[0], p3[1], z[4] };
		const float cc[3] = { p0[0], p0[1], z[5] }, d[3] = { p1[0], p1[1], z[5] };
		skyPoly(a, b, cc, d, grey, grey);                     // floor
	}
	glEnd();
}

// ---- CClouds::Render pieces ------------------------------------------------------

static void renderMoon(const timecycle::Colours& c, float colorBalance) {
	constexpr unsigned RANGE = 3 * 60 + 40;   // MOON_VISIBILITY_RANGE_MINS
	const unsigned vis = (unsigned)fabsf(player::minutesToday() - (float)RANGE);
	if (vis >= RANGE) return;
	const int colorB = (int)(RANGE - vis);
	const int colorRG = balance(colorB, colorBalance);
	if (colorRG == 0) return;
	static const float k_off[3] = { 0.0f, -100.0f, 15.0f };   // CAMERA_TO_CLOUD_OFFSET
	const float dist = length3(k_off);
	const float moonSz = (3 * 2 + 4) / dist;                  // CCoronas::MoonSize * 2 + 4
	additive(k_off, moonSz, moonSz, colorRG, colorRG, (int)(colorB * 0.85f), s_moon);

	// Phase: the original masks the moon through dest alpha with "lunar"
	// gliding across it over the month. Without dest alpha the mask is drawn
	// on top in the sky colour around the moon (sky gradient at its altitude).
	if (!s_mask) return;
	float d[3];
	saToGl(k_off, d);
	for (float& x : d) x /= dist;
	const float maskSz = moonSz * 1.7f * SPRITE_K;
	const float dx = 0.7f * maskSz;
	const float dy = -5.4f * ((float)player::g_clockDay / 31.0f - 0.5f) * maskSz;   // screen y is down
	const float elev = k_off[2] / dist / SKYP_ABOVE_HORIZON_Z;                         // 0 horizon .. 1 sky top
	auto mix = [&](u8 b, u8 t) { return (int)(b + (t - b) * elev); };
	const float cMask[3] = { d[0] * SPRITE_DIST + s_r[0] * dx + s_u[0] * dy,
	                         d[1] * SPRITE_DIST + s_r[1] * dx + s_u[1] * dy,
	                         d[2] * SPRITE_DIST + s_r[2] * dx + s_u[2] * dy };
	float dm[3] = { cMask[0], cMask[1], cMask[2] };
	const float il = 1.0f / length3(dm);
	for (float& x : dm) x *= il;
	quad(dm, maskSz, maskSz, rgb(mix(c.skyBotR, c.skyTopR), mix(c.skyBotG, c.skyTopG), mix(c.skyBotB, c.skyTopB)), 31, s_mask);
}

static void renderRockstarLogo() {
	static const float k_y[9] = { 0.00f, 0.05f, 0.13f, 0.40f, 0.70f, 0.60f, 0.27f, 0.55f, 0.75f };
	static const float k_z[9] = { 0.00f, 0.45f, 0.90f, 1.00f, 0.85f, 0.52f, 0.48f, 0.35f, 0.20f };
	static const float k_s[9] = { 1.00f, 1.40f, 0.90f, 1.00f, 0.60f, 1.50f, 1.30f, 1.00f, 0.80f };
	const unsigned h = player::g_clockH, m = player::g_clockM;
	if (!(h >= 22 || h < 6)) return;   // GetIsTimeInRange(22, 6)
	const unsigned t = h == 22 ? m : h == 5 ? 60 - m : 60;
	if (!t) return;
	const int colorB = (int)(255 * t / 60);
	for (int i = 0; i < 11; i++) {
		const int k = i % 9;
		const float off[3] = { i >= 9 ? -100.0f : 100.0f, -k_y[k] * 90.0f, 10.0f + k_z[k] * 80.0f };
		const float dist = length3(off);
		const int cc = balance(colorB, (float)(rand() % 32) * 0.015f);
		const float s = k_s[k] * 0.8f / dist;
		additive(off, s, s, cc, cc, cc, s_star);
	}
	static const float k_big[3] = { 100.0f, -90.0f, 10.0f };
	const int cc = balance(colorB, (float)(rand() % 128) * 0.0015625f + 0.5f);
	const float s = 5.0f / length3(k_big);
	additive(k_big, s, s, cc, cc, cc, s_star);
}

static void renderLowClouds(const View& v, const timecycle::Colours& c, float colorBalance) {
	static const float k_coords[12][3] = {
		{ 1.0f, 0.0f, 0.0f }, { 0.7f, -0.7f, 1.0f }, { 0.0f, -1.0f, 0.5f }, { -0.7f, -0.7f, 0.0f },
		{ -1.0f, 0.0f, 1.0f }, { -0.7f, 0.7f, 0.3f }, { 0.0f, 1.0f, 0.9f }, { 0.7f, 0.7f, 0.4f },
		{ 0.8f, 0.4f, 1.3f }, { -0.8f, 0.4f, 1.4f }, { 0.4f, -0.8f, 1.2f }, { -0.4f, -0.8f, 1.7f } };
	const int r = balance(c.lowCloudR, colorBalance), g = balance(c.lowCloudG, colorBalance),
	          b = balance(c.lowCloudB, colorBalance);
	for (const auto& o : k_coords) {
		// camPos.z is zeroed before CalcScreenCoors: clouds hang at 40..142 m absolute
		const float off[3] = { o[0] * 800.0f, o[1] * 800.0f, o[2] * 60.0f + 40.0f - v.camZ };
		const float dist = length3(off);
		additive(off, 320.0f / dist, 40.0f / dist, r, g, b, s_cloud);
	}
}

static void renderRainbow() {
	static const u8 k_col[6][3] = { { 30, 0, 0 }, { 30, 15, 0 }, { 30, 30, 0 }, { 10, 30, 10 }, { 0, 0, 30 }, { 15, 0, 30 } };
	const float rb = weather::g.rainbow;
	if (rb == 0.0f) return;
	for (int i = 0; i < 6; i++) {
		const float off[3] = { i * 1.5f, 100.0f, 5.0f };
		const float dist = length3(off);
		additive(off, 2.0f / dist, 50.0f / dist, (int)(k_col[i][0] * rb), (int)(k_col[i][1] * rb),
		         (int)(k_col[i][2] * rb), s_star);
	}
}

// Shooting star between 0 and 5 AM in extra sunny weather (a polyline on PC,
// a GE line here: triangle with a repeated vertex).
static void renderStreaks(const timecycle::Colours& c) {
	if (player::g_clockH >= 5) return;
	if (!weather::isExtraSunny(weather::g.oldType) || !weather::isExtraSunny(weather::g.newType)) return;
	const u32 timeMs = timing::vblanks() * 50u / 3u;
	const u32 delta = timeMs % 8192u;
	if (delta >= 800u) return;
	const u32 r64 = (timeMs / 8192u) % 64u;
	float size[3] = { (float)((int)(r64 % 7) - 3) * 0.1f, (float)((int)((timeMs & 0xFFFF) / 8192u) - 4) * 0.1f, 1.0f };
	float dir[3] = { (float)((int)(r64 % 9) - 5), (float)((int)(r64 % 10) - 5), 0.1f };
	const float is = 1.0f / length3(size), id = 1.0f / length3(dir);
	for (int i = 0; i < 3; i++) { size[i] *= is; dir[i] *= id; }
	const float scale = (float)((400 - (int)delta) * 2);
	float a[3], b[3], ga[3], gb[3];
	for (int i = 0; i < 3; i++) {
		a[i] = dir[i] * 1000.0f + size[i] * scale;
		b[i] = dir[i] * 1000.0f + size[i] * (scale + 50.0f);
	}
	saToGl(a, ga);
	saToGl(b, gb);
	const float la = SPRITE_DIST / length3(ga), lb = SPRITE_DIST / length3(gb);
	glPolyFmt(POLY_ALPHA(28) | POLY_CULL_NONE | POLY_ID(nextId()));
	glBindTexture(GL_TEXTURE_2D, 0);
	glBegin(GL_TRIANGLES);
	glColor(RGB15(31, 31, 31));
	vtx(ga[0] * la, ga[1] * la, ga[2] * la);
	glColor(rgb(c.skyTopR, c.skyTopG, c.skyTopB));   // alpha 225 -> 0 along the trail
	vtx(gb[0] * lb, gb[1] * lb, gb[2] * lb);
	vtx(gb[0] * lb, gb[1] * lb, gb[2] * lb);
	glEnd();
}

// ---- CCoronas::DoSunAndMoon + corona/flare render --------------------------------

struct Flare { float pos, size; u8 r, g, b; };
static const Flare k_sunFlare[] = {   // SunFlareDef (ColorMult x256)
	{ 4.00f, 8.00f, 36, 30, 24 }, { 3.00f, 11.2f, 24, 18, 15 }, { 2.00f, 8.00f, 24, 12, 12 },
	{ 1.50f, 9.60f, 54, 54, 48 }, { 1.25f, 8.00f, 24, 24, 18 }, { 0.80f, 22.4f, 36, 30, 24 },
	{ 0.60f, 6.40f, 24, 15, 12 }, { 0.25f, 16.0f, 36, 30, 30 }, { 0.10f, 9.60f, 18, 18, 18 },
	{ 0.05f, 22.4f, 36, 30, 24 }, { -0.03f, 4.80f, 18, 18, 18 }, { -0.10f, 9.60f, 42, 42, 42 },
	{ -0.30f, 8.00f, 18, 6, 6 }, { -0.40f, 96.0f, 18, 12, 9 }, { -0.55f, 6.40f, 18, 18, 12 },
	{ -0.75f, 22.4f, 42, 24, 18 }, { -0.90f, 8.32f, 21, 12, 18 }, { -1.00f, 17.6f, 42, 13, 18 },
	{ -1.20f, 5.60f, 21, 12, 12 }, { -1.35f, 14.4f, 42, 42, 24 }, { -1.70f, 86.8f, 21, 15, 15 },
	{ -2.00f, 8.00f, 48, 30, 30 }, { -2.50f, 7.20f, 21, 15, 12 }, { -3.00f, 22.4f, 42, 30, 24 },
	{ -6.00f, 38.4f, 42, 42, 30 }, { -9.00f, 22.4f, 42, 30, 36 },
};

static void renderSun(const timecycle::Colours& c) {
	const float* s = timecycle::vectorToSun();
	if (s[2] <= -0.1f) return;
	const float dist = (c.farClip > 10 ? c.farClip : 10) * 0.95f;
	const float scale = 1.0f + weather::g.foggyness;   // min(40, z) * Foggyness / 40 + 1
	const float sz = c.sunSize / dist;                  // m_fSunSize keeps the x10 storage, like the original
	// corona 1: core colour, 2.7335x, no LOS test
	additive(s, sz * 2.7335f, sz * 2.7335f * scale, (int)(c.sunCoreR / scale), (int)(c.sunCoreG / scale),
	         (int)(c.sunCoreB / scale), s_star);
	if (s[2] <= 0.0f) return;
	// corona 2: corona colour, 6x, with the sun flare
	additive(s, sz * 6.0f, sz * 6.0f * scale, (int)(c.sunCoronaR / scale), (int)(c.sunCoronaG / scale),
	         (int)(c.sunCoronaB / scale), s_star);

	// lens flare along the screen-centre -> sun line (no world LOS yet)
	float d[3];
	saToGl(s, d);
	const float vz = dot(d, s_f);
	if (vz <= 0.05f) return;
	const float tx = dot(d, s_r) / vz, ty = dot(d, s_u) / vz;
	if (fabsf(tx) > video::TAN_HALF_FOV_X || fabsf(ty) > video::TAN_HALF_FOV_Y) return;
	const float var = rnd(0.7f, 1.0f) * (255.0f / 256.0f);
	for (const Flare& fl : k_sunFlare) {
		const int r = (int)(c.sunCoronaR * var * fl.r / 256.0f), g = (int)(c.sunCoronaG * var * fl.g / 256.0f),
		          b = (int)(c.sunCoronaB * var * fl.b / 256.0f);
		const int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
		if (m < 8) continue;
		float p[3];
		for (int i = 0; i < 3; i++) p[i] = s_f[i] + s_r[i] * tx * fl.pos + s_u[i] * ty * fl.pos;
		const float il = 1.0f / length3(p);
		for (float& x : p) x *= il;
		// keep the on-screen size: the quad sits nearer than SPRITE_DIST off-axis
		const float half = fl.size * 4.0f / SA_SCREEN_W * 2.0f * video::TAN_HALF_FOV_X * SPRITE_DIST * il;
		quad(p, half, half, RGB15(r * 31 / m, g * 31 / m, b * 31 / m), (m * 31 + 127) / 255, s_star);
	}
}

// ---- fog ---------------------------------------------------------------------------

// RW linear fog from fogStart to farClip in the sky-bottom colour. The DS
// sees far less than the timecyc far clip (800 m on a clear day), so both
// distances are scaled to the DS view distance (dbg::opts.fogView): the fog
// still closes where the DS world ends, and the weather still sets how
// thick it is (FOGGY_SF: far 150 / start -200 -> already 57% at the camera).
// Recomputed only when the inputs change (32 divisions), applied in vblank.
static void setupFog(const timecycle::Colours& c) {
	static s16 s_lastFar = -32768, s_lastStart;
	static int s_lastView;
	static video::Fog s_fog;
	const u16 col = rgb(c.skyBotR, c.skyBotG, c.skyBotB);
	if (c.farClip != s_lastFar || c.fogStart != s_lastStart || dbg::opts.fogView != s_lastView) {
		s_lastFar = c.farClip;
		s_lastStart = c.fogStart;
		s_lastView = dbg::opts.fogView;
		const float view = (float)(dbg::opts.fogView < video::FAR_PLANE ? dbg::opts.fogView : video::FAR_PLANE);
		const float farClip = c.farClip > 10 ? (float)c.farClip : 10.0f;
		const float end = farClip < view ? farClip : view;
		const float start = (float)c.fogStart * (end / farClip);
		const int d0 = video::depth15(start > video::NEAR_PLANE ? start : video::NEAR_PLANE);
		const int d1 = video::depth15(end);
		const int range = d1 - d0 > 1 ? d1 - d0 : 1;
		int shift = 10;
		while (shift > 0 && (0x400 >> shift) * 31 < range) shift--;
		const int step = 0x400 >> shift;
		s_fog.shift = (u8)shift;
		s_fog.offset = (u16)d0;
		for (int i = 0; i < 32; i++) {
			const float dist = video::distForDepth15(d0 + i * step);
			float t = (dist - start) / (end - start);
			t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
			s_fog.table[i] = (u8)(t * 127.0f);
		}
	}
	s_fog.color = col;
	video::setFog(&s_fog);
}

// ---- frame -------------------------------------------------------------------------

void draw(const View& v) {
	const timecycle::Colours& c = timecycle::current();
	video::setManualSort(true);
	video::setClearColor(rgb(c.skyBotR, c.skyBotG, c.skyBotB));
	if (dbg::opts.fog) setupFog(c);

	// camera basis (R = Rx(pitch) Ry(-yaw); rows of R^T)
	const float sy = sinf(v.yaw), cy = cosf(v.yaw), sp = sinf(v.pitch), cp = cosf(v.pitch);
	s_r[0] = cy;        s_r[1] = 0.0f; s_r[2] = -sy;
	s_u[0] = -sp * sy;  s_u[1] = cp;   s_u[2] = -sp * cy;
	s_f[0] = -cp * sy;  s_f[1] = -sp;  s_f[2] = -cp * cy;

	if (!dbg::opts.sky) return;
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	projection(DEPTH_SKY);
	cameraRotation(v);
	renderSkyPolys(v, c);

	if (dbg::opts.skySprites) {
		projection(DEPTH_SPRITES);
		const weather::State& w = weather::g;
		const float colorBalance = w.foggyness > w.cloudCoverage ? w.foggyness : w.cloudCoverage;
		renderMoon(c, colorBalance);
		renderRockstarLogo();
		renderLowClouds(v, c, colorBalance > w.extraSunnyness ? colorBalance : w.extraSunnyness);
		renderRainbow();
		renderStreaks(c);
		renderSun(c);
	}

	glMatrixMode(GL_PROJECTION);
	glPopMatrix(1);
	glMatrixMode(GL_MODELVIEW);
	glBindTexture(GL_TEXTURE_2D, 0);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

// ---- weather effects ------------------------------------------------------------------
// CWeather::RenderRainStreaks draws 32 lines that respawn at random and
// jitter every frame: on a 256x192 screen that reads as noise. Instead the
// drops live in world space inside a box that wraps around the camera (an
// infinite, stable volume), fall with gravity + CWeather::WindDir, leave a
// motion-blur streak and splash on the ground. Same for the sandstorm dust
// (AddSandStormParticles): horizontal, wind driven. Everything is integer
// (1/256 m) and goes out as one GE line per drop (a triangle with a repeated
// vertex), all in a single BEGIN with one polygon ID: ~70 polys, ~0.3 ms.

static constexpr int MAX_DROPS = 72;
static constexpr int MAX_SPLASH = 24;
static constexpr s32 M = 256;                   // fixed point: 1/256 m

struct Drop { s32 x, y, z; };
struct Splash { s32 x, y, z; u8 age; };
static Drop s_drops[MAX_DROPS];
static Splash s_splash[MAX_SPLASH];
static int s_splashNext;
static bool s_dropsInit;
static u32 s_fxVbl;

static inline s32 rndi(s32 range) { return (s32)(rand() % (range > 0 ? range : 1)); }

// world point (1/256 m) relative to the camera -> v16 under R * scale(4)
static inline void dropVtx(s32 x, s32 y, s32 z, s32 cx, s32 cy, s32 cz) {
	glVertex3v16((v16)((x - cx) * 4), (v16)((z - cz) * 4), (v16)(-(y - cy) * 4));
}

struct Precip {
	int count;            // drops alive
	s32 vx, vy, vz;       // 1/256 m per vblank
	s32 half, height;     // wrap box half width and height above the ground
	int trail;            // streak length in vblanks of motion
	u16 head, tail;       // RGB15
	int alpha;
	bool splash;
};

static void precipitation(const View& v, const Precip& p, u32 dv) {
	const float fwdX = -sinf(v.yaw), fwdY = cosf(v.yaw);
	const s32 camX = (s32)(v.camX * M), camY = (s32)(v.camY * M), camZ = (s32)(v.camZ * M);
	// box centre: a little ahead of the camera, where the player looks
	const s32 bx = camX + (s32)(fwdX * (float)p.half * 0.6f), by = camY + (s32)(fwdY * (float)p.half * 0.6f);
	const s32 gz = (s32)(player::viewZ() * M);
	const s32 H = p.half, W = 2 * p.half;
	if (!s_dropsInit) {
		for (Drop& d : s_drops) d = { bx - H + rndi(W), by - H + rndi(W), gz + rndi(p.height) };
		s_dropsInit = true;
	}
	glPolyFmt(POLY_ALPHA(p.alpha) | POLY_CULL_NONE | POLY_ID(nextId()));
	glBegin(GL_TRIANGLES);
	for (int i = 0; i < p.count; i++) {
		Drop& d = s_drops[i];
		d.x += p.vx * (s32)dv;
		d.y += p.vy * (s32)dv;
		d.z += p.vz * (s32)dv;
		while (d.x - bx >= H) d.x -= W;
		while (d.x - bx < -H) d.x += W;
		while (d.y - by >= H) d.y -= W;
		while (d.y - by < -H) d.y += W;
		if (d.z < gz) {
			if (p.splash && (i & 1)) {
				s_splash[s_splashNext] = { d.x, d.y, gz, 0 };
				s_splashNext = (s_splashNext + 1) % MAX_SPLASH;
			}
			d.z += p.height;
			d.x += rndi(M) - M / 2;   // no visible repetition after a wrap
		} else if (d.z >= gz + p.height) {
			d.z -= p.height;
		}
		glColor(p.head);
		dropVtx(d.x, d.y, d.z, camX, camY, camZ);
		glColor(p.tail);
		const s32 tx = d.x - p.vx * p.trail, ty = d.y - p.vy * p.trail, tz = d.z - p.vz * p.trail;
		dropVtx(tx, ty, tz, camX, camY, camZ);
		dropVtx(tx, ty, tz, camX, camY, camZ);
	}
	if (p.splash) {
		// a tiny crown: two short lines opening up for 8 frames
		glColor(p.head);
		for (Splash& s : s_splash) {
			if (s.age >= 8) continue;
			const s32 r = 12 + s.age * 5, h = 20 + s.age * 3;
			if (s.x - bx >= H || s.x - bx < -H || s.y - by >= H || s.y - by < -H) { s.age = 8; continue; }
			dropVtx(s.x, s.y, s.z, camX, camY, camZ);
			dropVtx(s.x - r, s.y, s.z + h, camX, camY, camZ);
			dropVtx(s.x - r, s.y, s.z + h, camX, camY, camZ);
			dropVtx(s.x, s.y, s.z, camX, camY, camZ);
			dropVtx(s.x + r, s.y + r / 2, s.z + h, camX, camY, camZ);
			dropVtx(s.x + r, s.y + r / 2, s.z + h, camX, camY, camZ);
			s.age = (u8)(s.age + dv);
		}
	}
	glEnd();
}

void drawWeatherFx(const View& v) {
	const u32 vbl = timing::vblanks();
	u32 dv = vbl - s_fxVbl;
	s_fxVbl = vbl;
	if (dv > 4) dv = 4;
	if (!dbg::opts.weatherFx) return;
	const weather::State& w = weather::g;
	const u32 strength = w.currentRainParticleStrength;   // 0..~110, ramps one per frame
	const bool rain = strength > 0 && v.camZ <= 900.0f;
	const bool dust = !rain && w.wind > 1.01f;
	if (!rain && !dust) {
		s_dropsInit = false;
		return;
	}
	cameraRotation(v);
	glScalef(4.0f, 4.0f, 4.0f);
	glBindTexture(GL_TEXTURE_2D, 0);
	Precip p;
	if (rain) {
		// ~9 m/s fall, wind pushes up to ~4 m/s sideways
		const s32 n = (s32)(strength * MAX_DROPS / 77);
		p.count = n > MAX_DROPS ? MAX_DROPS : n;
		p.vx = (s32)(w.windDir[0] * 4.0f * M / 60.0f);
		p.vy = (s32)(w.windDir[1] * 4.0f * M / 60.0f);
		p.vz = -9 * M / 60;
		p.half = 7 * M;
		p.height = 7 * M;
		p.trail = 3;
		p.head = RGB15(25, 26, 29);
		p.tail = RGB15(12, 13, 16);
		p.alpha = 12 + (int)(strength > 110 ? 10 : strength * 10 / 110);
		p.splash = true;
	} else {
		// Sand2 particles: WindDir * 25, sand colour (0.67, 0.65, 0.55)
		p.count = 24 + (int)(w.sandstorm * 40.0f);
		if (p.count > MAX_DROPS) p.count = MAX_DROPS;
		p.vx = (s32)(w.windDir[0] * 25.0f * M / 60.0f);
		p.vy = (s32)(w.windDir[1] * 25.0f * M / 60.0f);
		p.vz = (s32)(w.windDir[2] * 25.0f * M / 60.0f);
		p.half = 10 * M;
		p.height = 6 * M;
		p.trail = 2;
		p.head = RGB15(21, 20, 17);
		p.tail = RGB15(15, 14, 12);
		p.alpha = 8 + (int)(w.sandstorm * 10.0f);
		p.splash = false;
	}
	precipitation(v, p, dv);
	glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

}
