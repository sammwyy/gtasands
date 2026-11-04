#include "assets/txdfile.h"

#include <string.h>

bool TxdFile::open(const char* path) {
	close();
	m_file = fopen(path, "rb");
	if (!m_file) return false;
	ds::TxdHeader th;
	if (fread(&th, sizeof th, 1, m_file) != 1 || th.magic != DS_FOURCC('D', 'T', 'X', 'D')) {
		close();
		return false;
	}
	m_dir = (ds::TxdEntry*)mem::alloc(th.count * sizeof(ds::TxdEntry), mem::TAG_TEXTURE);
	if (!m_dir || fread(m_dir, sizeof(ds::TxdEntry), th.count, m_file) != th.count) {
		close();
		return false;
	}
	m_count = th.count;
	return true;
}

void TxdFile::close() {
	if (m_file) fclose(m_file);
	mem::free(m_dir);
	m_file = nullptr;
	m_dir = nullptr;
	m_count = 0;
}

const ds::TexHeader* TxdFile::load(const char* name, mem::Scratch& scratch) const {
	for (int i = 0; i < m_count; i++) {
		if (name && strcmp(m_dir[i].name, name)) continue;
		void* buf = scratch.alloc(m_dir[i].size);
		if (!buf || fseek(m_file, m_dir[i].offset, SEEK_SET) != 0 ||
		    fread(buf, 1, m_dir[i].size, m_file) != m_dir[i].size)
			return nullptr;
		const ds::TexHeader* h = (const ds::TexHeader*)buf;
		return h->magic == DS_FOURCC('D', 'T', 'E', 'X') ? h : nullptr;
	}
	return nullptr;
}
