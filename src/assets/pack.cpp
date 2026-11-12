#include "assets/pack.h"

#include <string.h>
#include "platform/fs.h"
#if defined(GTASADS_DEBUG) && defined(ARM9)
#include "platform/timing.h"
#endif

bool Pack::open(const char* path) {
	close();
	m_file = fopen(path, "rb");
	if (!m_file) return false;
	ds::PackHeader h;
	if (fread(&h, sizeof h, 1, m_file) != 1 || h.magic != DS_FOURCC('D', 'S', 'P', 'K')) {
		close();
		return false;
	}
	m_count = h.count;
	if (m_count <= RESIDENT_MAX) {
		m_toc = (ds::PackEntry*)mem::alloc(m_count * sizeof(ds::PackEntry), mem::TAG_PACK);
		if (!m_toc || fread(m_toc, sizeof(ds::PackEntry), m_count, m_file) != m_count) {
			close();
			return false;
		}
		m_pages = m_indexed = 0;
		return true;
	}
	m_pages = (m_count + PAGE - 1) / PAGE;
	m_indexed = 0;
	m_pageKeys = (uint32_t*)mem::alloc(m_pages * 4, mem::TAG_PACK);
	m_cache = (CachedPage*)mem::alloc(sizeof(CachedPage) * PAGE_CACHE, mem::TAG_PACK);
	if (!m_pageKeys || !m_cache) {
		close();
		return false;
	}
	for (int i = 0; i < PAGE_CACHE; i++) {
		m_cache[i].id = -1;
		m_cache[i].stamp = 0;
	}
	return true;
}

bool Pack::initLookupCache(uint32_t maxBytes) {
	return m_file && fs::initLookupCache(m_file, maxBytes) == 0;
}

bool Pack::indexStep(uint32_t pages) {
	if (!m_file) return false;
	if (m_indexed >= m_pages) return true;
	const uint32_t n = m_pages - m_indexed < pages ? m_pages - m_indexed : pages;
	mem::Scratch scratch;
	ds::PackEntry* buf = (ds::PackEntry*)scratch.alloc(n * PAGE * sizeof(ds::PackEntry));
	if (!buf) return false;
	const uint32_t first = m_indexed * PAGE;
	const uint32_t entries = (first + n * PAGE > m_count ? m_count - first : n * PAGE);
	if (!read(sizeof(ds::PackHeader) + first * sizeof(ds::PackEntry), buf, entries * sizeof(ds::PackEntry)))
		return false;
	for (uint32_t i = 0; i < n; i++) m_pageKeys[m_indexed + i] = buf[i * PAGE].hash;
	m_indexed += n;
	return m_indexed >= m_pages;
}

void Pack::close() {
	if (m_file) fclose(m_file);
	mem::free(m_toc);
	mem::free(m_pageKeys);
	mem::free(m_cache);
	m_file = nullptr;
	m_toc = nullptr;
	m_pageKeys = nullptr;
	m_cache = nullptr;
	m_count = m_pages = m_indexed = 0;
}

const ds::PackEntry* Pack::page(uint32_t p) const {
	CachedPage* victim = &m_cache[0];
	for (int i = 0; i < PAGE_CACHE; i++) {
		if (m_cache[i].id == (int32_t)p) {
			m_cache[i].stamp = ++m_stamp;
			return m_cache[i].e;
		}
		if (m_cache[i].stamp < victim->stamp) victim = &m_cache[i];
	}
	const uint32_t first = p * PAGE;
	const uint32_t n = first + PAGE > m_count ? m_count - first : PAGE;
	victim->id = -1;
	if (!read(sizeof(ds::PackHeader) + first * sizeof(ds::PackEntry), victim->e, n * sizeof(ds::PackEntry)))
		return nullptr;
	victim->id = (int32_t)p;
	victim->stamp = ++m_stamp;
	return victim->e;
}

static const ds::PackEntry* lowerBound(const ds::PackEntry* e, uint32_t n, uint32_t h) {
	uint32_t lo = 0, hi = n;
	while (lo < hi) {
		const uint32_t mid = (lo + hi) / 2;
		if (e[mid].hash < h) lo = mid + 1;
		else hi = mid;
	}
	return lo < n && e[lo].hash == h ? &e[lo] : nullptr;
}

// Hash collisions are not resolved by name yet (none in the stock archives).
bool Pack::find(const char* name, ds::PackEntry& out) const {
	if (!m_file || !name) return false;
	const uint32_t h = ds::fnv1a(name);
	const ds::PackEntry* e = nullptr;
	if (m_toc) {
		e = lowerBound(m_toc, m_count, h);
	} else {
		if (m_indexed < m_pages) return false;
		// last page whose first hash <= h
		uint32_t lo = 0, hi = m_pages;
		while (lo < hi) {
			const uint32_t mid = (lo + hi) / 2;
			if (m_pageKeys[mid] <= h) lo = mid + 1;
			else hi = mid;
		}
		if (!lo) return false;
		const uint32_t p = lo - 1;
		const ds::PackEntry* pg = page(p);
		if (pg) e = lowerBound(pg, p * PAGE + PAGE > m_count ? m_count - p * PAGE : PAGE, h);
	}
	if (!e) return false;
	out = *e;
	return true;
}

bool Pack::read(uint32_t offset, void* dst, uint32_t size) const {
	if (!m_file) return false;
#if defined(GTASADS_DEBUG) && defined(ARM9)
	const uint32_t seekStart = timing::ticks();
#endif
	const bool seekOk = fseek(m_file, (long)offset, SEEK_SET) == 0;
#if defined(GTASADS_DEBUG) && defined(ARM9)
	m_lastSeekUs = timing::usSince(seekStart);
	const uint32_t readStart = timing::ticks();
#endif
	const bool readOk = seekOk && fread(dst, 1, size, m_file) == size;
#if defined(GTASADS_DEBUG) && defined(ARM9)
	m_lastDataUs = seekOk ? timing::usSince(readStart) : 0;
#endif
	return readOk;
}

void* Pack::load(const ds::PackEntry& e, mem::Tag tag) const {
	void* buf = mem::alloc(e.size, tag);
	if (buf && !read(e.offset, buf, e.size)) {
		mem::free(buf);
		buf = nullptr;
	}
	return buf;
}

void* Pack::load(const char* name, mem::Tag tag, uint32_t* size) const {
	ds::PackEntry e;
	if (!find(name, e)) return nullptr;
	if (size) *size = e.size;
	return load(e, tag);
}

size_t Pack::residentBytes() const {
	if (m_toc) return m_count * sizeof(ds::PackEntry);
	return m_pages * 4 + (m_cache ? sizeof(CachedPage) * PAGE_CACHE : 0);
}
