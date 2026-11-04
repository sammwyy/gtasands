#include "game/text/gxt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CText TheText;

// SA GXT key hash: CRC32 table, init 0xFFFFFFFF, uppercased chars, no final xor.
static uint32_t gxtKey(const char* s) {
	static uint32_t tab[256];
	if (!tab[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
			tab[i] = c;
		}
	}
	uint32_t key = 0xFFFFFFFFu;
	for (; *s; s++) {
		uint8_t c = (uint8_t)*s;
		if (c >= 'a' && c <= 'z') c -= 0x20;
		key = tab[(c ^ key) & 0xFF] ^ (key >> 8);
	}
	return key;
}

bool CText::load(const char* path) {
	unload();
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 4, SEEK_SET); // u16 version, u16 encoding
	bool gotTkey = false, gotTdat = false;
	while (!gotTkey || !gotTdat) {
		char magic[4];
		uint32_t size;
		if (fread(magic, 1, 4, f) != 4 || fread(&size, 4, 1, f) != 1 || !size) break;
		if (!memcmp(magic, "TKEY", 4)) {
			m_keyCount = size / 8;
			m_keys = (KeyEntry*)malloc(size);
			gotTkey = fread(m_keys, 1, size, f) == size;
		} else if (!memcmp(magic, "TDAT", 4)) {
			m_data = (char*)malloc(size);
			m_dataSize = size;
			gotTdat = fread(m_data, 1, size, f) == size;
		} else {
			fseek(f, size, SEEK_CUR); // TABL etc.
		}
	}
	fclose(f);
	if (!(gotTkey && gotTdat)) {
		unload();
		return false;
	}
	return true;
}

void CText::unload() {
	free(m_keys);
	free(m_data);
	m_keys = nullptr;
	m_data = nullptr;
	m_keyCount = m_dataSize = 0;
}

const GxtChar* CText::get(const char* key) const {
	static const GxtChar missing[] = { 'M', 'I', 'S', 'S', 'I', 'N', 'G', 0 };
	uint32_t h = gxtKey(key);
	int lo = 0, hi = (int)m_keyCount - 1;
	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		if (m_keys[mid].hash == h)
			return m_keys[mid].offset < m_dataSize ? (const GxtChar*)m_data + m_keys[mid].offset : missing;
		if (h > m_keys[mid].hash) lo = mid + 1;
		else hi = mid - 1;
	}
	return missing;
}
