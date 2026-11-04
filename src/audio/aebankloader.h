#pragma once

#include <stdint.h>

#include "audio/aehardware.h"

// CAEBankLoader / CAEMP3BankLoader, per sound instead of per bank slot.
// The PC loads whole banks into 45 fixed slots; the DS has 4 MB, so every
// sound is its own cache entry: (global bank id, sound index) -> DSND blob in
// STREAM (tag audio). Requests are queued and read by service() under a
// per-frame time budget (EnsureSoundBankIsLoaded returning false = not yet);
// unused entries are evicted LRU over budget::AUDIO_CACHE_BYTES. Entries
// referenced by a playing channel or pinned are never evicted.
// banks.dsbl (bank -> pak + index) and the pak bank tables stay resident
// (PERM), each pak file stays open; bank sound tables are a small LRU.
namespace aebank {

struct Stats {
	uint32_t bytes;          // resident sample bytes
	uint32_t tableBytes;     // bank offset tables
	uint16_t resident, pinned, queued, failed;
	uint32_t loads, evictions, oom;
	uint32_t serviceUs;      // last service()
};

void init();                                   // banks.dsbl + pak headers
bool available();                              // banks.dsbl was found
int numSounds(uint16_t bank);                  // -1 = bank not in the build
// Handle (>= 0) of a resident sound, or -1. A miss queues a load when `queue`.
int find(uint16_t bank, uint16_t id, bool queue = true);
const aehw::Sample* sample(int h);
void addRef(int h);                            // a channel plays it: not evictable
void release(int h);
void pin(uint16_t bank, uint16_t id);          // resident for good (queues the load)
void unpin(uint16_t bank, uint16_t id);
void requestBank(uint16_t bank, bool pinned);  // every sound of a bank
bool bankReady(uint16_t bank);                 // every sound resident (or failed)
bool loadNow(uint16_t bank, uint16_t id, bool pinned);   // synchronous read
void drop(uint16_t bank, uint16_t id);         // free at once if nothing plays it
void service(uint32_t budgetUs);               // queued reads, at least one per call
bool idle();                                   // nothing queued
const Stats& stats();

}
