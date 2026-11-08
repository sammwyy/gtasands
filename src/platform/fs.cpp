#include "platform/fs.h"

#include <filesystem.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include <errno.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

namespace fs {

static bool s_ready;
static bool s_fat;

// When the assets live on the SD card instead of inside the ROM (small .nds +
// /gtasads/ dir at the card root), this device maps "nitro:/x" to the real
// path so every fopen() call site stays unchanged. Read-only: the game only
// reads data. The volume name depends on the environment: libnds mounts the
// DSi SD as "sd:", a DLDI driver (melonDS, flashcarts) as "fat:" - fs::init
// picks whichever actually holds our dir.
static char s_base[64] = "fat:/gtasads";
static char s_path[256];

static const char* resolve(const char* path) {
	// devoptab hands us the path unmodified ("nitro:/x"); strip the device
	// name if present, then any leading slash
	const char* rel = strrchr(path, ':');
	rel = rel ? rel + 1 : path;
	if (*rel == '/') ++rel;
	snprintf(s_path, sizeof s_path, "%s/%s", s_base, rel);
	return s_path;
}

static int fatNitroOpen(_reent* r, void* fileStruct, const char* path, int, int) {
	FILE* f = fopen(resolve(path), "rb");
	if (!f) { r->_errno = ENOENT; return -1; }
	*(FILE**)fileStruct = f;
	return 0;
}
static int fatNitroClose(_reent*, void* fileStruct) {
	return fclose(*(FILE**)fileStruct);
}
static ssize_t fatNitroRead(_reent* r, void* fileStruct, char* ptr, size_t len) {
	ssize_t ret = (ssize_t)fread(ptr, 1, len, *(FILE**)fileStruct);
	if (ret < 0) r->_errno = EIO;
	return ret;
}
static off_t fatNitroSeek(_reent* r, void* fileStruct, off_t pos, int dir) {
	FILE* f = *(FILE**)fileStruct;
	if (fseek(f, pos, dir) < 0) { r->_errno = EINVAL; return -1; }
	return ftell(f);
}
static int fatNitroFstat(_reent*, void* fileStruct, struct stat* st) {
	FILE* f = *(FILE**)fileStruct;
	long cur = ftell(f);
	fseek(f, 0, SEEK_END);
	st->st_size = ftell(f);
	st->st_mode = S_IFREG;
	fseek(f, cur, SEEK_SET);
	return 0;
}
static int fatNitroStat(_reent* r, const char* path, struct stat* st) {
	FILE* f = fopen(resolve(path), "rb");
	if (!f) { r->_errno = ENOENT; return -1; }
	fseek(f, 0, SEEK_END);
	st->st_size = ftell(f);
	st->st_mode = S_IFREG;
	fclose(f);
	return 0;
}

static devoptab_t s_fatNitro;

static void installFatNitro() {
	s_fatNitro.name = "nitro";
	s_fatNitro.structSize = sizeof(FILE*);
	s_fatNitro.open_r = fatNitroOpen;
	s_fatNitro.close_r = fatNitroClose;
	s_fatNitro.read_r = fatNitroRead;
	s_fatNitro.seek_r = fatNitroSeek;
	s_fatNitro.fstat_r = fatNitroFstat;
	s_fatNitro.stat_r = fatNitroStat;
	AddDevice(&s_fatNitro);
}

bool init() {
#ifndef GTASADS_FAT
	// Assets embedded in the .nds (emulator, flashcarts with argv or card reads).
	s_ready = nitroFSInit(NULL);
	if (s_ready) return true;
#endif
	// Assets as loose files on the SD (TWiLight/nds-bootstrap can't load
	// a huge NitroFS homebrew): fat: is provided by the launcher/DSi SD
	// (hb bootstrap) or by a DLDI driver patched into the .nds (melonDS).
	s_ready = fatInitDefault();
	if (s_ready) {
		installFatNitro();
		s_fat = true;
		// pick the volume that really has the dir ("sd:" on a DSi, "fat:" via DLDI)
		struct stat st;
		for (const char* vol : { "sd", "fat" }) {
			snprintf(s_base, sizeof s_base, "%s:/gtasads", vol);
			if (stat(s_base, &st) == 0) break;
			s_base[0] = 0;
		}
		if (!s_base[0]) { // dir not found anywhere: still pick a sane default
			snprintf(s_base, sizeof s_base, "%s:/gtasads",
			         stat("sd:/", &st) == 0 ? "sd" : "fat");
		}
	}
#ifdef GTASADS_DEBUG
	if (s_fat) {
		// boot probe: proves the SD path works when there's no screen to read,
		// and reports per-file status (readable back on the PC from the real SD)
		static const char* probe[] = {
			"text/american.gxt", "movies/logo.dvid", "audio/sfx/genrl.dsfx",
			"models/txd/loadscs.txd", "models/gta3.dspk", "anim/ped.ifp",
		};
		FILE* f = nullptr;
		char mpath[64];
		for (const char* vol : { "sd", "fat" }) {
			snprintf(mpath, sizeof mpath, "%s:/gtasads_fsok.txt", vol);
			f = fopen(mpath, "w");
			if (f) break;
		}
		if (f) {
			struct stat st;
			fprintf(f, "base=%s sdroot=%d fatroot=%d\n", s_base,
			        stat("sd:/", &st) == 0, stat("fat:/", &st) == 0);
			char path[96];
			for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
				snprintf(path, sizeof path, "nitro:/%s", probe[i]);
				FILE* t = fopen(path, "rb");
				long sz = -1, magic = -1;
				if (t) {
					unsigned char hdr[4];
					magic = fread(hdr, 1, 4, t);
					fseek(t, 0, SEEK_END);
					sz = ftell(t);
					fclose(t);
				}
				fprintf(f, "%-30s %s sz=%ld hdr=%ld\n", probe[i],
				        t ? "ok" : "MISSING", sz, magic);
			}
			fclose(f);
		}
	}
#endif
	return s_ready;
}

bool ready() { return s_ready; }
const char* backend() { return s_ready ? (s_fat ? s_base : "nitrofs") : "none"; }

const char* radioRoot() {
	if (!s_ready) return nullptr;
	static bool triedMount, mounted;
	if (!s_fat && !triedMount) { mounted = fatInitDefault(); triedMount = true; }
	if (!s_fat && !mounted) return nullptr;
	static char root[80];
	char path[112];
	for (const char* vol : { "sd", "fat" }) {
		snprintf(root, sizeof root, "%s:/gtasads/audio/streams", vol);
		snprintf(path, sizeof path, "%s/ch.dsst", root);
		FILE* f = fopen(path, "rb");
		if (f) { fclose(f); return root; }
	}
	return nullptr;
}

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
