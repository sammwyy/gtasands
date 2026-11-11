#include "audio/aebankloader.h"

#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/timing.h"

namespace aebank {

using mem::budget::AUDIO_CACHE_BYTES;
using mem::budget::AUDIO_CACHE_ENTRIES;

static constexpr int MAX_PAKS = 16;
static constexpr int TABLE_SLOTS = 16;

struct Pak {
	char name[16];
	FILE* f;
	ds::SfxPakBank* banks;   // PERM, read once
	uint16_t numBanks;
	bool tried;
};

// One bank's sound offset table (BankHeader + u32 offsets, relative to the bank).
struct Table {
	uint16_t bank, n;
	uint32_t base;           // bank offset in the pak
	uint32_t* offs;
	uint32_t lastUse;
};

enum State : uint8_t { E_FREE, E_QUEUED, E_READY, E_FAILED };

struct Entry {
	uint16_t bank, id;
	State state;
	uint8_t refs;
	bool pinned;
	uint32_t lastUse;
	void* blob;
	aehw::Sample s;
};

static ds::BankLookup* s_lookup;
static uint16_t s_numBanks;
static Pak s_paks[MAX_PAKS];
static int s_numPaks;
static Table s_tables[TABLE_SLOTS];
static Entry s_e[AUDIO_CACHE_ENTRIES];
static uint32_t s_clock;
static Stats s_stats;

void init() {
	if (s_lookup) return;
	FILE* f = fopen("game:/audio/sfx/banks.dsbl", "rb");
	if (!f) {
		hud::log("\x1b[33maudio: banks.dsbl missing\x1b[39m");
		return;
	}
	ds::BankLookupHeader h;
	if (fread(&h, sizeof h, 1, f) == 1 && h.magic == DS_FOURCC('D', 'S', 'B', 'L')) {
		s_numPaks = h.numPaks < MAX_PAKS ? h.numPaks : MAX_PAKS;
		for (int i = 0; i < h.numPaks; i++) {
			char nm[16];
			if (fread(nm, 16, 1, f) != 1) break;
			if (i < MAX_PAKS) {
				memcpy(s_paks[i].name, nm, 16);
				s_paks[i].name[15] = 0;
			}
		}
		s_lookup = (ds::BankLookup*)mem::perm(h.numBanks * sizeof(ds::BankLookup), mem::TAG_AUDIO);
		if (s_lookup && fread(s_lookup, sizeof(ds::BankLookup), h.numBanks, f) == h.numBanks) s_numBanks = h.numBanks;
	}
	fclose(f);
	hud::log("audio: %u banks in %d paks", (unsigned)s_numBanks, s_numPaks);
}

bool available() { return s_numBanks > 0; }

static Pak* pakFor(uint16_t bank, uint16_t& index) {
	if (bank >= s_numBanks || s_lookup[bank].pak >= s_numPaks) return nullptr;
	Pak& p = s_paks[s_lookup[bank].pak];
	index = s_lookup[bank].index;
	if (!p.tried) {
		p.tried = true;
		char path[64];
		char lower[16];
		for (int i = 0; i < 16; i++) lower[i] = (char)(p.name[i] >= 'A' && p.name[i] <= 'Z' ? p.name[i] + 32 : p.name[i]);
	snprintf(path, sizeof path, "game:/audio/sfx/%s.dsfx", lower);
		p.f = fopen(path, "rb");
		ds::SfxPakHeader ph;
		if (p.f && fread(&ph, sizeof ph, 1, p.f) == 1 && ph.magic == DS_FOURCC('D', 'S', 'F', 'X')) {
			p.banks = (ds::SfxPakBank*)mem::perm(ph.numBanks * sizeof(ds::SfxPakBank), mem::TAG_AUDIO);
			if (p.banks && fread(p.banks, sizeof(ds::SfxPakBank), ph.numBanks, p.f) == ph.numBanks) p.numBanks = ph.numBanks;
		}
		if (!p.numBanks) {
			hud::log("\x1b[33maudio: %s unreadable\x1b[39m", path);
			if (p.f) fclose(p.f);
			p.f = nullptr;
		}
	}
	return p.f && index < p.numBanks ? &p : nullptr;
}

static Table* table(uint16_t bank) {
	for (Table& t : s_tables)
		if (t.offs && t.bank == bank) {
			t.lastUse = ++s_clock;
			return &t;
		}
	uint16_t idx;
	Pak* p = pakFor(bank, idx);
	if (!p) return nullptr;
	const uint32_t base = p->banks[idx].offset;
	ds::BankHeader bh;
	if (fseek(p->f, base, SEEK_SET) != 0 || fread(&bh, sizeof bh, 1, p->f) != 1 || bh.magic != DS_FOURCC('D', 'S', 'B', 'K'))
		return nullptr;
	Table* t = &s_tables[0];
	for (Table& c : s_tables) {
		if (!c.offs) { t = &c; break; }
		if (c.lastUse < t->lastUse) t = &c;
	}
	if (t->offs) {
		s_stats.tableBytes -= t->n * 4u;
		mem::free(t->offs);
		t->offs = nullptr;
	}
	t->offs = (uint32_t*)mem::alloc(bh.numSounds * 4u + 4, mem::TAG_AUDIO);
	if (!t->offs) return nullptr;
	if (fread(t->offs, 4, bh.numSounds, p->f) != bh.numSounds) {
		mem::free(t->offs);
		t->offs = nullptr;
		return nullptr;
	}
	t->bank = bank;
	t->n = bh.numSounds;
	t->base = base;
	t->lastUse = ++s_clock;
	s_stats.tableBytes += t->n * 4u;
	return t;
}

int numSounds(uint16_t bank) {
	const Table* t = table(bank);
	return t ? t->n : -1;
}

// Direct-mapped memo in front of the linear scan: the entities ask for the
// same few sounds every frame (EnsureSoundBankIsLoaded), 128 compares each.
static constexpr int MEMO = 64;
static int16_t s_memo[MEMO];

static int memoSlot(uint16_t bank, uint16_t id) { return (bank * 31 + id) & (MEMO - 1); }

static Entry* lookup(uint16_t bank, uint16_t id) {
	const int m = memoSlot(bank, id);
	const int h = s_memo[m] - 1;
	if (h >= 0 && s_e[h].state != E_FREE && s_e[h].bank == bank && s_e[h].id == id) return &s_e[h];
	for (Entry& e : s_e)
		if (e.state != E_FREE && e.bank == bank && e.id == id) {
			s_memo[m] = (int16_t)(&e - s_e + 1);
			return &e;
		}
	return nullptr;
}

static void freeEntry(Entry& e) {
	if (e.state == E_READY) {
		s_stats.bytes -= e.s.bytes;
		s_stats.resident--;
	}
	if (e.state == E_QUEUED) s_stats.queued--;
	if (e.state == E_FAILED) s_stats.failed--;
	if (e.pinned) s_stats.pinned--;
	mem::free(e.blob);
	e = {};
}

// LRU over evictable entries (ready, unpinned, nothing playing them).
static bool evictOne() {
	Entry* v = nullptr;
	for (Entry& e : s_e)
		if (e.state == E_READY && !e.pinned && !e.refs && (!v || e.lastUse < v->lastUse)) v = &e;
	if (!v) return false;
	freeEntry(*v);
	s_stats.evictions++;
	return true;
}

static Entry* newEntry(uint16_t bank, uint16_t id) {
	Entry* slot = nullptr;
	for (Entry& e : s_e)
		if (e.state == E_FREE) { slot = &e; break; }
	if (!slot) {
		// table full: recycle the oldest failed entry, else evict
		for (Entry& e : s_e)
			if (e.state == E_FAILED && (!slot || e.lastUse < slot->lastUse)) slot = &e;
		if (slot) freeEntry(*slot);
		else if (evictOne()) return newEntry(bank, id);
		else return nullptr;
	}
	slot->bank = bank;
	slot->id = id;
	slot->state = E_QUEUED;
	s_memo[memoSlot(bank, id)] = (int16_t)(slot - s_e + 1);
	slot->lastUse = ++s_clock;
	s_stats.queued++;
	return slot;
}

static void read(Entry& e) {
	s_stats.queued--;
	e.state = E_FAILED;
	s_stats.failed++;
	const Table* t = table(e.bank);
	uint16_t idx;
	Pak* p = t ? pakFor(e.bank, idx) : nullptr;
	if (!p || e.id >= t->n) return;
	ds::SoundHeader h;
	if (fseek(p->f, t->base + t->offs[e.id], SEEK_SET) != 0 || fread(&h, sizeof h, 1, p->f) != 1 ||
	    h.magic != DS_FOURCC('D', 'S', 'N', 'D'))
		return;
	const uint32_t bytes = (h.dataBytes + 3) & ~3u;
	while (s_stats.bytes + bytes > AUDIO_CACHE_BYTES && evictOne()) {}
	void* blob = mem::alloc(bytes, mem::TAG_AUDIO);
	while (!blob && evictOne()) blob = mem::alloc(bytes, mem::TAG_AUDIO);
	if (!blob) {
		s_stats.oom++;
		return;
	}
	if (fread(blob, 1, bytes, p->f) != bytes) {
		mem::free(blob);
		return;
	}
	DC_FlushRange(blob, bytes);   // the sound DMA reads main RAM, not the D-cache
	e.blob = blob;
	e.s = { blob, bytes, h.numSamples, h.loopStart, h.sampleRate, h.headroom, h.format };
	e.state = E_READY;
	s_stats.failed--;
	s_stats.resident++;
	s_stats.bytes += bytes;
	s_stats.loads++;
}

int find(uint16_t bank, uint16_t id, bool queue) {
	Entry* e = lookup(bank, id);
	if (!e && queue && s_numBanks) e = newEntry(bank, id);
	if (!e || e->state != E_READY) return -1;
	e->lastUse = ++s_clock;
	return (int)(e - s_e);
}

const aehw::Sample* sample(int h) {
	return h >= 0 && h < AUDIO_CACHE_ENTRIES && s_e[h].state == E_READY ? &s_e[h].s : nullptr;
}

void addRef(int h) {
	if (h >= 0 && h < AUDIO_CACHE_ENTRIES && s_e[h].refs < 255) s_e[h].refs++;
}

void release(int h) {
	if (h >= 0 && h < AUDIO_CACHE_ENTRIES && s_e[h].refs) s_e[h].refs--;
}

void pin(uint16_t bank, uint16_t id) {
	Entry* e = lookup(bank, id);
	if (!e && s_numBanks) e = newEntry(bank, id);
	if (e && !e->pinned) {
		e->pinned = true;
		s_stats.pinned++;
	}
}

void unpin(uint16_t bank, uint16_t id) {
	Entry* e = lookup(bank, id);
	if (e && e->pinned) {
		e->pinned = false;
		s_stats.pinned--;
	}
}

void requestBank(uint16_t bank, bool pinned) {
	const int n = numSounds(bank);
	for (int i = 0; i < n; i++) {
		if (pinned) pin(bank, (uint16_t)i);
		else find(bank, (uint16_t)i, true);
	}
}

bool bankReady(uint16_t bank) {
	const int n = numSounds(bank);
	if (n < 0) return true;   // not in the build: nothing to wait for
	for (int i = 0; i < n; i++) {
		const Entry* e = lookup(bank, (uint16_t)i);
		if (!e || e->state == E_QUEUED) return false;
	}
	return true;
}

bool loadNow(uint16_t bank, uint16_t id, bool pinned) {
	if (pinned) pin(bank, id);
	Entry* e = lookup(bank, id);
	if (!e && s_numBanks) e = newEntry(bank, id);
	if (e && e->state == E_QUEUED) read(*e);
	return e && e->state == E_READY;
}

void drop(uint16_t bank, uint16_t id) {
	Entry* e = lookup(bank, id);
	if (e && !e->refs) freeEntry(*e);
	else if (e) unpin(bank, id);
}

void service(uint32_t budgetUs) {
	const u32 t0 = timing::ticks();
	while (s_stats.queued) {
		// oldest request first
		Entry* next = nullptr;
		for (Entry& e : s_e)
			if (e.state == E_QUEUED && (!next || e.lastUse < next->lastUse)) next = &e;
		if (!next) break;
		read(*next);
		if (timing::usSince(t0) >= budgetUs) break;
	}
	s_stats.serviceUs = timing::usSince(t0);
}

bool idle() { return s_stats.queued == 0; }
const Stats& stats() { return s_stats; }

}
