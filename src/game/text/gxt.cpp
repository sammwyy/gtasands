#include "game/text/gxt.h"

#include <stdio.h>
#include <string.h>

#include "mem/mem.h"

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
			m_keys = (KeyEntry*)mem::alloc(size, mem::TAG_TEXT);
			gotTkey = m_keys && fread(m_keys, 1, size, f) == size;
		} else if (!memcmp(magic, "TDAT", 4)) {
			m_data = (char*)mem::alloc(size, mem::TAG_TEXT);
			m_dataSize = size;
			gotTdat = m_data && fread(m_data, 1, size, f) == size;
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

bool CText::loadMission(const char* path, const char* table) {
	mem::free(m_missionKeys);
	mem::free(m_missionData);
	m_missionKeys = nullptr;
	m_missionData = nullptr;
	m_missionKeyCount = m_missionDataSize = 0;
	FILE* f = fopen(path, "rb");
	if (!f || fseek(f, 4, SEEK_SET) != 0) {
		if (f) fclose(f);
		return false;
	}
	char tag[4];
	uint32_t bytes = 0;
	if (fread(tag, 1, 4, f) != 4 || memcmp(tag, "TABL", 4) ||
	    fread(&bytes, sizeof bytes, 1, f) != 1 || bytes > 4096 || bytes % 12) {
		fclose(f);
		return false;
	}
	uint32_t offset = 0;
	for (uint32_t i = 0; i < bytes / 12; i++) {
		char name[8];
		uint32_t at;
		if (fread(name, 1, sizeof name, f) != sizeof name || fread(&at, 4, 1, f) != 1) break;
		if (!strncmp(name, table, sizeof name)) { offset = at; break; }
	}
	bool ok = offset && fseek(f, offset + 8, SEEK_SET) == 0;
	for (int i = 0; ok && i < 2; i++) {
		uint32_t size;
		if (fread(tag, 1, 4, f) != 4 || fread(&size, 4, 1, f) != 1 || !size || size > 65536) {
			ok = false;
			break;
		}
		if (!memcmp(tag, "TKEY", 4) && size % 8 == 0) {
			m_missionKeys = (KeyEntry*)mem::alloc(size, mem::TAG_TEXT);
			m_missionKeyCount = size / 8;
			ok = m_missionKeys && fread(m_missionKeys, 1, size, f) == size;
		} else if (!memcmp(tag, "TDAT", 4)) {
			m_missionData = (char*)mem::alloc(size, mem::TAG_TEXT);
			m_missionDataSize = size;
			ok = m_missionData && fread(m_missionData, 1, size, f) == size;
		} else ok = false;
	}
	fclose(f);
	if (ok) ok = m_missionKeys && m_missionData;
	if (!ok) {
		mem::free(m_missionKeys);
		mem::free(m_missionData);
		m_missionKeys = nullptr;
		m_missionData = nullptr;
		m_missionKeyCount = m_missionDataSize = 0;
	}
	return ok;
}

void CText::unload() {
	mem::free(m_keys);
	mem::free(m_data);
	mem::free(m_missionKeys);
	mem::free(m_missionData);
	m_keys = nullptr;
	m_data = nullptr;
	m_keyCount = m_dataSize = 0;
	m_missionKeys = nullptr;
	m_missionData = nullptr;
	m_missionKeyCount = m_missionDataSize = 0;
}

const GxtChar* CText::get(const char* key) const {
	static const GxtChar missing[] = { 'M', 'I', 'S', 'S', 'I', 'N', 'G', 0 };
	uint32_t h = gxtKey(key);
	auto lookup = [&](const KeyEntry* keys, uint32_t count, const char* data, uint32_t size) -> const GxtChar* {
		int lo = 0, hi = (int)count - 1;
		while (lo <= hi) {
			int mid = (lo + hi) >> 1;
			if (keys[mid].hash == h)
				return keys[mid].offset < size ? (const GxtChar*)data + keys[mid].offset : nullptr;
			if (h > keys[mid].hash) lo = mid + 1;
			else hi = mid - 1;
		}
		return nullptr;
	};
	if (m_missionKeys)
		if (const GxtChar* s = lookup(m_missionKeys, m_missionKeyCount, m_missionData, m_missionDataSize))
			return s;
	const GxtChar* s = m_keys ? lookup(m_keys, m_keyCount, m_data, m_dataSize) : nullptr;
	return s ? s : missing;
}
