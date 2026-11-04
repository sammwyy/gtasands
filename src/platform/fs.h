#pragma once

#include <stddef.h>

// NitroFS access. Paths are relative to the nitrofs/ root (e.g. "data/gta.dat").
namespace fs {

bool init();
bool ready();
// Reads up to cap-1 bytes and NUL-terminates. Returns bytes read or -1.
long readText(const char* path, char* buf, size_t cap);

}
