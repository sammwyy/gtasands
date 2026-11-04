#include "game/vehicledata.h"

#include <stdio.h>
#include <string.h>

#include "hud/hud.h"
#include "mem/mem.h"

namespace vehdata {

struct Index { uint16_t modelId; uint8_t type, pad; };

static FILE* s_file;
static ds::VehHeader s_hdr;
static Index* s_index;
static uint8_t (*s_colours)[4];
static ds::VehSurface* s_surf;
static int s_cached = -1;
static ds::VehInfo s_cache;
static bool s_tried;

bool ready() { return s_index != nullptr; }
int count() { return s_index ? s_hdr.numVehicles : 0; }
int modelId(int i) { return (unsigned)i < (unsigned)count() ? s_index[i].modelId : -1; }
uint8_t type(int i) { return (unsigned)i < (unsigned)count() ? s_index[i].type : (uint8_t)ds::VEH_UNKNOWN; }

int findModel(int id) {
	for (int i = 0; i < count(); i++)
		if (s_index[i].modelId == id) return i;
	return -1;
}

bool drivable(int i) {
	const uint8_t t = type(i);
	return t == ds::VEH_CAR || t == ds::VEH_MTRUCK;
}

bool read(int i, ds::VehInfo& out) {
	if (!s_file || (unsigned)i >= (unsigned)count()) return false;
	if (i != s_cached) {
		if (fseek(s_file, (long)(s_hdr.vehOffset + i * sizeof(ds::VehInfo)), SEEK_SET) ||
		    fread(&s_cache, sizeof s_cache, 1, s_file) != 1)
			return false;
		s_cached = i;
	}
	out = s_cache;
	return true;
}

void colour(int index, uint8_t& r, uint8_t& g, uint8_t& b) {
	if (!s_colours || (unsigned)index >= s_hdr.numColours) index = 1;
	if (!s_colours) { r = g = b = 200; return; }
	r = s_colours[index][0];
	g = s_colours[index][1];
	b = s_colours[index][2];
}

// SURFACE_WHEELBASE is in the RUBBER adhesion group (0)
float tyreAdhesion(uint8_t surface) {
	const int g = s_surf && surface < s_hdr.numSurfaces ? s_surf[surface].adhesionGroup : 2;
	return s_hdr.adhesive[g < 6 ? g : 2][0];
}

float wetMultiplier(uint8_t surface, float wetRoads) {
	const int w = s_surf && surface < s_hdr.numSurfaces ? s_surf[surface].wetGrip : 0;
	return (float)w * 0.01f * wetRoads + 1.0f;
}

uint8_t skidmark(uint8_t surface) {
	return s_surf && surface < s_hdr.numSurfaces ? s_surf[surface].skidmark : 0;
}

bool initStep() {
	if (s_tried) return true;
	s_tried = true;
	s_file = fopen("nitro:/data/vehicles.dveh", "rb");
	if (!s_file || fread(&s_hdr, sizeof s_hdr, 1, s_file) != 1 || s_hdr.magic != DS_FOURCC('D', 'V', 'E', 'H') ||
	    s_hdr.version != 1 || !s_hdr.numVehicles) {
		hud::log("\x1b[33mvehicles.dveh missing: no vehicles\x1b[39m");
		if (s_file) fclose(s_file);
		s_file = nullptr;
		return true;
	}
	s_index = (Index*)mem::alloc(sizeof(Index) * s_hdr.numVehicles, mem::TAG_WORLD);
	s_colours = (uint8_t(*)[4])mem::alloc(4 * (s_hdr.numColours ? s_hdr.numColours : 1), mem::TAG_WORLD);
	s_surf = (ds::VehSurface*)mem::alloc(sizeof(ds::VehSurface) * (s_hdr.numSurfaces ? s_hdr.numSurfaces : 1), mem::TAG_WORLD);
	bool ok = s_index && s_colours && s_surf;
	ok = ok && !fseek(s_file, (long)s_hdr.colourOffset, SEEK_SET) &&
	     fread(s_colours, 4, s_hdr.numColours, s_file) == s_hdr.numColours;
	ok = ok && !fseek(s_file, (long)s_hdr.surfaceOffset, SEEK_SET) &&
	     fread(s_surf, sizeof(ds::VehSurface), s_hdr.numSurfaces, s_file) == s_hdr.numSurfaces;
	if (ok) {   // one sequential read of the records, only the index is kept
		mem::Scratch scratch;
		ds::VehInfo* v = (ds::VehInfo*)scratch.alloc(sizeof(ds::VehInfo) * s_hdr.numVehicles);
		ok = v && !fseek(s_file, (long)s_hdr.vehOffset, SEEK_SET) &&
		     fread(v, sizeof(ds::VehInfo), s_hdr.numVehicles, s_file) == s_hdr.numVehicles;
		for (int i = 0; ok && i < s_hdr.numVehicles; i++) s_index[i] = { v[i].modelId, v[i].type, 0 };
	}
	if (!ok) {
		mem::free(s_index);
		mem::free(s_colours);
		mem::free(s_surf);
		s_index = nullptr;
		s_colours = nullptr;
		s_surf = nullptr;
		hud::log("\x1b[31mvehicles.dveh read failed\x1b[39m");
		return true;
	}
	hud::log("vehicles: %u models, %u colours", s_hdr.numVehicles, s_hdr.numColours);
	return true;
}

}
