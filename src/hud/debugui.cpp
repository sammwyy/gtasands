#include "hud/debugui.h"

#include <math.h>
#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "game/entryexits.h"
#include "game/hud/chud.h"
#include "game/hud/radar.h"
#include "game/menu/menu.h"
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

enum Tab { TAB_LOG, TAB_PERF, TAB_MEM, TAB_GAME, TAB_REND, TAB_SKY, TAB_CHTS, TAB_COUNT };
static const char* const k_tabs[TAB_COUNT] = { "LOG", "PRF", "MEM", "GME", "RND", "SKY", "CHT" };   // 3 letters + a gap
static constexpr int TAB_W = 4;
static constexpr int COLS = 31;          // last column left empty: writing it wraps (and row 23 would scroll)
static constexpr int ROWS = 24;
static constexpr int REFRESH = 3;        // pages are rebuilt every 3rd frame (or on a tap / tab change)
static constexpr uint8_t DEFAULT_COL = 9;

static int s_tab = TAB_PERF;
static bool s_repaint = true;
static int s_tapRow = -1, s_tapCol = -1;
static int s_heapCheck = -1;
static u16 s_cpuHist[COLS];
static int s_histPos;
static int s_weaponIdx = 1;
static int s_cheatCat, s_cheatPage[6], s_cheatWeather = weather::EXTRASUNNY_LA;
static uint32_t s_frameNo;

// ---- text buffer ----------------------------------------------------------------
// The page is composed in RAM and only the cells that differ from what the
// console shows are written out. Printing straight to the console blanked a
// row and then reprinted it (and the buttons over it) every frame, which
// flickered, and a page that skipped a row left the previous tab's text there.
static char s_ch[ROWS][COLS];
static uint8_t s_col[ROWS][COLS];
static char s_shownCh[ROWS][COLS];
static uint8_t s_shownCol[ROWS][COLS];

static void clearBuffer() {
	memset(s_ch, ' ', sizeof s_ch);
	memset(s_col, DEFAULT_COL, sizeof s_col);
}

static void invalidateShown() { memset(s_shownCh, 0, sizeof s_shownCh); }

// Writes `text` (with \x1b[3Nm / \x1b[39m colour codes) at (row, col).
// wrap: continue on the next rows (log lines), else clip at the row end.
// Returns the number of rows used.
static int write(int row, int col, const char* text, bool wrap = false) {
	uint8_t colour = DEFAULT_COL;
	const int startRow = row;
	for (const char* s = text; *s; s++) {
		if (*s == '\x1b') {
			if (s[1] == '[') {
				int v = 0;
				s += 2;
				while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
				if (*s == 'm') colour = v >= 30 && v <= 37 ? (uint8_t)(v - 30) : DEFAULT_COL;
				if (!*s) break;
			}
			continue;
		}
		if (col >= COLS) {
			if (!wrap) break;
			col = 0;
			row++;
		}
		if ((unsigned)row >= (unsigned)ROWS) break;
		s_ch[row][col] = *s == '\n' ? ' ' : *s;
		s_col[row][col] = colour;
		col++;
	}
	return row - startRow + 1;
}

static void flush() {
	for (int r = 0; r < ROWS; r++) {
		int c = 0;
		while (c < COLS) {
			if (s_ch[r][c] == s_shownCh[r][c] && s_col[r][c] == s_shownCol[r][c]) { c++; continue; }
			int e = c;
			while (e < COLS && (s_ch[r][e] != s_shownCh[r][e] || s_col[r][e] != s_shownCol[r][e])) e++;
			printf("\x1b[%d;%dH", r, c);
			int cur = -1;
			for (int k = c; k < e; k++) {
				if (s_col[r][k] != cur) {
					cur = s_col[r][k];
					printf("\x1b[%dm", cur == DEFAULT_COL ? 39 : 30 + cur);
				}
				putchar(s_ch[r][k]);
				s_shownCh[r][k] = s_ch[r][k];
				s_shownCol[r][k] = s_col[r][k];
			}
			printf("\x1b[39m");
			c = e;
		}
	}
}

// ---- text helpers -------------------------------------------------------------

