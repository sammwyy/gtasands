#include "hud/debugui.h"

#include <math.h>
#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "assets/texture.h"
#include "audio/aebankloader.h"
#include "audio/aesoundmanager.h"
#include "audio/aestream.h"
#include "audio/audioengine.h"
#include "game/animstore.h"
#include "game/cj.h"
#include "game/collision.h"
#include "game/debugopts.h"
#include "game/hud/chud.h"
#include "game/hud/radar.h"
#include "game/pedmodel.h"
#include "game/peds.h"
#include "game/player.h"
#include "game/timecycle.h"
#include "game/vehicle.h"
#include "game/vehicledata.h"
#include "game/vehiclemodel.h"
#include "game/weather.h"
#include "game/world.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "render/renderer.h"

namespace dbgui {

enum Tab { TAB_LOG, TAB_PERF, TAB_MEM, TAB_GAME, TAB_REND, TAB_SKY, TAB_COUNT };
static const char* const k_tabs[TAB_COUNT] = { "LOG", "PERF", "MEM", "GAME", "REND", "SKY" };
static constexpr int TAB_W = 5;          // characters per tab (">" marker + 4-char label)
static constexpr int COLS = 31;          // last column left empty: writing it wraps (and row 23 would scroll)
static constexpr int ROWS = 24;

static int s_tab = TAB_PERF;
static bool s_repaint = true;
static unsigned s_logSeq = ~0u;
static int s_tapRow = -1, s_tapCol = -1;
static int s_heapCheck = -1;
static u16 s_cpuHist[COLS];
static int s_histPos;
static int s_weaponIdx = 1;

// ---- text helpers -------------------------------------------------------------

static int visibleLen(const char* s) {
	int n = 0;
	for (; *s; s++) {
		if (*s == '\x1b') { while (*s && *s != 'm' && *s != 'H' && *s != 'J') s++; if (!*s) break; continue; }
		n++;
	}
	return n;
}

// Prints `text` at (row, col) and blanks the rest of the row up to COLS.
static void put(int row, int col, const char* text) {
	printf("\x1b[%d;%dH%s", row, col, text);
	for (int n = col + visibleLen(text); n < COLS; n++) putchar(' ');
}

static void line(int row, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(int row, const char* fmt, ...) {
	char buf[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	put(row, 0, buf);
}

// "[label]" at (row, col); true when tapped this frame. Returns the column
// after the button through `next` so rows of buttons chain.
static bool button(int row, int& col, const char* label) {
	const int w = (int)strlen(label) + 2;
	printf("\x1b[%d;%dH\x1b[36m[%s]\x1b[39m", row, col, label);
	const bool hit = s_tapRow == row && s_tapCol >= col && s_tapCol < col + w;
	col += w + 1;
	return hit;
}

static void bar(char* out, int n, u32 v, u32 full) {
	const int k = full ? (int)((v * (u32)n + full / 2) / full) : 0;
	for (int i = 0; i < n; i++) out[i] = i < k ? (i < n * 3 / 4 ? '=' : '#') : '.';
	out[n] = 0;
}

static void blank(int from, int to) {
	for (int r = from; r <= to; r++) put(r, 0, "");
}

// ---- pages --------------------------------------------------------------------

static void pageLog() {
	line(1, "\x1b[33m%d lines\x1b[39m", hud::logCount());
	int c = 22;
	const bool clear = button(1, c, "clear");
	if (clear) hud::logClear();
	const unsigned seq = hud::logSeq();
	if (!s_repaint && seq == s_logSeq && !clear) return;
	s_logSeq = hud::logSeq();
	// newest at the bottom: walk back until the 21 rows (2..22) are full
	const int top = 2, bottom = 22, avail = bottom - top + 1;
	int used = 0, n = 0;
	while (n < hud::logCount()) {
		const int rows = visibleLen(hud::logLine(n)) / 32 + 1;
		if (used + rows > avail) break;
		used += rows;
		n++;
	}
	blank(top, bottom);
	int row = bottom - used + 1;
	for (int i = n - 1; i >= 0; i--) {
		const char* l = hud::logLine(i);
		printf("\x1b[%d;0H%s\x1b[39m", row, l);
		row += visibleLen(l) / 32 + 1;
	}
}

static void pagePerf() {
	const u32 frame = 16715;  // one 60 Hz frame, us
	const u32 cpu = timing::frameUs();
	s_cpuHist[s_histPos] = (u16)(cpu > 65535 ? 65535 : cpu);
	s_histPos = (s_histPos + 1) % COLS;
	char b[24];
	bar(b, 16, cpu, frame);
	line(1, "fps \x1b[33m%2lu\x1b[39m cpu %5lu us %3lu%%", timing::fps(), cpu, cpu * 100 / frame);
	line(2, "     [%s]", b);
	static const char* const k_sec[] = { "3d ", "sim", "hud" };
	for (int s = 0; s < timing::SEC_COUNT; s++) {
		const u32 us = timing::sectionUs((timing::Section)s);
		bar(b, 16, us, frame);
		line(3 + s, "%s %5lu [%s]", k_sec[s], us, b);
	}
	// cpu history, one column per frame (height = share of a 60 Hz frame)
	static const char k_lv[] = " .:-=+*#%";
	char h[COLS + 1];
	u32 mn = 0xFFFF, mx = 0;
	for (int i = 0; i < COLS; i++) {
		const u32 v = s_cpuHist[(s_histPos + i) % COLS];
		if (v < mn) mn = v;
		if (v > mx) mx = v;
		const u32 k = v * 8 / frame;
		h[i] = k_lv[k > 8 ? 8 : k];
	}
	h[COLS] = 0;
	line(6, "hist min %5lu max %5lu us", mn, mx);
	line(7, "%s", h);
	line(8, "polys %4d/2048 verts %4d/6144", renderer::polyCount(), renderer::vertCount());
	line(9, "cj verts %d  clip %.14s", cj::vertCount(), cj::animName());
	const player::State& p = player::g;
	line(10, "pos %7.1f %7.1f %5.1f", p.x, p.y, p.z);
	line(11, "hdg %5.2f spd %5.1f aim %d", p.heading, p.speed, (int)p.aiming);
	char loco[40];
	player::locoDebug(loco, sizeof loco);
	line(12, "%s", loco);
	line(14, "radar rng %3d tiles %2d", (int)radar::range(), radar::tilesCached());
	line(13, "sim x%d.%02d %s  vbl %lu", dbg::opts.simSpeedQ / 4, dbg::opts.simSpeedQ % 4 * 25,
	     dbg::opts.fixedStep ? "fixed" : "frame", timing::vblanks());
	const world::Stats& w = world::stats();
	line(15, "map %3u/%3u cull f%3u b%3u", w.drawn, w.candidates, w.culledFrustum, w.culledBudget);
	line(16, "polys %4u>%4u us u%4u d%4u", w.polysBefore, w.polysAfter, w.updateUs, w.drawUs);
	line(17, "mdl %3u+%2u %3luK sec%2u i%4u", w.modelsReady, w.modelsLoading,
	     (unsigned long)(w.listBytes >> 10), w.sectors, w.instances);
	line(18, "tex %3u fail %2u vram %3luK", w.textures, w.texFailed, (unsigned long)(w.texVram >> 10));
	const col::Stats& cs = col::stats();
	line(19, "col %3u %3luK near %2u miss %2u", cs.models, (unsigned long)(cs.bytes >> 10), cs.nearCount, cs.missing);
	line(20, "col us u%4u q%4u loads %lu", cs.updateUs, cs.queryUs, (unsigned long)cs.loads);
	const aesound::Stats& as = aesound::stats();
	line(21, "snd %2u/%2u ch steal %lu us %4lu", as.active, as.physical, (unsigned long)as.stolen,
	     (unsigned long)audio::stats().serviceUs);
	line(22, "steps %lu land %lu shots %lu", (unsigned long)audio::stats().footsteps,
	     (unsigned long)audio::stats().landings, (unsigned long)audio::stats().shots);
	blank(23, 23);
}

static void regionRow(int row, const char* name, mem::Region r) {
	const mem::RegionStats s = mem::stats(r);
	line(row, "%-5s%5u%5u%5u%5u%4lu", name, (unsigned)(s.used >> 10), (unsigned)(s.capacity >> 10),
	     (unsigned)(s.peak >> 10), (unsigned)(s.largestFree >> 10), (unsigned long)s.failures);
}

static void pageMem() {
	line(1, "\x1b[33mKB   used  cap peak   lf  err\x1b[39m");
	regionRow(2, "STRM", mem::REGION_STREAM);
	regionRow(3, "PERM", mem::REGION_PERM);
	regionRow(4, "SYS", mem::REGION_SYS);
	regionRow(5, "SCR", mem::REGION_SCRATCH);
	line(6, "blocks %lu  total %uK", (unsigned long)mem::stats(mem::REGION_STREAM).blocks,
	     (unsigned)(mem::totalBytes() >> 10));
	for (int t = 0, row = 7; t < mem::TAG_COUNT; t += 2, row++) {
		char a[16], b[20] = "";
		snprintf(a, sizeof a, "%-6s%5uK", mem::tagName((mem::Tag)t), (unsigned)(mem::tagBytes((mem::Tag)t) >> 10));
		if (t + 1 < mem::TAG_COUNT)
			snprintf(b, sizeof b, "%-6s%5uK", mem::tagName((mem::Tag)(t + 1)),
			         (unsigned)(mem::tagBytes((mem::Tag)(t + 1)) >> 10));
		else   // odd tag count: VRAM shares the last row
			snprintf(b, sizeof b, "vram%4uK/%3d", (unsigned)(tex::vramBytes() >> 10), tex::loadedCount());
		line(row, "%s   %s", a, b);
	}
	line(14, "anim %2d clips %3uK (budget %uK)", animstore::residentClips(),
	     (unsigned)(animstore::residentBytes() >> 10), (unsigned)(mem::budget::ANIM_CACHE_BYTES >> 10));
	line(15, "heap check: %s", s_heapCheck < 0 ? "-" : s_heapCheck ? "\x1b[32mOK\x1b[39m" : "\x1b[31mCORRUPT\x1b[39m");
	put(16, 0, "");
	int c = 0;
	if (button(16, c, "check heap")) s_heapCheck = mem::check() ? 1 : 0;
	if (button(16, c, "trim anims")) animstore::trim(0);
	const aebank::Stats& ab = aebank::stats();
	line(17, "audio %3u snd %3luK/%uK q%u f%u", ab.resident, (unsigned long)(ab.bytes >> 10),
	     (unsigned)(mem::budget::AUDIO_CACHE_BYTES >> 10), ab.queued, ab.failed);
	line(18, "audio loads %lu evict %lu trk %ld", (unsigned long)ab.loads, (unsigned long)ab.evictions,
	     aestream::stats().trackId == ~0u ? -1L : (long)aestream::stats().trackId);
	blank(19, 23);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// GAME tab, rows 15..23: vehicles (spawn from vehicles.dveh by type or model id, enter, damage)
enum VCat { VC_CAR, VC_BIKE, VC_BMX, VC_QUAD, VC_BOAT, VC_HELI, VC_PLANE, VC_TRAIN, VC_TRAILER, VC_RC, VC_COUNT };
static const char* const k_vcat[VC_COUNT] = { "car", "bike", "bmx", "quad", "boat", "heli", "plane", "train", "trailer", "rc" };
static int s_vehPick = -1;
static int s_lastCar = -1;
static int s_vcat = VC_CAR;
static int s_spawnId = 400;

static int catOf(int i) {
	const int id = vehdata::modelId(i);
	if (veh::isRemote(id)) return VC_RC;
	switch (vehdata::type(i)) {
	case ds::VEH_BIKE: return VC_BIKE;
	case ds::VEH_BMX: return VC_BMX;
	case ds::VEH_QUAD: return VC_QUAD;
	case ds::VEH_BOAT: return VC_BOAT;
	case ds::VEH_HELI: case ds::VEH_FHELI: return VC_HELI;
	case ds::VEH_PLANE: case ds::VEH_FPLANE: return VC_PLANE;
	case ds::VEH_TRAIN: return VC_TRAIN;
	case ds::VEH_TRAILER: return VC_TRAILER;
	default: return VC_CAR;
	}
}

static int nextOfCat(int from, int dir, int cat) {
	const int n = vehdata::count();
	for (int k = 1; k <= n; k++) {
		const int i = ((from + dir * k) % n + n) % n;
		if (vehdata::spawnable(i) && catOf(i) == cat) return i;
	}
	return -1;
}

// in front of CJ (boats: the nearest water, trains: the nearest track; CJ is
// moved next to them), side on
static void spawnHere(int index) {
	if (index < 0) return;
	player::State& p = player::g;
	const int cat = catOf(index);
	const float d = cat == VC_PLANE ? 14.0f : cat == VC_HELI || cat == VC_TRAIN ? 10.0f : cat == VC_BOAT ? 8.0f : 6.0f;
	float x = p.x - sinf(p.heading) * d, y = p.y + cosf(p.heading) * d, z = p.z + 1.0f;
	float sx, sy, sz;
	if (veh::spawnPoint(index, x, y, sx, sy, sz)) {
		if ((sx - x) * (sx - x) + (sy - y) * (sy - y) > 25.0f) {
			p.x = sx + 6.0f; p.y = sy; p.z = sz + 2.0f;   // next to it; the collision streams in first
			player::placeOnGround();
		}
		x = sx; y = sy; z = sz + 0.5f;
	}
	const int id = veh::spawn(index, x, y, z, p.heading + 1.5708f);
	if (id >= 0) s_lastCar = id;
}

static void pageVehicles() {
	if (!vehdata::ready()) {
		line(15, "\x1b[33mno vehicles.dveh\x1b[39m");
		blank(16, 23);
		return;
	}
	if (s_vehPick < 0) s_vehPick = vehdata::findModel(492) >= 0 ? vehdata::findModel(492) : nextOfCat(0, 1, VC_CAR);
	ds::VehInfo info;
	const bool have = vehdata::read(s_vehPick, info);
	line(15, "\x1b[33m%-5s\x1b[39m %-9.9s %d", k_vcat[s_vcat], have ? info.model : "?", have ? info.modelId : 0);
	int c = 21;
	if (button(15, c, "<")) s_vehPick = nextOfCat(s_vehPick, -1, s_vcat);
	if (button(15, c, ">")) s_vehPick = nextOfCat(s_vehPick, 1, s_vcat);
	put(16, 0, "");
	c = 0;
	const bool prev = button(16, c, "type<");
	const bool next = button(16, c, "type>");
	if (prev || next) {
		s_vcat = (s_vcat + (next ? 1 : VC_COUNT - 1)) % VC_COUNT;
		const int i = nextOfCat(-1, 1, s_vcat);
		if (i >= 0) s_vehPick = i;
	}
	if (button(16, c, "spawn")) spawnHere(s_vehPick);
	if (button(16, c, "rnd")) {
		for (int k = rand() % 8 + 1; k > 0; k--) s_vehPick = nextOfCat(s_vehPick, 1, s_vcat);
		spawnHere(s_vehPick);
	}
	line(17, "id %3d", s_spawnId);
	c = 7;
	if (button(17, c, "-10")) s_spawnId -= 10;
	if (button(17, c, "-")) s_spawnId -= 1;
	if (button(17, c, "+")) s_spawnId += 1;
	if (button(17, c, "+10")) s_spawnId += 10;
	if (s_spawnId < 400) s_spawnId = 611;
	if (s_spawnId > 611) s_spawnId = 400;
	if (button(17, c, "go")) {
		const int i = vehdata::findModel(s_spawnId);
		if (i >= 0) { s_vehPick = i; s_vcat = catOf(i); spawnHere(i); }
		else hud::log("\x1b[33mno vehicle %d\x1b[39m", s_spawnId);
	}
	int cur = player::vehicleId() >= 0 ? player::vehicleId() : s_lastCar;
	if (!veh::get(cur)) {
		cur = -1;
		for (int i = 0; i < veh::MAX && cur < 0; i++)
			if (veh::get(i)) cur = i;
	}
	put(18, 0, "");
	c = 0;
	if (button(18, c, "enter") && cur >= 0) player::enterVehicleNow(cur);
	if (button(18, c, "del all")) { veh::removeAll(); s_lastCar = -1; }
	if (button(18, c, "repair")) veh::repair(cur);
	if (button(18, c, "boom")) veh::blowUp(cur);
	const veh::Vehicle* v = veh::get(cur);
	if (v) {
		static const char* const k_st[] = { "-", "phys", "PLAYER", "WRECK" };
		line(19, "#%d %-6s hp%4d %3dkm/h g%d %s", cur, k_st[v->status & 3], (int)v->health,
		     (int)(fabsf(v->speedFwd) * 50.0f * 3.6f), v->gear, v->inWater ? "wet" : "");
		line(20, "susp %.2f %.2f %.2f %.2f %s", v->susp[0], v->susp[1], v->susp[2], v->susp[3], v->asleep ? "zz" : "");
		char d[vehmdl::COMP_COUNT + 1];
		for (int k = 0; k < vehmdl::COMP_COUNT; k++) d[k] = "o~x"[v->dmg[k] < 3 ? v->dmg[k] : 0];
		d[vehmdl::COMP_COUNT] = 0;
		line(21, "dmg %s t%x l%+.2f r%.2f", d + vehmdl::COMP_DOOR_LF, v->burst, v->lean, v->rotor);
	} else {
		line(19, "(no vehicle)");
		blank(20, 21);
	}
	put(22, 0, "");
	c = 0;
	if (button(22, c, "flip")) veh::flip(cur);
	if (button(22, c, "hp250") && v) veh::setHealth(cur, 249.0f);
	if (button(22, c, "tyre")) veh::burstTyre(cur, -1);
	if (button(22, c, dbg::opts.skidmarks ? "skid:on" : "skid:off")) dbg::opts.skidmarks = !dbg::opts.skidmarks;
	const vehmdl::Stats& ms = vehmdl::stats();
	const veh::Stats& vs = veh::stats();
	line(23, "m%u/%u t%u %luK n%u st%lu dr%lu sk%u", ms.ready, ms.models, ms.textures, (unsigned long)(ms.texVram >> 10),
	     vs.active, (unsigned long)vs.stepUs, (unsigned long)vs.drawUs, vs.skids);
}

static void pageGame(bool inGame) {
	if (!inGame) {
		line(1, "\x1b[33m(start a game first)\x1b[39m");
		blank(2, 23);
		return;
	}
	player::State& p = player::g;
	int c;
	line(1, "wanted %d", p.wantedLevel);
	c = 14;
	if (button(1, c, "-")) player::setWantedLevel(p.wantedLevel - 1);
	if (button(1, c, "+")) player::setWantedLevel(p.wantedLevel + 1);
	if (button(1, c, "0")) player::setWantedLevel(0);

	line(2, "health %3d", (int)p.health);
	c = 14;
	if (button(2, c, "-")) p.health = clampf(p.health - 25.0f, 0.0f, p.maxHealth);
	if (button(2, c, "+")) p.health = clampf(p.health + 25.0f, 0.0f, p.maxHealth);
	if (button(2, c, "max")) p.health = p.maxHealth;

	line(3, "armour %3d", (int)p.armour);
	c = 14;
	if (button(3, c, "-")) p.armour = clampf(p.armour - 25.0f, 0.0f, p.maxArmour);
	if (button(3, c, "+")) p.armour = clampf(p.armour + 25.0f, 0.0f, p.maxArmour);
	if (button(3, c, "max")) p.armour = p.maxArmour;

	line(4, "money $%d", p.money);
	c = 0;
	put(5, 0, "");
	if (button(5, c, "-1k")) p.money -= 1000;
	if (button(5, c, "+1k")) p.money += 1000;
	if (button(5, c, "+100k")) p.money += 100000;
	if (button(5, c, "0")) p.money = 0;

	const int wt = player::weaponTypeAt(s_weaponIdx);
	line(6, "weapon in hand: %d %.12s", p.weaponType, p.weapon.txd ? p.weapon.txd : "fists");
	line(7, "give: id %d", wt);
	c = 12;
	if (button(7, c, "<") && --s_weaponIdx < 1) {
		s_weaponIdx = 1;
		while (player::weaponTypeAt(s_weaponIdx + 1) >= 0) s_weaponIdx++;
	}
	if (button(7, c, ">") && player::weaponTypeAt(++s_weaponIdx) < 0) s_weaponIdx = 1;
	if (button(7, c, "give")) player::giveWeapon(wt, 200);
	put(8, 0, "");
	c = 0;
	if (button(8, c, "all weapons")) player::giveAllWeapons();
	if (button(8, c, "remove all")) player::clearWeapons();

	line(9, "fat    %4d", (int)p.fat);
	c = 12;
	if (button(9, c, "0")) player::setBodyStats(0.0f, p.muscle);
	if (button(9, c, "-")) player::setBodyStats(p.fat - 250.0f, p.muscle);
	if (button(9, c, "+")) player::setBodyStats(p.fat + 250.0f, p.muscle);
	if (button(9, c, "max")) player::setBodyStats(1000.0f, p.muscle);

	line(10, "muscle %4d", (int)p.muscle);
	c = 12;
	if (button(10, c, "0")) player::setBodyStats(p.fat, 0.0f);
	if (button(10, c, "-")) player::setBodyStats(p.fat, p.muscle - 250.0f);
	if (button(10, c, "+")) player::setBodyStats(p.fat, p.muscle + 250.0f);
	if (button(10, c, "max")) player::setBodyStats(p.fat, 1000.0f);

	line(11, "sprint %3d%%", (int)p.sprint);
	c = 12;
	if (button(11, c, "refill")) player::refillSprint();

	line(12, "clock %02u:%02u", player::g_clockH, player::g_clockM);
	c = 12;
	if (button(12, c, "+1h")) player::g_clockH = (player::g_clockH + 1) % 24;
	if (button(12, c, "+6h")) player::g_clockH = (player::g_clockH + 6) % 24;

	line(13, "outfit %.10s", cj::outfitName(cj::outfit()));
	c = 18;
	const int no = cj::outfitCount();
	if (button(13, c, "<")) cj::setOutfit((cj::outfit() + no - 1) % no);
	if (button(13, c, ">")) cj::setOutfit((cj::outfit() + 1) % no);

	put(14, 0, "");
	c = 0;
	if (button(14, c, "respawn")) player::respawn();
	if (button(14, c, "full heal")) { p.health = p.maxHealth; p.armour = p.maxArmour; player::refillSprint(); }
	pageVehicles();
}

static void toggle2(int row, const char* a, bool& va, const char* b, bool& vb);

static void toggleRow(int row, const char* name, bool& v) {
	line(row, "%s", name);
	int c = 18;
	if (button(row, c, v ? "ON " : "OFF")) v = !v;
}

// REND tab rows 15..23: the ped population (game/peds)
static void pagePeds() {
	dbg::Options& o = dbg::opts;
	const peds::Stats& ps = peds::stats();
	line(15, "\x1b[33mpeds\x1b[39m %2u/%-2u c%u g%u p%u d%u", ps.live, ps.maxPeds, ps.civ, ps.gang, ps.cops, ps.dealers);
	int c = 24;
	if (button(15, c, o.peds ? "ON " : "OFF")) o.peds = !o.peds;
	line(16, "density %3d%%  polys %3d", o.pedDensity, o.pedPolys);
	put(17, 0, "");
	c = 0;
	if (button(17, c, "d-") && o.pedDensity > 0) o.pedDensity -= 25;
	if (button(17, c, "d+") && o.pedDensity < 200) o.pedDensity += 25;
	if (button(17, c, "p-") && o.pedPolys > 100) o.pedPolys -= 100;
	if (button(17, c, "p+") && o.pedPolys < 1200) o.pedPolys += 100;
	put(18, 0, "");
	c = 0;
	if (button(18, c, "spawn")) peds::spawnNear(player::g.x, player::g.y, player::g.z);
	if (button(18, c, "kill all")) peds::removeAll();
	const pedmodel::Stats& ms = pedmodel::stats();
	line(19, "zone t%u %s slot%u mdl %u/%u %luK", ps.zoneType, ps.weekend ? "wkend" : "wkday", ps.slot, ms.ready,
	     ms.models, (unsigned long)(ms.listBytes >> 10));
	line(20, "drawn %u posed %u polys %u", ps.drawn, ps.posed, ps.polys);
	line(21, "upd %luus draw %luus +%u -%u", (unsigned long)ps.updateUs, (unsigned long)ps.drawUs, ps.spawned, ps.removed);
	// nearest ped
	int best = -1;
	float bestD = 1e9f;
	peds::Info in;
	for (int i = 0; i < peds::MAX; i++)
		if (peds::info(i, in)) {
			const float dx = in.x - player::g.x, dy = in.y - player::g.y;
			if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = i; }
		}
	if (best >= 0 && peds::info(best, in))
		line(22, "#%d id%d %-7s hp%3d %.0fm t%d", best, in.modelId, peds::taskName(in.task), (int)in.health, sqrtf(bestD), in.target);
	else
		line(22, "(no peds)");
	blank(23, 23);
}

static void pageRend() {
	dbg::Options& o = dbg::opts;
	toggleRow(1, "CJ mesh", o.drawCj);
	toggleRow(2, "skeleton", o.skeleton);
	toggleRow(3, "ground", o.ground);
	toggleRow(4, "fixed 60Hz step", o.fixedStep);
	line(5, "sim speed x%d.%02d", o.simSpeedQ / 4, o.simSpeedQ % 4 * 25);
	int c = 18;
	if (button(5, c, "-") && o.simSpeedQ > 0) o.simSpeedQ--;
	if (button(5, c, "+") && o.simSpeedQ < 8) o.simSpeedQ++;
	put(6, 0, "");
	c = 0;
	if (button(6, c, o.simSpeedQ ? "pause" : "resume")) o.simSpeedQ = o.simSpeedQ ? 0 : 4;
	if (button(6, c, "x1")) o.simSpeedQ = 4;
	line(7, "camera dist %.1f", o.camDist);
	c = 18;
	if (button(7, c, "-")) o.camDist = clampf(o.camDist - 0.4f, 1.2f, 12.0f);
	if (button(7, c, "+")) o.camDist = clampf(o.camDist + 0.4f, 1.2f, 12.0f);
	put(8, 0, "");
	c = 0;
	if (button(8, c, "repaint HUD")) chud::invalidate();
	if (button(8, c, "defaults")) o = dbg::Options();
	toggleRow(9, "map (world)", o.world);
	line(10, "map poly budget %4d", o.worldPolys);
	c = 22;
	if (button(10, c, "-") && o.worldPolys > 400) o.worldPolys -= 100;
	if (button(10, c, "+") && o.worldPolys < 2048) o.worldPolys += 100;
	toggle2(11, "colls", o.collision, "outline", o.colOutline);
	line(12, "col range %2dm  polys %4d", o.colRange, o.colPolys);
	c = 0;
	put(13, 0, "");
	if (button(13, c, "r-") && o.colRange > 2) o.colRange -= 2;
	if (button(13, c, "r+") && o.colRange < 40) o.colRange += 2;
	if (button(13, c, "p-") && o.colPolys > 100) o.colPolys -= 100;
	if (button(13, c, "p+") && o.colPolys < 1800) o.colPolys += 100;
	toggleRow(14, "physics (col)", o.physics);
	pagePeds();
}

// Two toggles on one row: name at col 0 / 13, buttons at col 7 / 24.
static void toggle2(int row, const char* a, bool& va, const char* b, bool& vb) {
	char buf[40];
	snprintf(buf, sizeof buf, "%-6.6s        %-10.10s", a, b);
	put(row, 0, buf);
	int c = 7;
	if (button(row, c, va ? "ON " : "OFF")) va = !va;
	c = 24;
	if (button(row, c, vb ? "ON " : "OFF")) vb = !vb;
}

static int s_pickWeather = weather::EXTRASUNNY_LA;

static void pageSky(bool inGame) {
	if (!inGame) {
		line(1, "\x1b[33m(start a game first)\x1b[39m");
		blank(2, 23);
		return;
	}
	dbg::Options& o = dbg::opts;
	weather::State& w = weather::g;
	const timecycle::Colours& c = timecycle::current();
	int col;

	line(1, "time %02u:%02u:%02u  day %u/%u", player::g_clockH, player::g_clockM, player::clockSeconds(),
	     player::g_clockDay, player::g_clockMonth);
	put(2, 0, "");
	col = 0;
	if (button(2, col, "-1h")) player::setClock(player::g_clockH + 23, player::g_clockM);
	if (button(2, col, "+1h")) player::setClock(player::g_clockH + 1, player::g_clockM);
	if (button(2, col, "+6h")) player::setClock(player::g_clockH + 6, player::g_clockM);
	if (button(2, col, "0h")) player::setClock(0, 0);
	if (button(2, col, "12h")) player::setClock(12, 0);
	line(3, "rate x%d", o.clockRate);
	col = 9;
	if (button(3, col, "||")) o.clockRate = 0;
	if (button(3, col, "x1")) o.clockRate = 1;
	if (button(3, col, "x10")) o.clockRate = 10;
	if (button(3, col, "x60")) o.clockRate = 60;

	line(4, "w %s>%s %2d%%", weather::name(w.oldType), weather::name(w.newType), (int)(w.interpolation * 100.0f));
	line(5, "rgn %s  frc %s", weather::regionName(w.region), weather::name(w.forcedType));
	line(6, "pick %s", weather::name((weather::Type)s_pickWeather));
	col = 17;
	if (button(6, col, "<")) s_pickWeather = (s_pickWeather + weather::NUM_PLAYABLE - 1) % weather::NUM_PLAYABLE;
	if (button(6, col, ">")) s_pickWeather = (s_pickWeather + 1) % weather::NUM_PLAYABLE;
	put(7, 0, "");
	col = 0;
	if (button(7, col, "next hour")) weather::forceWeather((weather::Type)s_pickWeather);
	if (button(7, col, "now")) weather::forceWeatherNow((weather::Type)s_pickWeather);
	if (button(7, col, "auto")) weather::releaseWeather();
	put(8, 0, "");
	col = 0;
	if (button(8, col, "sun")) weather::forceWeatherNow(weather::EXTRASUNNY_LA);
	if (button(8, col, "cld")) weather::forceWeatherNow(weather::CLOUDY_LA);
	if (button(8, col, "rain")) weather::forceWeatherNow(weather::RAINY_SF);
	if (button(8, col, "fog")) weather::forceWeatherNow(weather::FOGGY_SF);
	if (button(8, col, "sand")) weather::forceWeatherNow(weather::SANDSTORM_DESERT);

	line(9, "rain %.2f sand %.2f fog %.2f", w.rain, w.sandstorm, w.foggyness);
	line(10, "cloud %.2f xsun %.2f wind %.2f", w.cloudCoverage, w.extraSunnyness, w.wind);
	line(11, "glare %.2f rbow %.2f wet %.2f", w.sunGlare, w.rainbow, w.wetRoads);
	const float* s = timecycle::vectorToSun();
	line(12, "sun %+.2f %+.2f %+.2f dn %.2f", s[0], s[1], s[2], timecycle::dayNightBalance());
	line(13, "skyT %3d %3d %3d B %3d %3d %3d", c.skyTopR, c.skyTopG, c.skyTopB, c.skyBotR, c.skyBotG, c.skyBotB);
	u8 pr, pg, pb;
	timecycle::pedLight(pr, pg, pb);
	line(14, "far %d fog %d view %d", c.farClip, c.fogStart, o.fogView);
	col = 23;
	if (button(14, col, "-") && o.fogView > 40) o.fogView -= 20;
	if (button(14, col, "+") && o.fogView < 200) o.fogView += 20;
	line(15, "ped light %3d %3d %3d", pr, pg, pb);
	toggle2(16, "sky", o.sky, "sprites", o.skySprites);
	toggle2(17, "fog", o.fog, "weather fx", o.weatherFx);
	line(18, "ped lit");
	col = 7;
	if (button(18, col, o.pedLight ? "ON " : "OFF")) o.pedLight = !o.pedLight;
	col = 13;
	if (button(18, col, "lightning")) weather::debugFlash();
	blank(19, 23);
}

// ---- frame --------------------------------------------------------------------

void enter() {
	s_repaint = true;
	consoleClear();
}

void frame(const input::State& in, bool inGame) {
	s_tapRow = s_tapCol = -1;
	if (in.tapped) {
		s_tapRow = in.touchY / 8;
		s_tapCol = in.touchX / 8;
		if (s_tapRow == 0 && s_tapCol / TAB_W < TAB_COUNT && s_tapCol / TAB_W != s_tab) {
			s_tab = s_tapCol / TAB_W;
			s_repaint = true;
			s_tapRow = -1;   // the tap only switched tabs
		}
	}
	if (s_repaint) consoleClear();
	// tab bar
	printf("\x1b[0;0H");
	// tab bar: the console has no background colours, so the active tab is
	// marked with '>' and yellow
	for (int t = 0; t < TAB_COUNT; t++) {
		char lbl[8];
		snprintf(lbl, sizeof lbl, t == s_tab ? ">%s" : " %s", k_tabs[t]);
		printf(t == s_tab ? "\x1b[33m%-*s\x1b[39m" : "\x1b[37m%-*s\x1b[39m", TAB_W, lbl);
	}
	switch (s_tab) {
	case TAB_LOG: pageLog(); break;
	case TAB_PERF: pagePerf(); break;
	case TAB_MEM: pageMem(); break;
	case TAB_GAME: pageGame(inGame); break;
	case TAB_SKY: pageSky(inGame); break;
	default: pageRend(); break;
	}
	s_repaint = false;
	(void)ROWS;
}

}
