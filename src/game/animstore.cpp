#include "game/animstore.h"

#include <stdio.h>
#include <string.h>

#include "assets/pack.h"
#include "hud/hud.h"
#include "mem/pool.h"

namespace animstore {

struct DirRef { uint32_t hash, offset, size; };

struct Block {
	FILE* file;          // loose file (owned) or nullptr
	const Pack* pack;    // pack-backed block
	uint32_t base;       // DANM start inside file/pack
	uint32_t count;
	uint32_t blockHash;  // fnv1a(DANM block name)
	DirRef* dir;         // sorted by hash
};

struct Clip {
	uint32_t hash;
	uint32_t size;
	uint32_t lastUse;
	uint16_t refs;
	uint8_t block;
	uint8_t pinned;
	ds::AnimEntry* data;
};

static Block s_blocks[mem::budget::MAX_ANIM_BLOCKS];
static int s_numBlocks;
static mem::Pool<Clip> s_clips;
static uint32_t s_bytes, s_clock;

void init() {
	if (s_clips.capacity()) return;
	s_clips.init(mem::budget::MAX_ANIM_CLIPS, mem::TAG_ANIM);
}

static bool readAt(const Block& b, uint32_t off, void* dst, uint32_t size) {
	if (b.pack) return b.pack->read(b.base + off, dst, size);
	return fseek(b.file, (long)(b.base + off), SEEK_SET) == 0 && fread(dst, 1, size, b.file) == size;
}

static int addBlock(Block& b) {
	ds::AnimHeader h;
	if (!readAt(b, 0, &h, sizeof h) || h.magic != DS_FOURCC('D', 'A', 'N', 'M') || h.version != 2) {
		hud::log("\x1b[31manim block: bad DANM (rebuild assets)\x1b[39m");
		return NO_BLOCK;
	}
	b.count = h.numAnims;
	b.blockHash = ds::fnv1a(h.block);
	b.dir = (DirRef*)mem::alloc(b.count * sizeof(DirRef), mem::TAG_ANIM);
	if (!b.dir) return NO_BLOCK;
	{
		mem::Scratch scratch;
		ds::AnimDirEntry* raw = (ds::AnimDirEntry*)scratch.alloc(b.count * sizeof(ds::AnimDirEntry));
		if (!raw || !readAt(b, sizeof h, raw, b.count * sizeof(ds::AnimDirEntry))) {
			mem::free(b.dir);
			return NO_BLOCK;
		}
		for (uint32_t i = 0; i < b.count; i++) {
			char name[25];
			memcpy(name, raw[i].name, 24);
			name[24] = 0;
			b.dir[i] = { ds::fnv1a(name), raw[i].offset, raw[i].size };
		}
	}
	// insertion sort: directories are a few hundred entries, once per load
	for (uint32_t i = 1; i < b.count; i++) {
		const DirRef v = b.dir[i];
		uint32_t j = i;
		for (; j > 0 && b.dir[j - 1].hash > v.hash; j--) b.dir[j] = b.dir[j - 1];
		b.dir[j] = v;
	}
	s_blocks[s_numBlocks] = b;
	return s_numBlocks++;
}

int openFile(const char* path) {
	init();
	if (s_numBlocks >= mem::budget::MAX_ANIM_BLOCKS) return NO_BLOCK;
	Block b = {};
	b.file = fopen(path, "rb");
	if (!b.file) return NO_BLOCK;
	const int id = addBlock(b);
	if (id == NO_BLOCK) fclose(b.file);
	return id;
}

int openInPack(const Pack& pack, const char* entry) {
	init();
	if (s_numBlocks >= mem::budget::MAX_ANIM_BLOCKS) return NO_BLOCK;
	ds::PackEntry e;
	if (!pack.find(entry, e)) return NO_BLOCK;
	Block b = {};
	b.pack = &pack;
	b.base = e.offset;
	return addBlock(b);
}

int findBlock(const char* name) {
	const uint32_t h = ds::fnv1a(name);
	for (int i = 0; i < s_numBlocks; i++)
		if (s_blocks[i].blockHash == h) return i;
	return NO_BLOCK;
}

static const DirRef* lookup(int block, uint32_t h) {
	if ((unsigned)block >= (unsigned)s_numBlocks) return nullptr;
	const Block& b = s_blocks[block];
	uint32_t lo = 0, hi = b.count;
	while (lo < hi) {
		const uint32_t mid = (lo + hi) / 2;
		if (b.dir[mid].hash < h) lo = mid + 1;
		else hi = mid;
	}
	return lo < b.count && b.dir[lo].hash == h ? &b.dir[lo] : nullptr;
}

bool hasClip(int block, const char* name) { return name && lookup(block, ds::fnv1a(name)); }

static void evict(Clip& c) {
	s_bytes -= c.size;
	mem::free(c.data);
	s_clips.destroy(&c);
}

// Least-recently-used unreferenced clip, or nullptr.
static Clip* lruVictim() {
	Clip* best = nullptr;
	s_clips.forEach([&](Clip& c) {
		if (!c.refs && !c.pinned && (!best || c.lastUse < best->lastUse)) best = &c;
	});
	return best;
}

void trim(uint32_t budget) {
	while (s_bytes > budget) {
		Clip* v = lruVictim();
		if (!v) break;
		evict(*v);
	}
}

Clip* acquire(int block, const char* name) {
	if (!name || (unsigned)block >= (unsigned)s_numBlocks) return nullptr;
	const uint32_t h = ds::fnv1a(name);
	Clip* hit = nullptr;
	s_clips.forEach([&](Clip& c) {
		if (c.block == block && c.hash == h) hit = &c;
	});
	if (hit) {
		hit->refs++;
		hit->lastUse = ++s_clock;
		return hit;
	}
	const DirRef* d = lookup(block, h);
	if (!d) return nullptr;
	trim(mem::budget::ANIM_CACHE_BYTES > d->size ? mem::budget::ANIM_CACHE_BYTES - d->size : 0);
	if (s_clips.full()) {
		Clip* v = lruVictim();
		if (!v) return nullptr;
		evict(*v);
	}
	void* data = mem::alloc(d->size, mem::TAG_ANIM);
	while (!data) {   // heap pressure: give back cached clips before failing
		Clip* v = lruVictim();
		if (!v) break;
		evict(*v);
		data = mem::alloc(d->size, mem::TAG_ANIM);
	}
	if (!data) return nullptr;
	if (!readAt(s_blocks[block], d->offset, data, d->size)) {
		mem::free(data);
		return nullptr;
	}
	Clip* c = s_clips.create();
	c->hash = h;
	c->size = d->size;
	c->lastUse = ++s_clock;
	c->refs = 1;
	c->block = (uint8_t)block;
	c->pinned = 0;
	c->data = (ds::AnimEntry*)data;
	s_bytes += d->size;
	return c;
}

void release(Clip* c) {
	if (c && c->refs) c->refs--;
}

bool pin(int block, const char* name) {
	Clip* c = acquire(block, name);
	if (!c) return false;
	c->pinned = 1;
	c->refs--;   // the pin is the permanent reference
	return true;
}

const ds::AnimEntry* entry(const Clip* c) { return c ? c->data : nullptr; }
uint32_t residentBytes() { return s_bytes; }
int residentClips() { return s_clips.count(); }

}
