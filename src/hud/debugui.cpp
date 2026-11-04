#include "hud/debugui.h"

#include <nds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "assets/texture.h"
#include "game/animstore.h"
#include "game/cj.h"
#include "game/debugopts.h"
#include "game/hud/chud.h"
#include "game/hud/radar.h"
#include "game/player.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"
#include "render/renderer.h"

namespace dbgui {

enum Tab { TAB_LOG, TAB_PERF, TAB_MEM, TAB_GAME, TAB_REND, TAB_COUNT };
static const char* const k_tabs[TAB_COUNT] = { "LOG", "PERF", "MEM", "GAME", "REND" };
static constexpr int TAB_W = 6;          // characters per tab
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
	blank(15, 23);
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
		char a[16], b[16] = "";
		snprintf(a, sizeof a, "%-6s%5uK", mem::tagName((mem::Tag)t), (unsigned)(mem::tagBytes((mem::Tag)t) >> 10));
		if (t + 1 < mem::TAG_COUNT)
			snprintf(b, sizeof b, "%-6s%5uK", mem::tagName((mem::Tag)(t + 1)),
			         (unsigned)(mem::tagBytes((mem::Tag)(t + 1)) >> 10));
		line(row, "%s   %s", a, b);
	}
	line(13, "vram %3uK in %d textures", (unsigned)(tex::vramBytes() >> 10), tex::loadedCount());
	line(14, "anim %2d clips %3uK (budget %uK)", animstore::residentClips(),
	     (unsigned)(animstore::residentBytes() >> 10), (unsigned)(mem::budget::ANIM_CACHE_BYTES >> 10));
	line(15, "heap check: %s", s_heapCheck < 0 ? "-" : s_heapCheck ? "\x1b[32mOK\x1b[39m" : "\x1b[31mCORRUPT\x1b[39m");
	put(16, 0, "");
	int c = 0;
	if (button(16, c, "check heap")) s_heapCheck = mem::check() ? 1 : 0;
	if (button(16, c, "trim anims")) animstore::trim(0);
	blank(17, 23);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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
	if (button(1, c, "-")) p.wantedLevel = p.wantedLevel > 0 ? p.wantedLevel - 1 : 0;
	if (button(1, c, "+")) p.wantedLevel = p.wantedLevel < 6 ? p.wantedLevel + 1 : 6;
	if (button(1, c, "0")) p.wantedLevel = 0;

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
	blank(15, 23);
}

static void toggleRow(int row, const char* name, bool& v) {
	line(row, "%s", name);
	int c = 18;
	if (button(row, c, v ? "ON " : "OFF")) v = !v;
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
	blank(9, 23);
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
	// bracketed and yellow
	for (int t = 0; t < TAB_COUNT; t++) {
		char lbl[8];
		snprintf(lbl, sizeof lbl, t == s_tab ? "[%s]" : " %s ", k_tabs[t]);
		printf(t == s_tab ? "\x1b[33m%-*s\x1b[39m" : "\x1b[37m%-*s\x1b[39m", TAB_W, lbl);
	}
	switch (s_tab) {
	case TAB_LOG: pageLog(); break;
	case TAB_PERF: pagePerf(); break;
	case TAB_MEM: pageMem(); break;
	case TAB_GAME: pageGame(inGame); break;
	default: pageRend(); break;
	}
	s_repaint = false;
	(void)ROWS;
}

}
