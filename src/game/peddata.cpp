#include "game/peddata.h"

#include <stdio.h>
#include <string.h>

#include "hud/hud.h"
#include "mem/mem.h"

namespace peddata {

static FILE* s_file;
static ds::PedHeader s_hdr;
static ds::PedModel* s_models;
static ds::PedStat* s_stats;
static ds::PedAnimGroup* s_anims;
static ds::PedGroup* s_groups;
static uint16_t* s_members;
static ds::PedZone* s_zones;
static ds::PathCell* s_cells;
static ds::PathNode* s_nodes;
static uint16_t* s_links;
static int s_step;
static Stats s_st;
static int s_rowKey = -1;
static ds::PopRow s_row;

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

bool ready() { return s_nodes != nullptr; }

bool initStep() {
	if (s_nodes || s_step >= 99) return true;   // a second new game: the tables are still there
	bool ok = true;
	switch (s_step++) {
	case 0:
		s_file = fopen("nitro:/data/peds.dped", "rb");
		if (!s_file || fread(&s_hdr, sizeof s_hdr, 1, s_file) != 1 || s_hdr.magic != DS_FOURCC('D', 'P', 'E', 'D') ||
		    s_hdr.version != 1) {
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
	case 3: ok = table(s_zones, s_hdr.zoneOffset, s_hdr.numZones) &&
	             table(s_cells, s_hdr.cellOffset, (uint32_t)s_hdr.gridW * s_hdr.gridH); break;
	case 4: ok = table(s_links, s_hdr.linkOffset, s_hdr.numLinks) && table(s_nodes, s_hdr.nodeOffset, s_hdr.numNodes); break;
	default: return true;
	}
	if (!ok) {
		hud::log("\x1b[31mpeds.dped: read failed\x1b[39m");
		mem::free(s_nodes);
		s_nodes = nullptr;
		s_step = 99;
		return true;
	}
	if (s_step == 5) {
		s_st.models = s_hdr.numModels;
		s_st.nodes = (uint16_t)s_hdr.numNodes;
		s_st.zones = s_hdr.numZones;
		hud::log("peds: %u models %u nodes %u zones %luK", s_st.models, s_st.nodes, s_st.zones,
		         (unsigned long)(s_st.bytes >> 10));
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
	for (int i = 0; s_zones && i < s_hdr.numZones; i++) {
		const ds::PedZone& z = s_zones[i];
		if (x < z.x1 || x > z.x2 || y < z.y1 || y > z.y2) continue;
		const int32_t a = (int32_t)(z.x2 - z.x1) * (z.y2 - z.y1);
		if (a < bestArea) { bestArea = a; best = &z; }
	}
	return best;
}

int numNodes() { return s_nodes ? (int)s_hdr.numNodes : 0; }
const ds::PathNode* node(int i) { return (unsigned)i < (unsigned)numNodes() ? &s_nodes[i] : nullptr; }
int link(const ds::PathNode* n, int k) { return k < n->numLinks ? s_links[n->firstLink + k] : -1; }

bool cellRange(float x, float y, float r, int& cx0, int& cy0, int& cx1, int& cy1) {
	if (!s_nodes) return false;
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

const ds::PathCell* cell(int cx, int cy) { return &s_cells[cy * s_hdr.gridW + cx]; }

int nearestNode(float x, float y, float z, float range) {
	int best = -1;
	float bestD = range * range;
	forNodesNear(x, y, range, [&](int i) {
		const ds::PathNode& n = s_nodes[i];
		const float dx = nodeX(&n) - x, dy = nodeY(&n) - y, dz = (nodeZ(&n) - z) * 3.0f;   // other floors count more
		const float d = dx * dx + dy * dy + dz * dz;
		if (d < bestD) { bestD = d; best = i; }
		return true;
	});
	return best;
}

}
