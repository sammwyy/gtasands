#pragma once

#include <stdint.h>

#include "assets/ds_formats.h"

// CModelInfo (vehicle part) + cHandlingDataMgr + CVehicleModelInfo colours +
// SurfaceInfos_c adhesion, all from data/vehicles.dveh (tools/gtasa/vehicles.py,
// handling already in game units). Only the header, the carcols table, the
// surface table and a 4-byte index per vehicle stay resident (~2.5 KB); a
// VehInfo record (228 B) is read when a vehicle model is requested.
namespace vehdata {

// Loading-screen job: opens the file and reads the resident tables.
bool initStep();
bool ready();
int count();
// Index entry: model id + eVehicleType, for pickers / lookups without a read.
int modelId(int i);
uint8_t type(int i);
int findModel(int modelId);              // -1 if not a vehicle
// Reads record i (one small cached read). False if out of range / no file.
bool read(int i, ds::VehInfo& out);
// Every IDE vehicle type spawns; trailers and the fake aircraft (f_heli /
// f_plane) cannot be entered.
bool spawnable(int i);
bool enterable(int i);
// The open file's header / a raw read (waterlevel / train tracks tables).
const ds::VehHeader* header();
bool readAt(uint32_t offset, void* out, uint32_t size);

// carcols.dat colour table (RGB)
void colour(int index, uint8_t& r, uint8_t& g, uint8_t& b);
// SurfaceInfos_c::GetAdhesiveLimit for a tyre (RUBBER, CColPoint::m_nSurfaceTypeA =
// SURFACE_WHEELBASE) on COL surface `surface`, and GetWetMultiplier.
float tyreAdhesion(uint8_t surface);
float wetMultiplier(uint8_t surface, float wetRoads);
uint8_t skidmark(uint8_t surface);       // 0 default, 1 sandy, 2 muddy
// the same, split for the fixed-point physics (per-group tables built once)
int surfaceGroup(uint8_t surface);
int wetGrip(uint8_t surface);            // percent
float groupAdhesion(int group);

}