static void line(int row, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(int row, const char* fmt, ...) {
	char buf[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	write(row, 0, buf);
}

// "[label]" at (row, col); true when tapped this frame. Advances `col` so
// rows of buttons chain.
static bool button(int row, int& col, const char* label, bool selected = false) {
	const int w = (int)strlen(label) + 2;
	char buf[40];
	snprintf(buf, sizeof buf, "%s[%s]\x1b[39m", selected ? "\x1b[33m" : "\x1b[36m", label);
	write(row, col, buf);
	const bool hit = s_tapRow == row && s_tapCol >= col && s_tapCol < col + w;
	col += w + 1;
	return hit;
}

static bool toggle(int row, int col, bool& v) {
	if (!button(row, col, v ? "ON " : "OFF")) return false;
	v = !v;
	return true;
}

static void bar(char* out, int n, u32 v, u32 full) {
	const int k = full ? (int)((v * (u32)n + full / 2) / full) : 0;
	for (int i = 0; i < n; i++) out[i] = i < k ? (i < n * 3 / 4 ? '=' : '#') : '.';
	out[n] = 0;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- vehicles (shared by CHTS > VEH and the GAME status) ----------------------

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

static void spawnHere(int index) {
	if (index < 0) return;
	const int id = veh::spawnInFront(index);
	if (id >= 0) s_lastCar = id;
}

// the vehicle the actions apply to: the one CJ is in, the last spawned, any
static int currentCar() {
	int cur = player::vehicleId() >= 0 ? player::vehicleId() : s_lastCar;
	if (!veh::get(cur)) {
		cur = -1;
		for (int i = 0; i < veh::MAX && cur < 0; i++)
			if (veh::get(i)) cur = i;
	}
	return cur;
}

// ---- pages --------------------------------------------------------------------

static void pageLog() {
	line(1, "\x1b[33m%d lines\x1b[39m", hud::logCount());
	int c = 22;
	if (button(1, c, "clear")) hud::logClear();
	// newest at the bottom: walk back until the 21 rows (2..22) are full
	const int top = 2, bottom = 22, avail = bottom - top + 1;
	int used = 0, n = 0;
	auto rowsOf = [](const char* l) {
		int len = 0;
		for (const char* s = l; *s; s++) {
			if (*s == '\x1b') { while (*s && *s != 'm') s++; if (!*s) break; continue; }
			len++;
		}
		return len / COLS + 1;
	};
	while (n < hud::logCount()) {
		const int rows = rowsOf(hud::logLine(n));
		if (used + rows > avail) break;
		used += rows;
		n++;
	}
	int row = bottom - used + 1;
	for (int i = n - 1; i >= 0; i--) row += write(row, 0, hud::logLine(i), true);
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
	for (int s = 0; s <= timing::SEC_HUD; s++) {
		const u32 us = timing::sectionUs((timing::Section)s);
		bar(b, 16, us, frame);
		line(3 + s, "%s %5lu [%s]", k_sec[s], us, b);
	}
	// cpu history, one column per refresh (height = share of a 60 Hz frame)
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
	line(13, "sim x%d.%02d %s  vbl %lu", dbg::opts.simSpeedQ / 4, dbg::opts.simSpeedQ % 4 * 25,
	     dbg::opts.fixedStep ? "fixed" : "frame", timing::vblanks());
	line(14, "radar rng %3d tiles %2d", (int)radar::range(), radar::tilesCached());
	const world::Stats& w = world::stats();
	line(15, "map %3u/%3u cull f%3u b%3u", w.drawn, w.candidates, w.culledFrustum, w.culledBudget);
	line(16, "polys %4u>%4u us u%4lu d%4lu", w.polysBefore, w.polysAfter,
	     (unsigned long)w.updateUs, (unsigned long)w.drawUs);
	line(17, "mdl %3u+%2u %3luK sec%2u i%4u", w.modelsReady, w.modelsLoading,
	     (unsigned long)(w.listBytes >> 10), w.sectors, w.instances);
	line(18, "tex %3u fail %2u vram %3luK", w.textures, w.texFailed, (unsigned long)(w.texVram >> 10));
	const col::Stats& cs = col::stats();
	line(19, "col %3u %3luK near %2u miss %2u", cs.models, (unsigned long)(cs.bytes >> 10), cs.nearCount, cs.missing);
	line(20, "col us u%4u q%4u loads %lu", cs.updateUs, cs.queryUs, (unsigned long)cs.loads);
	const aesound::Stats& as = aesound::stats();
	line(21, "snd %2u/%2u ch steal %lu us %4lu", as.active, as.physical, (unsigned long)as.stolen,
	     (unsigned long)audio::stats().serviceUs);
	const peds::Stats& ps = peds::stats();
	line(22, "ped u%4lu d%4lu car %4lu %4lu", (unsigned long)ps.updateUs, (unsigned long)ps.drawUs,
	     (unsigned long)veh::stats().stepUs, (unsigned long)veh::stats().drawUs);
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
	line(14, "anim %2d clips %3uK budget %uK", animstore::residentClips(),
	     (unsigned)(animstore::residentBytes() >> 10), (unsigned)(mem::budget::ANIM_CACHE_BYTES >> 10));
	line(15, "heap check: %s", s_heapCheck < 0 ? "-" : s_heapCheck ? "\x1b[32mOK\x1b[39m" : "\x1b[31mCORRUPT\x1b[39m");
	int c = 0;
	if (button(16, c, "check heap")) s_heapCheck = mem::check() ? 1 : 0;
	if (button(16, c, "trim anims")) animstore::trim(0);
	const aebank::Stats& ab = aebank::stats();
	line(17, "audio %3u snd %3luK/%uK q%u f%u", ab.resident, (unsigned long)(ab.bytes >> 10),
	     (unsigned)(mem::budget::AUDIO_CACHE_BYTES >> 10), ab.queued, ab.failed);
	line(18, "audio loads %lu evict %lu trk %ld", (unsigned long)ab.loads, (unsigned long)ab.evictions,
	     aestream::stats().trackId == ~0u ? -1L : (long)aestream::stats().trackId);
	const vehmdl::Stats& vm = vehmdl::stats();
	const pedmodel::Stats& pm = pedmodel::stats();
	line(19, "veh mdl %u/%u tex %u %luK", vm.ready, vm.models, vm.textures, (unsigned long)(vm.texVram >> 10));
	line(20, "ped mdl %u/%u %luK tex %luK", pm.ready, pm.models, (unsigned long)(pm.listBytes >> 10),
	     (unsigned long)(pm.texVram >> 10));
}

static void notInGame() { line(2, "\x1b[33m(start a game first)\x1b[39m"); }

// GAME: read-only state (every action lives in CHTS)
static void pageGame(bool inGame) {
	if (!inGame) { notInGame(); return; }
	const player::State& p = player::g;
	line(1, "\x1b[33mCJ\x1b[39m hp %3d/%3d arm %3d/%3d", (int)p.health, (int)p.maxHealth, (int)p.armour, (int)p.maxArmour);
	line(2, "$%d  wanted %d  sprint %d%%", p.money, p.wantedLevel, (int)p.sprint);
	line(3, "weapon %d %.12s %d/%d", p.weaponType, p.weapon.txd ? p.weapon.txd : "fists", p.weapon.clip, p.weapon.total);
	line(4, "fat %4d muscle %4d %.10s", (int)p.fat, (int)p.muscle, cj::outfitName(cj::outfit()));
	line(5, "clock %02u:%02u day %u  area %d", player::g_clockH, player::g_clockM, player::g_clockDay, world::area());
	line(6, "zone %.24s", p.zoneName ? (const char*)p.zoneName : "-");
	line(7, "car task %d  in vehicle %d", player::vehicleId(), (int)p.inVehicle);

	const int cur = currentCar();
	const veh::Vehicle* v = veh::get(cur);
	if (v) {
		static const char* const k_st[] = { "-", "phys", "PLAYER", "WRECK" };
		line(9, "\x1b[33mveh\x1b[39m #%d %-6s hp%4d %3dkm/h g%d %s", cur, k_st[v->status & 3], (int)v->health,
		     (int)(fabsf(v->speedFwd) * 50.0f * 3.6f), v->gear, v->inWater ? "wet" : "");
		line(10, "susp %.2f %.2f %.2f %.2f %s", v->susp[0], v->susp[1], v->susp[2], v->susp[3], v->asleep ? "zz" : "");
		line(12, "lean %+.2f rotor %.2f", v->lean, v->rotor);
		char d[vehmdl::COMP_COUNT + 1];
		for (int k = 0; k < vehmdl::COMP_COUNT; k++) d[k] = "o~x"[v->dmg[k] < 3 ? v->dmg[k] : 0];
		d[vehmdl::COMP_COUNT] = 0;
		line(11, "dmg %s t%x", d + vehmdl::COMP_DOOR_LF, v->burst);
	} else {
		line(9, "\x1b[33mveh\x1b[39m (none)");
	}
	const vehmdl::Stats& ms = vehmdl::stats();
	const veh::Stats& vs = veh::stats();
	line(13, "m%u/%u t%u %luK n%u traffic %u sk%u", ms.ready, ms.models, ms.textures, (unsigned long)(ms.texVram >> 10),
	     vs.active, vs.traffic, vs.skids);

	const peds::Stats& ps = peds::stats();
	line(15, "\x1b[33mpeds\x1b[39m %2u/%-2u c%u g%u p%u d%u", ps.live, ps.maxPeds, ps.civ, ps.gang, ps.cops, ps.dealers);
	line(16, "zone t%u %s slot%u mdl %u", ps.zoneType, ps.weekend ? "wkend" : "wkday", ps.slot, ps.models);
	line(17, "drawn %u posed %u polys %u", ps.drawn, ps.posed, ps.polys);
	line(18, "spawned %u removed %u", ps.spawned, ps.removed);
	int best = -1;
	float bestD = 1e9f;
	peds::Info in;
	for (int i = 0; i < peds::MAX; i++)
		if (peds::info(i, in)) {
			const float dx = in.x - p.x, dy = in.y - p.y;
			if (dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; best = i; }
		}
	if (best >= 0 && peds::info(best, in))
		line(19, "#%d id%d %-7s hp%3d %.0fm t%d", best, in.modelId, peds::taskName(in.task), (int)in.health, sqrtf(bestD), in.target);
	else
		line(19, "(no peds)");
}

static void toggleRow(int row, const char* name, bool& v) {
	line(row, "%s", name);
	toggle(row, 18, v);
}

// Two toggles on one row: name at col 0 / 13, buttons at col 7 / 24.
static void toggle2(int row, const char* a, bool& va, const char* b, bool& vb) {
	line(row, "%-6.6s        %-10.10s", a, b);
	toggle(row, 7, va);
	toggle(row, 24, vb);
}

// REND: render / simulation switches
static void pageRend() {
	dbg::Options& o = dbg::opts;
	toggle2(1, "CJ", o.drawCj, "skeleton", o.skeleton);
	toggle2(2, "ground", o.ground, "fixed 60Hz", o.fixedStep);
	line(3, "sim speed x%d.%02d", o.simSpeedQ / 4, o.simSpeedQ % 4 * 25);
	int c = 17;
	if (button(3, c, "-") && o.simSpeedQ > 0) o.simSpeedQ--;
	if (button(3, c, "+") && o.simSpeedQ < 8) o.simSpeedQ++;
	if (button(3, c, o.simSpeedQ ? "||" : ">")) o.simSpeedQ = o.simSpeedQ ? 0 : 4;
	line(4, "camera dist %.1f", o.camDist);
	c = 18;
	if (button(4, c, "-")) o.camDist = clampf(o.camDist - 0.4f, 1.2f, 12.0f);
	if (button(4, c, "+")) o.camDist = clampf(o.camDist + 0.4f, 1.2f, 12.0f);
	toggle2(5, "map", o.world, "water", o.water);
	line(6, "map poly budget %4d", o.worldPolys);
	c = 22;
	if (button(6, c, "-") && o.worldPolys > 400) o.worldPolys -= 100;
	if (button(6, c, "+") && o.worldPolys < 2048) o.worldPolys += 100;
	line(7, "ped poly budget %4d", o.pedPolys);
	c = 22;
	if (button(7, c, "-") && o.pedPolys > 100) o.pedPolys -= 100;
	if (button(7, c, "+") && o.pedPolys < 1200) o.pedPolys += 100;
	toggle2(8, "colls", o.collision, "outline", o.colOutline);
	line(9, "col range %2dm  polys %4d", o.colRange, o.colPolys);
	c = 0;
	if (button(10, c, "r-") && o.colRange > 2) o.colRange -= 2;
	if (button(10, c, "r+") && o.colRange < 40) o.colRange += 2;
	if (button(10, c, "p-") && o.colPolys > 100) o.colPolys -= 100;
	if (button(10, c, "p+") && o.colPolys < 1800) o.colPolys += 100;
	toggle2(11, "physic", o.physics, "veh lights", o.vehLights);
	toggleRow(12, "skidmarks", o.skidmarks);
	toggle2(13, "wires", o.worldWires, "clutter", o.worldClutter);
	toggleRow(15, "telegraph poles", o.worldPoles);
	const world::Stats& ws = world::stats();
	line(16, "hidden w%u junk%u pole%u", ws.hiddenWires, ws.hiddenClutter, ws.hiddenPoles);
	c = 0;
	if (button(14, c, "repaint HUD")) chud::invalidate();
	if (button(14, c, "defaults")) o = dbg::Options();
}

// SKY: time cycle / weather state and the sky render switches
static void pageSky(bool inGame) {
	if (!inGame) { notInGame(); return; }
	dbg::Options& o = dbg::opts;
	const weather::State& w = weather::g;
	const timecycle::Colours& c = timecycle::current();
	line(1, "time %02u:%02u:%02u  day %u/%u  x%d", player::g_clockH, player::g_clockM, player::clockSeconds(),
	     player::g_clockDay, player::g_clockMonth, o.clockRate);
	line(2, "w %s>%s %2d%%", weather::name(w.oldType), weather::name(w.newType), (int)(w.interpolation * 100.0f));
	line(3, "rgn %s  frc %s", weather::regionName(w.region), weather::name(w.forcedType));
	line(4, "rain %.2f sand %.2f fog %.2f", w.rain, w.sandstorm, w.foggyness);
	line(5, "cloud %.2f xsun %.2f wind %.2f", w.cloudCoverage, w.extraSunnyness, w.wind);
	line(6, "glare %.2f rbow %.2f wet %.2f", w.sunGlare, w.rainbow, w.wetRoads);
	const float* s = timecycle::vectorToSun();
	line(7, "sun %+.2f %+.2f %+.2f dn %.2f", s[0], s[1], s[2], timecycle::dayNightBalance());
	line(8, "skyT %3d %3d %3d B %3d %3d %3d", c.skyTopR, c.skyTopG, c.skyTopB, c.skyBotR, c.skyBotG, c.skyBotB);
	u8 pr, pg, pb;
	timecycle::pedLight(pr, pg, pb);
	line(9, "ped light %3d %3d %3d", pr, pg, pb);
	line(10, "far %d fog %d view %d", c.farClip, c.fogStart, o.fogView);
	int col = 23;
	if (button(10, col, "-") && o.fogView > 40) o.fogView -= 20;
	if (button(10, col, "+") && o.fogView < 200) o.fogView += 20;
	toggle2(12, "sky", o.sky, "sprites", o.skySprites);
	toggle2(13, "fog", o.fog, "weather fx", o.weatherFx);
	line(14, "ped lit");
	toggle(14, 7, o.pedLight);
}

// ---- CHTS: every action, listed top to bottom; pages only when a category
// does not fit rows 1..21 (row 22 = page control, row 23 = categories)
static constexpr int ITEM_TOP = 1, ITEM_ROWS = 21;
static int s_item, s_itemPage;

// Row of the next item on the current page, or -1 (it is on another page).
static int item() {
	const int r = s_item++ - s_itemPage * ITEM_ROWS;
	return r >= 0 && r < ITEM_ROWS ? ITEM_TOP + r : -1;
}
static int header(const char* text) {
	const int r = item();
	if (r >= 0) line(r, "\x1b[33m%s\x1b[39m", text);
	return r;
}

static void cheatsPlayer() {
	player::State& p = player::g;
	int r, c;
	if ((r = item()) >= 0) {
		line(r, "$%d", p.money);
		c = 12;
		if (button(r, c, "+1k")) p.money += 1000;
		if (button(r, c, "+100k")) p.money += 100000;
		if (button(r, c, "0")) p.money = 0;
	}
	if ((r = item()) >= 0) {
		line(r, "Health %d/%d", (int)p.health, (int)p.maxHealth);
		c = 17;
		if (button(r, c, "-25")) p.health = clampf(p.health - 25, 0, p.maxHealth);
		if (button(r, c, "max")) p.health = p.maxHealth;
	}
	if ((r = item()) >= 0) {
		line(r, "Armour %d/%d", (int)p.armour, (int)p.maxArmour);
		c = 17;
		if (button(r, c, "-25")) p.armour = clampf(p.armour - 25, 0, p.maxArmour);
		if (button(r, c, "max")) p.armour = p.maxArmour;
	}
	if ((r = item()) >= 0) {
		line(r, "Wanted");
		c = 7;
		for (int i = 0; i <= 5; i++) {
			char l[2] = { (char)('0' + i), 0 };
			if (button(r, c, l, p.wantedLevel == i)) player::setWantedLevel(i);
		}
	}
	if ((r = item()) >= 0) {
		line(r, "Full heal / sprint");
		c = 20;
		if (button(r, c, "heal")) { p.health = p.maxHealth; p.armour = p.maxArmour; player::refillSprint(); }
	}
	if ((r = item()) >= 0) {
		line(r, "CJ speed x%d.%02d", dbg::opts.playerSpeedQ / 4, dbg::opts.playerSpeedQ % 4 * 25);
		c = 23;
		if (button(r, c, "<") && dbg::opts.playerSpeedQ > 1) dbg::opts.playerSpeedQ--;
		if (button(r, c, ">") && dbg::opts.playerSpeedQ < 16) dbg::opts.playerSpeedQ++;
	}
	if ((r = item()) >= 0) {
		line(r, "Weapon %d", player::weaponTypeAt(s_weaponIdx));
		c = 13;
		if (button(r, c, "<") && --s_weaponIdx < 1) {
			s_weaponIdx = 1;
			while (player::weaponTypeAt(s_weaponIdx + 1) >= 0) s_weaponIdx++;
		}
		if (button(r, c, ">") && player::weaponTypeAt(++s_weaponIdx) < 0) s_weaponIdx = 1;
		if (button(r, c, "give")) player::giveWeapon(player::weaponTypeAt(s_weaponIdx), 200);
	}
	if ((r = item()) >= 0) {
		line(r, "All weapons");
		c = 15;
		if (button(r, c, "give")) player::giveAllWeapons();
		if (button(r, c, "clear")) player::clearWeapons();
	}
	if ((r = item()) >= 0) {
		line(r, "Fat %d", (int)p.fat);
		c = 13;
		if (button(r, c, "0")) player::setBodyStats(0, p.muscle);
		if (button(r, c, "-")) player::setBodyStats(p.fat - 250, p.muscle);
		if (button(r, c, "+")) player::setBodyStats(p.fat + 250, p.muscle);
		if (button(r, c, "max")) player::setBodyStats(1000, p.muscle);
	}
	if ((r = item()) >= 0) {
		line(r, "Muscle %d", (int)p.muscle);
		c = 13;
		if (button(r, c, "0")) player::setBodyStats(p.fat, 0);
		if (button(r, c, "-")) player::setBodyStats(p.fat, p.muscle - 250);
		if (button(r, c, "+")) player::setBodyStats(p.fat, p.muscle + 250);
		if (button(r, c, "max")) player::setBodyStats(p.fat, 1000);
	}
	if ((r = item()) >= 0) {
		line(r, "Outfit %.10s", cj::outfitName(cj::outfit()));
		c = 24;
		const int n = cj::outfitCount();
		if (n && button(r, c, "<")) cj::setOutfit((cj::outfit() + n - 1) % n);
		if (n && button(r, c, ">")) cj::setOutfit((cj::outfit() + 1) % n);
	}
	if ((r = item()) >= 0) {
		line(r, "Respawn");
		c = 20;
		if (button(r, c, "do")) player::respawn();
	}
}

static void cheatsVehicles() {
	int r, c;
	if (!vehdata::ready()) {
		if ((r = item()) >= 0) line(r, "vehicles.dveh not ready");
		return;
	}
	if (s_vehPick < 0) s_vehPick = vehdata::findModel(492) >= 0 ? vehdata::findModel(492) : nextOfCat(0, 1, VC_CAR);
	if ((r = item()) >= 0) {
		line(r, "Type %s", k_vcat[s_vcat]);
		c = 23;
		if (button(r, c, "<")) s_vcat = (s_vcat + VC_COUNT - 1) % VC_COUNT;
		if (button(r, c, ">")) s_vcat = (s_vcat + 1) % VC_COUNT;
	}
	if (s_vehPick < 0 || catOf(s_vehPick) != s_vcat) s_vehPick = nextOfCat(-1, 1, s_vcat);
	ds::VehInfo info;
	const bool have = s_vehPick >= 0 && vehdata::read(s_vehPick, info);
	if ((r = item()) >= 0) {
		line(r, "Spawn");
		c = 6;
		if (button(r, c, "<")) s_vehPick = nextOfCat(s_vehPick, -1, s_vcat);
		char label[16];
		snprintf(label, sizeof label, "%d %.8s", have ? info.modelId : 0, have ? info.model : "?");
		if (button(r, c, label) && have) spawnHere(s_vehPick);
		if (button(r, c, ">")) s_vehPick = nextOfCat(s_vehPick, 1, s_vcat);
	}
	if ((r = item()) >= 0) {
		line(r, "Random of type");
		c = 20;
		if (button(r, c, "spawn")) {
			for (int k = rand() % 8 + 1; k > 0; k--) s_vehPick = nextOfCat(s_vehPick, 1, s_vcat);
			spawnHere(s_vehPick);
		}
	}
	if ((r = item()) >= 0) {
		line(r, "Id %3d", s_spawnId);
		c = 7;
		if (button(r, c, "-10")) s_spawnId -= 10;
		if (button(r, c, "-")) s_spawnId -= 1;
		if (button(r, c, "+")) s_spawnId += 1;
		if (button(r, c, "+10")) s_spawnId += 10;
		if (s_spawnId < 400) s_spawnId = 611;
		if (s_spawnId > 611) s_spawnId = 400;
		if (button(r, c, "go")) {
			const int i = vehdata::findModel(s_spawnId);
			if (i >= 0 && vehdata::spawnable(i)) { s_vehPick = i; s_vcat = catOf(i); spawnHere(i); }
			else hud::log("\x1b[33mno vehicle %d\x1b[39m", s_spawnId);
		}
	}
	const int cur = currentCar();
	const veh::Vehicle* v = veh::get(cur);
	if ((r = item()) >= 0) {
		if (v) line(r, "Car #%d hp %d %dkm/h", cur, (int)v->health, (int)(fabsf(v->speedFwd) * 180.0f));
		else line(r, "Car (none)");
	}
	if ((r = item()) >= 0) {
		c = 0;
		if (button(r, c, "enter") && v) player::enterVehicleNow(cur);
		if (button(r, c, "repair")) veh::repair(cur);
		if (button(r, c, "flip")) veh::flip(cur);
		if (button(r, c, "boom")) veh::blowUp(cur);
	}
	if ((r = item()) >= 0) {
		c = 0;
		if (button(r, c, "fire") && v) veh::setHealth(cur, 249.0f);
		if (button(r, c, "tyres")) veh::burstTyre(cur, -1);
		if (button(r, c, "remove all")) { veh::removeAll(); s_lastCar = -1; }
	}
	if ((r = item()) >= 0) {
		line(r, "Always radio");
		toggle(r, 20, dbg::opts.alwaysRadio);
	}
	if ((r = item()) >= 0) {
		line(r, "Radio station %2d", FrontEndMenuManager.m_nRadioStation);
		c = 23;
		if (button(r, c, "<")) FrontEndMenuManager.scrollRadioStations(-1);
		if (button(r, c, ">")) FrontEndMenuManager.scrollRadioStations(1);
	}
}

static void cheatsPeds() {
	player::State& p = player::g;
	int r, c;
	if ((r = item()) >= 0) {
		line(r, "Peds %d live", peds::count());
		toggle(r, 20, dbg::opts.peds);
	}
	if ((r = item()) >= 0) {
		line(r, "Density %d%%", dbg::opts.pedDensity);
		c = 23;
		if (button(r, c, "<") && dbg::opts.pedDensity > 0) dbg::opts.pedDensity -= 25;
		if (button(r, c, ">") && dbg::opts.pedDensity < 200) dbg::opts.pedDensity += 25;
	}
	if ((r = item()) >= 0) {
		c = 0;
		if (button(r, c, "spawn ped")) peds::spawnNear(p.x, p.y, p.z);
		if (button(r, c, "remove all")) peds::removeAll();
	}
}

static void cheatsTime() {
	int r, c;
	if ((r = item()) >= 0) {
		line(r, "Clock %02u:%02u", player::g_clockH, player::g_clockM);
		c = 19;
		if (button(r, c, "-1h")) player::setClock(player::g_clockH + 23, player::g_clockM);
		if (button(r, c, "+1h")) player::setClock(player::g_clockH + 1, player::g_clockM);
	}
	if ((r = item()) >= 0) {
		line(r, "Hour");
		c = 9;
		for (int h = 0; h < 24; h += 6) {
			char label[3];
			snprintf(label, sizeof label, "%02d", h);
			if (button(r, c, label)) player::setClock(h, 0);
		}
	}
	if ((r = item()) >= 0) {
		line(r, "Rate x%d", dbg::opts.clockRate);
		c = 9;
		static const int k_rates[] = { 0, 1, 10, 60 };
		for (int k : k_rates) {
			char label[4];
			snprintf(label, sizeof label, "%d", k);
			if (button(r, c, label, dbg::opts.clockRate == k)) dbg::opts.clockRate = k;
		}
	}
	if ((r = item()) >= 0) {
		line(r, "Weather %.12s", weather::name((weather::Type)s_cheatWeather));
		c = 23;
		if (button(r, c, "<")) s_cheatWeather = (s_cheatWeather + weather::NUM_PLAYABLE - 1) % weather::NUM_PLAYABLE;
		if (button(r, c, ">")) s_cheatWeather = (s_cheatWeather + 1) % weather::NUM_PLAYABLE;
	}
	if ((r = item()) >= 0) {
		c = 0;
		if (button(r, c, "now")) weather::forceWeatherNow((weather::Type)s_cheatWeather);
		if (button(r, c, "next hour")) weather::forceWeather((weather::Type)s_cheatWeather);
		if (button(r, c, "auto")) weather::releaseWeather();
	}
	if ((r = item()) >= 0) {
		c = 0;
		if (button(r, c, "sun")) weather::forceWeatherNow(weather::EXTRASUNNY_LA);
		if (button(r, c, "cld")) weather::forceWeatherNow(weather::CLOUDY_LA);
		if (button(r, c, "rain")) weather::forceWeatherNow(weather::RAINY_SF);
		if (button(r, c, "fog")) weather::forceWeatherNow(weather::FOGGY_SF);
		if (button(r, c, "sand")) weather::forceWeatherNow(weather::SANDSTORM_DESERT);
	}
	if ((r = item()) >= 0) {
		line(r, "Lightning");
		c = 20;
		if (button(r, c, "flash")) weather::debugFlash();
	}
	if ((r = item()) >= 0) line(r, "Now %.12s > %.12s", weather::name(weather::g.oldType), weather::name(weather::g.newType));
}

static void cheatsWorld() {
	int r, c;
	if ((r = item()) >= 0) line(r, "IPL exits: %d outside / %d", entryexits::exteriorCount(), entryexits::count());
	if ((r = item()) >= 0) {
		line(r, "All interiors");
		c = 20;
		if (button(r, c, entryexits::unlocked() ? "open" : "unlock", entryexits::unlocked()) && !entryexits::unlocked()) {
			if (!entryexits::unlockAll()) hud::log("entry_exits.denx missing");
			else chud::invalidate();
		}
	}
}

static void cheatsWarp() {
	struct Warp { const char* name; float x, y, z; };
	static const Warp k_ls[] = { { "Grove Street", 2495.0f, -1664.0f, 13.5f }, { "Downtown LS", 1481.0f, -1731.0f, 13.4f },
	                             { "LS Airport", 1684.0f, -2334.0f, 13.5f }, { "Santa Maria", 342.0f, -1800.0f, 4.7f } };
	static const Warp k_sf[] = { { "Downtown SF", -2150.0f, 550.0f, 35.0f }, { "SF Garage", -2030.0f, 160.0f, 28.8f } };
	static const Warp k_lv[] = { { "The Strip", 2027.0f, 1343.0f, 10.8f }, { "North LV", 2515.0f, 1975.0f, 10.8f },
	                             { "South LV", 1942.0f, 1073.0f, 10.8f } };
	static const Warp k_cs[] = { { "Desert Strip", 400.0f, 2500.0f, 16.5f }, { "Bone County", -68.5f, 1020.75f, 19.7f },
	                             { "Mt. Chiliad", -2326.0f, -1625.0f, 483.7f }, { "Dillimore", 655.0f, -575.0f, 16.3f },
	                             { "Palomino Ck", 2230.0f, -112.0f, 26.5f } };
	struct Group { const char* name; const Warp* w; int n; };
	static const Group k_groups[] = { { "LOS SANTOS", k_ls, 4 }, { "SAN FIERRO", k_sf, 2 },
	                                  { "LAS VENTURAS", k_lv, 3 }, { "COUNTRYSIDE / DESERT", k_cs, 5 } };
	for (const Group& g : k_groups) {
		header(g.name);
		// two places per row
		for (int i = 0; i < g.n; i += 2) {
			const int r = item();
			if (r < 0) continue;
			for (int k = i; k < i + 2 && k < g.n; k++) {
				int c = k == i ? 1 : 16;
				char label[14];
				snprintf(label, sizeof label, "%.12s", g.w[k].name);
				if (button(r, c, label)) player::teleport(g.w[k].x, g.w[k].y, g.w[k].z, 0.0f);
			}
		}
	}
}

static void pageCheats(bool inGame) {
	static const char* const cats[] = { "CJ", "VEH", "PEDS", "TIME", "WRLD", "WARP" };
	static const int cols[] = { 0, 5, 10, 15, 20, 25, 30 };
	for (int i = 0; i < 6; i++) {
		char buf[16];
		snprintf(buf, sizeof buf, "%s%.*s", i == s_cheatCat ? "\x1b[33m" : "\x1b[37m", cols[i + 1] - cols[i], cats[i]);
		write(23, cols[i], buf);
		if (s_tapRow == 23 && s_tapCol >= cols[i] && s_tapCol < cols[i + 1]) {
			s_cheatCat = i;
			s_tapRow = -1;
		}
	}
	if (!inGame) { notInGame(); return; }
	s_item = 0;
	s_itemPage = s_cheatPage[s_cheatCat];
	switch (s_cheatCat) {
	case 0: cheatsPlayer(); break;
	case 1: cheatsVehicles(); break;
	case 2: cheatsPeds(); break;
	case 3: cheatsTime(); break;
	case 4: cheatsWorld(); break;
	default: cheatsWarp(); break;
	}
	const int pages = (s_item + ITEM_ROWS - 1) / ITEM_ROWS;
	if (s_itemPage >= pages && pages) s_cheatPage[s_cheatCat] = pages - 1;
	if (pages > 1) {
		line(22, "page %d/%d", s_itemPage + 1, pages);
		int c = 19;
		if (button(22, c, "<") && s_itemPage > 0) s_cheatPage[s_cheatCat]--;
		if (button(22, c, ">") && s_itemPage + 1 < pages) s_cheatPage[s_cheatCat]++;
	}
}

// ---- frame --------------------------------------------------------------------

void enter() {
	s_repaint = true;
	consoleClear();
	clearBuffer();
	memset(s_shownCh, ' ', sizeof s_shownCh);   // what consoleClear left
	memset(s_shownCol, DEFAULT_COL, sizeof s_shownCol);
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
	// stats change every frame but nobody reads them at 60 Hz: rebuild the
	// page every few frames, or at once when something was tapped
	if (!s_repaint && !in.tapped && ++s_frameNo % REFRESH) return;
	// now and then rewrite everything (something else may have printed)
	if (s_frameNo % 600 == 0) invalidateShown();
	clearBuffer();
	// tab bar: the console has no background colours, so the active tab is
	// marked in yellow; four columns per tab leave room for CHTS
	for (int t = 0; t < TAB_COUNT; t++) {
		char buf[16];
		snprintf(buf, sizeof buf, "%s%-4.4s", t == s_tab ? "\x1b[33m" : "\x1b[37m", k_tabs[t]);
		write(0, t * TAB_W, buf);
	}
	switch (s_tab) {
	case TAB_LOG: pageLog(); break;
	case TAB_PERF: pagePerf(); break;
	case TAB_MEM: pageMem(); break;
	case TAB_GAME: pageGame(inGame); break;
	case TAB_SKY: pageSky(inGame); break;
	case TAB_CHTS: pageCheats(inGame); break;
	default: pageRend(); break;
	}
	flush();
	s_repaint = false;
}

#ifdef GTASADS_DEBUG
void debugShow(int tab, int cheatCat) {
	s_tab = tab;
	s_cheatCat = cheatCat;
	s_repaint = true;
}

void debugDump(FILE* f) {
	for (int r = 0; r < ROWS; r++) fprintf(f, "%2d|%.*s|\n", r, COLS, s_shownCh[r]);
}
#endif

}
