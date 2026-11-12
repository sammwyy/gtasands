#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "assets/ds_formats.h"
#include "mem/mem.h"

// DSPK reader. Small packs (player/anim, <= RESIDENT_MAX entries) keep the
// whole TOC in RAM. Big packs (gta3.dspk, ~16k entries = 256 KB of TOC) keep
// only a sparse index (first hash of every 64-entry page, ~1 KB) plus a
// 4-page LRU cache: a lookup costs at most one 1 KB read. The sparse index is
// built incrementally by indexStep() so the loading screen never stalls on it.
class Pack {
public:
	static constexpr uint32_t RESIDENT_MAX = 2048;
	static constexpr uint32_t PAGE = 64;          // entries per TOC page (1 KB)
	static constexpr int PAGE_CACHE = 4;

	bool open(const char* path);
	bool initLookupCache(uint32_t maxBytes);
	// Builds the sparse index a few pages per call; true once lookups work.
	// Resident packs are ready right after open().
	bool indexStep(uint32_t pages = 16);
	bool ready() const { return m_file && m_indexed >= m_pages; }
	void close();

	uint32_t count() const { return m_count; }
	bool isOpen() const { return m_file != nullptr; }
	bool find(const char* name, ds::PackEntry& out) const;
	// Reads an entry into a mem::alloc block (free with mem::free). nullptr if
	// missing. `size` receives the entry size when non-null.
	void* load(const char* name, mem::Tag tag, uint32_t* size = nullptr) const;
	void* load(const ds::PackEntry& e, mem::Tag tag) const;
	// Raw read at an absolute file offset (partial entry reads).
	bool read(uint32_t offset, void* dst, uint32_t size) const;
#if defined(GTASADS_DEBUG) && defined(ARM9)
	uint32_t lastSeekUs() const { return m_lastSeekUs; }
	uint32_t lastDataUs() const { return m_lastDataUs; }
#endif
	size_t residentBytes() const;

private:
	const ds::PackEntry* page(uint32_t p) const;

	FILE* m_file = nullptr;
#if defined(GTASADS_DEBUG) && defined(ARM9)
	mutable uint32_t m_lastSeekUs = 0, m_lastDataUs = 0;
#endif
	uint32_t m_count = 0;
	uint32_t m_pages = 0, m_indexed = 0;
	ds::PackEntry* m_toc = nullptr;      // resident mode
	uint32_t* m_pageKeys = nullptr;      // paged mode: first hash per page
	struct CachedPage { int32_t id; uint32_t stamp; ds::PackEntry e[PAGE]; };
	mutable CachedPage* m_cache = nullptr;
	mutable uint32_t m_stamp = 0;
};
