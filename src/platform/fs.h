#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// NitroFS access. Paths are relative to the nitrofs/ root (e.g. "data/gta.dat").
namespace fs {

bool init();
bool ready();
// "nitrofs" (embedded in .nds) | "fat:/gtasads" (loose files on SD) | "none".
const char* backend();
const char* radioRoot();
int initLookupCache(FILE* stream, uint32_t maxBytes);
// Reads up to cap-1 bytes and NUL-terminates. Returns bytes read or -1.
long readText(const char* path, char* buf, size_t cap);

}
