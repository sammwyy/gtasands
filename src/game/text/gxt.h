#pragma once

#include <stdint.h>

// CText port: GXT main table (TKEY/TDAT). GxtChar is the game's 8-bit encoding.
typedef uint8_t GxtChar;

class CText {
public:
	bool load(const char* path);
	void unload();
	const GxtChar* get(const char* key) const;

private:
	struct KeyEntry { uint32_t offset, hash; };
	KeyEntry* m_keys = nullptr;
	uint32_t m_keyCount = 0;
	char* m_data = nullptr;
	uint32_t m_dataSize = 0;
};

extern CText TheText;
