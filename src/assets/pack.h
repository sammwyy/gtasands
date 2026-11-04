#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "assets/ds_formats.h"

// DSPK reader (NitroFS). The TOC (16 B/entry) is kept in RAM: gta3.dspk is
// ~16k entries = 256 KB, so a future step is indexing by model id instead.
class Pack {
public:
	bool open(const char* path);
	void close();

	uint32_t count() const { return m_count; }
	bool isOpen() const { return m_file != nullptr; }
	const ds::PackEntry* find(const char* name) const;
	// Reads an entry into a malloc'd buffer (caller frees). Returns NULL on failure.
	void* load(const ds::PackEntry* e) const;

private:
	FILE* m_file = nullptr;
	ds::PackEntry* m_toc = nullptr;
	uint32_t m_count = 0;
};
