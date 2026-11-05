#pragma once

#include <stdint.h>

#include "assets/ds_formats.h"

// The ped half of CModelInfo + CPedStats + CPopulation's ped groups + CPopCycle's
// table + CTheZones' zone info + CPathFind's ped nodes, from data/peds.dped
// (tools/gtasa/peds.py). Resident: the 12 B model records, stats, walkcycle
// anim groups, ped groups, zones and the ped path graph inside the map bounds
// (~35 KB). Model names and popcycle rows are read on demand.
namespace peddata {

// ePedType (the values the population code tests)
enum PedType : uint8_t {
	PT_PLAYER1 = 0, PT_CIVMALE = 4, PT_CIVFEMALE = 5, PT_COP = 6,
	PT_GANG1 = 7, PT_GANG2 = 8, PT_GANG3 = 9, PT_GANG10 = 16,
	PT_DEALER = 17, PT_MEDIC = 18, PT_FIREMAN = 19, PT_CRIMINAL = 20, PT_BUM = 21, PT_PROSTITUTE = 22,
};
inline bool isGang(int t) { return t >= PT_GANG1 && t <= PT_GANG10; }
// ePopcycleGroup ids used directly
enum : int { GROUP_BALLAS = 42, GROUP_DEALERS = 52, NUM_PERC_GROUPS = 18 };

// Loading-screen job (one read per table).
bool initStep();
bool ready();

int numModels();
const ds::PedModel* model(int i);
int findModel(int modelId);            // index or -1
bool name(int i, ds::PedName& out);    // one 48-byte read
const ds::PedStat* stat(int i);        // never null once ready (falls back to 0)
const ds::PedAnimGroup* animGroup(int i);
// CPopulation::m_PedGroups: model indices of ePopcyclePedGroup g.
int groupSize(int g);
int groupMember(int g, int k);
// CPopulation::m_TranslationArray: ePopcycleGroup (0..17 others, 18.. gangs) and
// world zone (0 LA, 1 SF, 2 VG) -> ePopcyclePedGroup.
int pedGroupOf(int popGroup, int worldZone);
// CPopCycle::m_nMaxNumPeds & co for [zone type][weekend][2 h slot].
bool popRow(int zoneType, int weekend, int slot, ds::PopRow& out);
// CTheZones::GetZoneInfo: the smallest zone around (x, y), or null.
const ds::PedZone* zoneAt(float x, float y);

// Ped path graph (local indices). Positions in metres.
int numNodes();
const ds::PathNode* node(int i);
inline float nodeX(const ds::PathNode* n) { return n->x * 0.125f; }
inline float nodeY(const ds::PathNode* n) { return n->y * 0.125f; }
inline float nodeZ(const ds::PathNode* n) { return n->z * 0.125f; }
int link(const ds::PathNode* n, int k);   // node index of the k-th link
// CPathFind::FindNodeClosestToCoors over the ped nodes within `range` m (-1 if none).
int nearestNode(float x, float y, float z, float range);
// Calls fn(index) for every node in the grid cells overlapping the square
// [x - r, x + r] x [y - r, y + r]. Stops when fn returns false.
template <typename F> void forNodesNear(float x, float y, float r, F fn);
// internal for forNodesNear
bool cellRange(float x, float y, float r, int& cx0, int& cy0, int& cx1, int& cy1);
const ds::PathCell* cell(int cx, int cy);

struct Stats { uint16_t models, nodes, zones; uint32_t bytes; };
const Stats& stats();

template <typename F> void forNodesNear(float x, float y, float r, F fn) {
	int cx0, cy0, cx1, cy1;
	if (!cellRange(x, y, r, cx0, cy0, cx1, cy1)) return;
	for (int cy = cy0; cy <= cy1; cy++)
		for (int cx = cx0; cx <= cx1; cx++) {
			const ds::PathCell* c = cell(cx, cy);
			for (int i = c->first; i < c->first + c->count; i++)
				if (!fn(i)) return;
		}
}

}
