#pragma once

#include <stdint.h>

#include "audio/aesoundmanager.h"

// CAETwinLoopSoundEntity (Audio/Entities/AETwinLoopSoundEntity.cpp): two
// loops of the same material run together, only one audible; every
// swapTimeMin..Max ms the volume jumps to the other so the loop never
// sounds repetitive (rain, later engines / skids).
struct TwinLoop {
	aesound::Sound* sounds[2];
	uint16_t bank, ids[2];
	uint16_t swapMin, swapMax;
	uint32_t swapAtMs;
	bool first, inUse;

	void init(uint16_t bank, uint16_t soundA, uint16_t soundB, uint16_t swapMinMs, uint16_t swapMaxMs);
	// PlayTwinLoopSound: both loops start at random phases
	void play(float x, float y, float z, float volume, float speed, uint16_t flags);
	void update(float x, float y, float z, float volume, float speed);   // UpdateTwinLoopSound
	bool switchesThisFrame() const;                                       // DoSoundsSwitchThisFrame
	bool active() const { return inUse && (sounds[0] || sounds[1]); }
	void stop();                                                          // StopSoundAndForget
};
