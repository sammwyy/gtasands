#pragma once

#include <stdio.h>

#include "assets/ds_formats.h"
#include "mem/mem.h"

// A loose DTXD opened for single-entry reads: only the directory (40 B per
// texture) is read up front, each texture is read on demand into the
// caller's scratch scope. fronten2.txd is ~490 KB and hud.txd ~46 KB; the
// screens that use them need one or two entries at a time.
class TxdFile {
public:
	bool open(const char* path);
	void close();
	~TxdFile() { close(); }
	bool isOpen() const { return m_file != nullptr; }
	int count() const { return m_count; }
	// Reads texture `name` (nullptr = the first one) into scratch.
	const ds::TexHeader* load(const char* name, mem::Scratch& scratch) const;

private:
	FILE* m_file = nullptr;
	ds::TxdEntry* m_dir = nullptr;
	int m_count = 0;
};
