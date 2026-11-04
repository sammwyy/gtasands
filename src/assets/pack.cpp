#include "assets/pack.h"

#include <stdlib.h>

bool Pack::open(const char* path) {
	close();
	m_file = fopen(path, "rb");
	if (!m_file) return false;
	ds::PackHeader h;
	if (fread(&h, sizeof h, 1, m_file) != 1 || h.magic != DS_FOURCC('D', 'S', 'P', 'K')) {
		close();
		return false;
	}
	m_toc = (ds::PackEntry*)malloc(h.count * sizeof(ds::PackEntry));
	if (!m_toc || fread(m_toc, sizeof(ds::PackEntry), h.count, m_file) != h.count) {
		close();
		return false;
	}
	m_count = h.count;
	return true;
}

void Pack::close() {
	if (m_file) fclose(m_file);
	free(m_toc);
	m_file = nullptr;
	m_toc = nullptr;
	m_count = 0;
}

// Hash collisions are not resolved by name yet (none in the stock archives).
const ds::PackEntry* Pack::find(const char* name) const {
	uint32_t h = ds::fnv1a(name);
	uint32_t lo = 0, hi = m_count;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (m_toc[mid].hash < h) lo = mid + 1;
		else hi = mid;
	}
	return lo < m_count && m_toc[lo].hash == h ? &m_toc[lo] : nullptr;
}

void* Pack::load(const ds::PackEntry* e) const {
	if (!e || !m_file) return nullptr;
	void* buf = malloc(e->size);
	if (!buf) return nullptr;
	if (fseek(m_file, e->offset, SEEK_SET) != 0 || fread(buf, 1, e->size, m_file) != e->size) {
		free(buf);
		return nullptr;
	}
	return buf;
}
