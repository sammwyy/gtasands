#include "game/peddata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud/hud.h"
#include "game/player.h"
#include "mem/mem.h"
#include "platform/timing.h"

namespace peddata {

static FILE* s_file;
static ds::PedHeader s_hdr;
static ds::PedModel* s_models;
static ds::PedStat* s_stats;
static ds::PedAnimGroup* s_anims;
static ds::PedGroup* s_groups;
static uint16_t* s_members;
static ds::PedZone* s_zones;
static ds::PedGroup* s_carGroups;
static uint16_t* s_carMembers;
static int s_step;
static int s_initWait;
static Stats s_st;
static int s_rowKey = -1;
static ds::PopRow s_row;
static constexpr int CACHE_CELLS = 32, CACHE_RADIUS = 2;
struct PathChunk {
	int16_t cx, cy;
	ds::PathCell cell;
	uint32_t linkFirst, linkCount, bytes;
	ds::PathNode* nodes;
	uint16_t* links;
	bool used;
};
static PathChunk s_paths[2][CACHE_CELLS];
static int s_centerX = -9999, s_centerY = -9999;
struct PendingChunk {
	int16_t cx, cy;
	uint8_t kind, stage;
	ds::PathCell cell;
	uint32_t linkFirst, linkCount;
	ds::PathNode* nodes;
	uint16_t* links;
	bool active;
};
static PendingChunk s_pending;
static int s_nextKind;

static bool readAt(uint32_t off, void* dst, uint32_t n) {
	return s_file && !fseek(s_file, (long)off, SEEK_SET) && fread(dst, 1, n, s_file) == n;
}

// one table per call: allocate + read
template <typename T> static bool table(T*& out, uint32_t off, uint32_t count) {
	const uint32_t n = count * sizeof(T);
	out = (T*)mem::alloc(n ? n : 4, mem::TAG_PED);
	if (!out || !readAt(off, out, n)) return false;
	s_st.bytes += n;
	return true;
}

bool ready() { return s_step == 7; }

static void clearChunk(PathChunk& c) {
	if (!c.used) return;
	mem::free(c.nodes);
	mem::free(c.links);
	s_st.bytes -= c.bytes;
	c = {};
}

static PathChunk* chunk(int kind, int cx, int cy) {
	for (PathChunk& c : s_paths[kind])
		if (c.used && c.cx == cx && c.cy == cy) return &c;
	return nullptr;
}

static void discardPending() {
	mem::free(s_pending.nodes);
	mem::free(s_pending.links);
	s_pending = {};
}

static bool stepChunk() {
	PendingChunk& p = s_pending;
	const int kind = p.kind;
	PathChunk* c = nullptr;
	for (PathChunk& slot : s_paths[kind])
		if (!slot.used) { c = &slot; break; }
	if (!c) return false;
	if (p.stage == 0) {
		const uint32_t cellOff = (kind ? s_hdr.roadCellOffset : s_hdr.cellOffset) +
			(p.cy * s_hdr.gridW + p.cx) * sizeof(ds::PathCell);
		if (!readAt(cellOff, &p.cell, sizeof p.cell)) return false;
		if ((uint32_t)p.cell.first + p.cell.count > (kind ? s_hdr.numRoadNodes : s_hdr.numNodes)) return false;
		if (p.cell.count) {
			p.nodes = (ds::PathNode*)mem::alloc(p.cell.count * sizeof(ds::PathNode), mem::TAG_PED);
			if (!p.nodes) return false;
			p.stage = 1;
			return false;
		}
	}
	if (p.stage == 1) {
		const uint32_t nodeOff = (kind ? s_hdr.roadNodeOffset : s_hdr.nodeOffset) +
			p.cell.first * sizeof(ds::PathNode);
		if (!readAt(nodeOff, p.nodes, p.cell.count * sizeof(ds::PathNode))) return false;
		p.linkFirst = p.nodes[0].firstLink;
		p.linkCount = p.nodes[p.cell.count - 1].firstLink + p.nodes[p.cell.count - 1].numLinks - p.linkFirst;
		if (p.linkFirst + p.linkCount > (kind ? s_hdr.numRoadLinks : s_hdr.numLinks)) return false;
		if (p.linkCount) {
			p.links = (uint16_t*)mem::alloc(p.linkCount * sizeof(uint16_t), mem::TAG_PED);
			if (!p.links) return false;
			p.stage = 2;
			return false;
		}
	}
	if (p.stage == 2 && !readAt((kind ? s_hdr.roadLinkOffset : s_hdr.linkOffset) +
	                            p.linkFirst * sizeof(uint16_t), p.links, p.linkCount * sizeof(uint16_t)))
		return false;
	*c = { p.cx, p.cy, p.cell, p.linkFirst, p.linkCount,
	       p.cell.count * sizeof(ds::PathNode) + p.linkCount * sizeof(uint16_t), p.nodes, p.links, true };
	s_st.bytes += c->bytes;
	s_st.pathLoads++;
	p.nodes = nullptr;
	p.links = nullptr;
	discardPending();
	s_nextKind = 1 - kind;
	return true;
}

static bool service(float x, float y) {
	const int cx = (int)(x - s_hdr.gridX0) / s_hdr.cellSize;
	const int cy = (int)(y - s_hdr.gridY0) / s_hdr.cellSize;
	if (cx < 0 || cy < 0 || cx >= s_hdr.gridW || cy >= s_hdr.gridH) return false;
	if (cx != s_centerX || cy != s_centerY) {
		s_centerX = cx;
		s_centerY = cy;
		if (s_pending.active &&
		    (abs(s_pending.cx - cx) > CACHE_RADIUS || abs(s_pending.cy - cy) > CACHE_RADIUS))
			discardPending();
		for (auto& paths : s_paths)
			for (PathChunk& c : paths)
				if (c.used && (abs(c.cx - cx) > CACHE_RADIUS || abs(c.cy - cy) > CACHE_RADIUS))
					clearChunk(c);
	}
	if (s_pending.active) { stepChunk(); return false; }
	for (int i = 0; i < 2; i++) {
		const int kind = (i + s_nextKind) & 1;
		for (int radius = 0; radius <= CACHE_RADIUS; radius++)
			for (int dy = -radius; dy <= radius; dy++)
				for (int dx = -radius; dx <= radius; dx++) {
					if (abs(dx) != radius && abs(dy) != radius) continue;
					const int xx = cx + dx, yy = cy + dy;
					if (xx < 0 || yy < 0 || xx >= s_hdr.gridW || yy >= s_hdr.gridH || chunk(kind, xx, yy)) continue;
					s_pending = { (int16_t)xx, (int16_t)yy, (uint8_t)kind, 0, {}, 0, 0, nullptr, nullptr, true };
					stepChunk();
					return false;
				}
	}
	return true;
}

void update(float x, float y) {
	if (!ready()) return;
	const u32 t0 = timing::ticks();
	service(x, y);
	s_st.pathUs = (uint16_t)timing::usSince(t0);
	if (s_st.pathUs > s_st.pathPeakUs) s_st.pathPeakUs = s_st.pathUs;
}

bool initStep() {
	if (s_step >= 7) return true;   // a second new game: the tables are still there
	bool ok = true;
	switch (s_step++) {
	case 0:
		s_file = fopen("game:/data/peds.dped", "rb");
		if (!s_file || fread(&s_hdr, sizeof s_hdr, 1, s_file) != 1 || s_hdr.magic != DS_FOURCC('D', 'P', 'E', 'D') ||
		    s_hdr.version != 5) {
			hud::log("\x1b[33mpeds.dped missing/old (make assets)\x1b[39m");
			if (s_file) fclose(s_file);
			s_file = nullptr;
			s_step = 99;
			return true;
		}
		return false;
	case 1: ok = table(s_models, s_hdr.modelOffset, s_hdr.numModels) && table(s_stats, s_hdr.statOffset, s_hdr.numStats); break;
	case 2: {
		ok = table(s_anims, s_hdr.animOffset, s_hdr.numAnimGroups) && table(s_groups, s_hdr.groupOffset, s_hdr.numGroups);
		uint32_t members = 0;
		for (int g = 0; ok && g < s_hdr.numGroups; g++)
			if (s_groups[g].first + s_groups[g].count > members) members = s_groups[g].first + s_groups[g].count;
		ok = ok && table(s_members, s_hdr.groupOffset + s_hdr.numGroups * sizeof(ds::PedGroup), members);
		break;
	}
	case 3: ok = table(s_zones, s_hdr.zoneOffset, s_hdr.numZones); break;
	case 4: break;
	case 5: {
		ok = table(s_carGroups, s_hdr.carGroupOffset, 29);
		uint32_t count = 0;
		for (int g = 0; ok && g < 29; g++)
			if (s_carGroups[g].first + s_carGroups[g].count > count) count = s_carGroups[g].first + s_carGroups[g].count;
		ok = ok && table(s_carMembers, s_hdr.carGroupOffset + 29 * sizeof(ds::PedGroup), count);
		break;
	}
	case 6:
		if (!service(player::g.x, player::g.y)) {
			if (++s_initWait > 900) {
				hud::log("\x1b[31mpeds.dped: path preload failed\x1b[39m");
				s_step = 99;
				return true;
			}
			s_step--;
			return false;
		}
		s_step = 7;
		s_st.models = s_hdr.numModels;
		s_st.nodes = (uint16_t)s_hdr.numNodes;
		s_st.zones = s_hdr.numZones;
		hud::log("peds: %u models %u nodes %u zones %luK", s_st.models, s_st.nodes, s_st.zones,
		         (unsigned long)(s_st.bytes >> 10));
		return true;
	default: return true;
	}
	if (!ok) {
		hud::log("\x1b[31mpeds.dped: read failed\x1b[39m");
		s_step = 99;
		return true;
	}
	return false;
}

const Stats& stats() { return s_st; }
int numModels() { return s_models ? s_hdr.numModels : 0; }
const ds::PedModel* model(int i) { return (unsigned)i < (unsigned)numModels() ? &s_models[i] : nullptr; }

int findModel(int id) {
	for (int i = 0; i < numModels(); i++)
		if (s_models[i].modelId == id) return i;
	return -1;
}

bool name(int i, ds::PedName& out) {
	return (unsigned)i < (unsigned)numModels() && readAt(s_hdr.nameOffset + i * sizeof(ds::PedName), &out, sizeof out);
}

const ds::PedStat* stat(int i) {
	if (!s_stats || !s_hdr.numStats) return nullptr;
	return &s_stats[(unsigned)i < s_hdr.numStats ? i : 0];
}

const ds::PedAnimGroup* animGroup(int i) {
	if (!s_anims || !s_hdr.numAnimGroups) return nullptr;
	return &s_anims[(unsigned)i < s_hdr.numAnimGroups ? i : 0];
}

int groupSize(int g) { return s_groups && (unsigned)g < s_hdr.numGroups ? s_groups[g].count : 0; }
int groupMember(int g, int k) { return (unsigned)k < (unsigned)groupSize(g) ? s_members[s_groups[g].first + k] : -1; }

int pedGroupOf(int pg, int zone) {
	// ePopcycleGroup -> first ePopcyclePedGroup; the groups with LA / SF / VG
	// variants take three consecutive lines in pedgrp.dat
	static const uint8_t k_base[18] = { 0, 3, 6, 9, 10, 11, 14, 17, 20, 23, 26, 29, 30, 33, 36, 39, 40, 41 };
	static const uint8_t k_triple[18] = { 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0 };
	if (zone < 0 || zone > 2) zone = 0;
	if (pg >= 0 && pg < 18) return k_base[pg] + (k_triple[pg] ? zone : 0);
	if (pg >= 18 && pg < 33) return pg + 24;   // gangs .. wives: one line each from 42
	return -1;
}

bool popRow(int zoneType, int weekend, int slot, ds::PopRow& out) {
	if (!s_file || zoneType < 0 || zoneType >= 20) return false;
	const int key = (zoneType * 2 + (weekend ? 1 : 0)) * 12 + (slot % 12);
	if (key != s_rowKey) {
		if (!readAt(s_hdr.popOffset + key * sizeof(ds::PopRow), &s_row, sizeof s_row)) return false;
		s_rowKey = key;
	}
	out = s_row;
	return true;
}

const ds::PedZone* zoneAt(float x, float y) {
	const ds::PedZone* best = nullptr;
	int32_t bestArea = 0x7FFFFFFF;
	// 1/16 m integers: four soft-float compares per zone over ~380 zones made
	// every population / traffic spawn attempt cost ~0.7 ms
	const int32_t ix = (int32_t)(x * 16.0f), iy = (int32_t)(y * 16.0f);
	for (int i = 0; s_zones && i < s_hdr.numZones; i++) {
		const ds::PedZone& z = s_zones[i];
		if (ix < z.x1 * 16 || ix > z.x2 * 16 || iy < z.y1 * 16 || iy > z.y2 * 16) continue;
		const int32_t a = (int32_t)(z.x2 - z.x1) * (z.y2 - z.y1);
		if (a < bestArea) { bestArea = a; best = &z; }
	}
	return best;
}

int numNodes() { return ready() ? (int)s_hdr.numNodes : 0; }
static const ds::PathNode* pathNode(int kind, int i) {
	if (!ready() || i < 0 || (uint32_t)i >= (kind ? s_hdr.numRoadNodes : s_hdr.numNodes)) return nullptr;
	for (const PathChunk& c : s_paths[kind])
		if (c.used && i >= c.cell.first && i - c.cell.first < c.cell.count) return &c.nodes[i - c.cell.first];
	return nullptr;
}
const ds::PathNode* node(int i) { return pathNode(0, i); }
static int pathLink(int kind, const ds::PathNode* n, int k) {
	if (!n || k < 0 || k >= n->numLinks) return -1;
	const uint32_t index = n->firstLink + k;
	for (const PathChunk& c : s_paths[kind])
		if (c.used && index >= c.linkFirst && index - c.linkFirst < c.linkCount)
			return c.links[index - c.linkFirst];
	return -1;
}
int link(const ds::PathNode* n, int k) { return pathLink(0, n, k); }
int numRoadNodes() { return ready() ? (int)s_hdr.numRoadNodes : 0; }
const ds::PathNode* roadNode(int i) { return pathNode(1, i); }
int roadLink(const ds::PathNode* n, int k) { return pathLink(1, n, k); }
int carGroupSize(int g) { return s_carGroups && (unsigned)g < 29 ? s_carGroups[g].count : 0; }
int carGroupMember(int g, int k) { return (unsigned)k < (unsigned)carGroupSize(g) ? s_carMembers[s_carGroups[g].first + k] : -1; }

bool cellRange(float x, float y, float r, int& cx0, int& cy0, int& cx1, int& cy1) {
	if (!ready()) return false;
	const float cs = (float)s_hdr.cellSize;
	cx0 = (int)((x - r - s_hdr.gridX0) / cs);
	cy0 = (int)((y - r - s_hdr.gridY0) / cs);
	cx1 = (int)((x + r - s_hdr.gridX0) / cs);
	cy1 = (int)((y + r - s_hdr.gridY0) / cs);
	if (cx1 < 0 || cy1 < 0 || cx0 >= s_hdr.gridW || cy0 >= s_hdr.gridH) return false;
	if (cx0 < 0) cx0 = 0;
	if (cy0 < 0) cy0 = 0;
	if (cx1 >= s_hdr.gridW) cx1 = s_hdr.gridW - 1;
	if (cy1 >= s_hdr.gridH) cy1 = s_hdr.gridH - 1;
	return true;
}

const ds::PathCell* cell(int cx, int cy) {
	const PathChunk* c = chunk(0, cx, cy);
	return c ? &c->cell : nullptr;
}
const ds::PathCell* roadCell(int cx, int cy) {
	const PathChunk* c = chunk(1, cx, cy);
	return c ? &c->cell : nullptr;
}

int nearestNode(float x, float y, float z, float range) {
	int best = -1;
	float bestD = range * range;
	forNodesNear(x, y, range, [&](int i) {
		const ds::PathNode* n = node(i);
		if (!n) return true;
		const float dx = nodeX(n) - x, dy = nodeY(n) - y, dz = (nodeZ(n) - z) * 3.0f;   // other floors count more
		const float d = dx * dx + dy * dy + dz * dz;
		if (d < bestD) { bestD = d; best = i; }
		return true;
	});
	return best;
}

}
