#include "platform/fs.h"

#include <filesystem.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <nds/arm9/device_io.h>
#include <sys/stat.h>

namespace fs {

static bool s_ready;
static bool s_fat;

// When the assets live on the SD card instead of inside the ROM (small .nds +
// /gtasads/ dir at the card root), this device maps "game:/x" to the real
// path so every fopen() call site stays unchanged. Read-only: the game only
// reads data. The volume name depends on the environment: libnds mounts the
// DSi SD as "sd:", a DLDI driver (melonDS, flashcarts) as "fat:" - fs::init
// picks whichever actually holds our dir.
static char s_base[64] = "fat:/gtasads";
static int s_files[64];

static void resolve(const char* path, char* out, size_t size) {
	const char* rel = strchr(path, ':');
	rel = rel ? rel + 1 : path;
	if (*rel == '/') ++rel;
	snprintf(out, size, "%s/%s", s_base, rel);
}

static bool gameDrive(const char* name) { return strcmp(name, "game") == 0; }
static int gameOpen(const char* path, int flags, mode_t) {
	if ((flags & O_ACCMODE) != O_RDONLY) { errno = EROFS; return -1; }
	char real[256];
	resolve(path, real, sizeof real);
	int fd = open(real, O_RDONLY);
	if (fd < 0) return -1;
	for (unsigned i = 0; i < sizeof s_files / sizeof s_files[0]; i++) {
		if (s_files[i] < 0) { s_files[i] = fd; return (i + 1) << 2; }
	}
	close(fd);
	errno = EMFILE;
	return -1;
}
static int file(int fd) {
	const unsigned i = (unsigned)(fd >> 2);
	if (i > 0 && i <= sizeof s_files / sizeof s_files[0] && s_files[i - 1] >= 0) return s_files[i - 1];
	errno = EBADF;
	return -1;
}
static int gameClose(int fd) {
	const int real = file(fd);
	if (real < 0) return -1;
	s_files[(fd >> 2) - 1] = -1;
	return close(real);
}
static ssize_t gameRead(int fd, void* ptr, size_t len) {
	const int real = file(fd);
	return real < 0 ? -1 : read(real, ptr, len);
}
static off_t gameSeek(int fd, off_t pos, int whence) {
	const int real = file(fd);
	return real < 0 ? -1 : lseek(real, pos, whence);
}
static int gameFstat(int fd, struct stat* st) {
	const int real = file(fd);
	return real < 0 ? -1 : fstat(real, st);
}
static int gameStat(const char* path, struct stat* st) {
	char real[256];
	resolve(path, real, sizeof real);
	return stat(real, st);
}
static device_io_t s_gameDevice = {};

static bool installGameDevice() {
	for (int& fd : s_files) fd = -1;
	s_gameDevice.isdrive = gameDrive;
	s_gameDevice.open = gameOpen;
	s_gameDevice.close = gameClose;
	s_gameDevice.read = gameRead;
	s_gameDevice.lseek = gameSeek;
	s_gameDevice.fstat = gameFstat;
	s_gameDevice.stat = gameStat;
	return deviceIoAdd(&s_gameDevice) >= 0;
}

bool init() {
#ifndef GTASADS_FAT
	// Assets embedded in the .nds (emulator, flashcarts with argv or card reads).
	s_ready = nitroFSInit(NULL);
	if (s_ready) {
		strcpy(s_base, "nitro:");
		return s_ready = installGameDevice();
	}
#endif
	// Assets as loose files on the SD (TWiLight/nds-bootstrap can't load
	// a huge NitroFS homebrew): fat: is provided by the launcher/DSi SD
	// (hb bootstrap) or by a DLDI driver patched into the .nds (melonDS).
	s_ready = fatInitDefault();
	if (s_ready) {
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
		s_ready = installGameDevice();
	}
#if defined(GTASADS_DEBUG) && !defined(GTASADS_NO_FS_PROBE)
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
			fprintf(f, "base=%s\n", s_base);
			char path[96];
			for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
				snprintf(path, sizeof path, "game:/%s", probe[i]);
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
	snprintf(full, sizeof full, "game:/%s", path);
	FILE* f = fopen(full, "rb");
	if (!f) return -1;
	size_t n = fread(buf, 1, cap - 1, f);
	buf[n] = 0;
	fclose(f);
	return (long)n;
}

}
