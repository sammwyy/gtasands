#include "platform/fs.h"

#include <filesystem.h>
#include <stdio.h>

namespace fs {

static bool s_ready;

bool init() {
	s_ready = nitroFSInit(NULL);
	return s_ready;
}

bool ready() { return s_ready; }

long readText(const char* path, char* buf, size_t cap) {
	char full[256];
	snprintf(full, sizeof full, "nitro:/%s", path);
	FILE* f = fopen(full, "rb");
	if (!f) return -1;
	size_t n = fread(buf, 1, cap - 1, f);
	buf[n] = 0;
	fclose(f);
	return (long)n;
}

}
