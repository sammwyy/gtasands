#include "audio/aehardware.h"

#include <nds.h>

#include "assets/ds_formats.h"
#ifdef GTASADS_DEBUG
#include <stdio.h>
#include <string.h>

#include "mem/mem.h"
#include "platform/timing.h"
#endif

namespace aehw {

static bool s_on;
static uint32_t s_sfxMask = (1u << SFX_CHANNELS) - 1;

void init() {
	if (s_on) return;
	s_on = true;
	soundEnable();
	// channel state (soundGetActiveChannels) refreshed by the ARM7 at 192 Hz:
	// reading it never blocks on a synchronise round trip
	soundSetAutoUpdate(true);
	soundSetMixerVolume(127);
}

static unsigned timerFor(unsigned hz) {
	if (hz < 1000) hz = 1000;
	if (hz > 65000) hz = 65000;
	return soundTimerFromHz(hz);
}

void play(int ch, const Sample& s, unsigned vol, unsigned pan, unsigned hz) {
	if (ch < 0 || ch >= NUM_CHANNELS || !s.data) return;
	const unsigned words = s.bytes / 4;
	const SoundFmt fmt = s.fmt == ds::SND_ADPCM ? SoundFmt_ImaAdpcm : s.fmt == ds::SND_PCM16 ? SoundFmt_Pcm16 : SoundFmt_Pcm8;
	unsigned pnt = 0, len = words;
	SoundMode mode = SoundMode_OneShot;
	if (s.loopStart >= 0) {
		// loop point in words. ADPCM: after the header word, at a whole word
		// of nibbles (8 samples); the mixer saves the decoder state when it
		// first passes it and restores it on every repeat
		const uint32_t ls = (uint32_t)s.loopStart;
		pnt = s.fmt == ds::SND_ADPCM ? 1 + ls / 8 : s.fmt == ds::SND_PCM16 ? ls / 2 : ls / 4;
		if (pnt + 1 < words) {
			len = words - pnt;
			mode = SoundMode_Repeat;
		} else {
			pnt = 0;
		}
	}
	if (vol > VOL_MAX) vol = VOL_MAX;
	soundPreparePcm((unsigned)ch | SOUND_START, vol, pan & 127, timerFor(hz), mode, fmt, s.data, pnt, len);
}

void playRing(int ch, const void* pcm16, uint32_t bytes, unsigned vol, unsigned pan, unsigned hz) {
	if (ch < 0 || ch >= NUM_CHANNELS) return;
	soundPreparePcm((unsigned)ch | SOUND_START, vol > VOL_MAX ? VOL_MAX : vol, pan & 127, timerFor(hz),
	                SoundMode_Repeat, SoundFmt_Pcm16, pcm16, 0, bytes / 4);
}

void setVolume(int ch, unsigned vol) { soundChSetVolume((unsigned)ch, vol > VOL_MAX ? VOL_MAX : vol); }
void setPan(int ch, unsigned pan) { soundChSetPan((unsigned)ch, pan & 127); }
void setFreq(int ch, unsigned hz) { soundChSetTimer((unsigned)ch, timerFor(hz)); }
void stop(int ch) { if (ch >= 0 && ch < NUM_CHANNELS) soundStop(1u << ch); }
uint32_t activeMask() { return s_on ? soundGetActiveChannels() : 0; }
void setMasterVolume(unsigned v) { soundSetMixerVolume(v > 127 ? 127 : v); }
uint32_t sfxMask() { return s_sfxMask; }

#ifdef GTASADS_DEBUG
static int16_t* s_cap[2];
static uint32_t s_capSamples, s_capHz, s_capEndVbl;
static int16_t s_silence[16] __attribute__((aligned(4)));

bool captureStart(uint32_t samples, unsigned hz) {
	if (s_cap[0]) return false;
	for (int i = 0; i < 2; i++) {
		s_cap[i] = (int16_t*)mem::zalloc(samples * 2, mem::TAG_TEMP);
		if (!s_cap[i]) return false;
		DC_FlushRange(s_cap[i], samples * 2);
	}
	s_capSamples = samples;
	s_capHz = hz;
	// the sound manager gives up 1 and 3: their timers clock the capture
	s_sfxMask &= ~((1u << 1) | (1u << 3));
	static const int k_capCh[2] = { 1, 3 };
	for (int ch : k_capCh) {
		stop(ch);
		soundPreparePcm((unsigned)ch, 0, 64, timerFor(hz), SoundMode_Repeat, SoundFmt_Pcm16, s_silence, 0, sizeof s_silence / 4);
	}
	soundPrepareCap(0, SoundCapDst_ChBNormal, SoundCapSrc_Mixer, false, SoundCapFmt_Pcm16, s_cap[0], samples / 2);
	soundPrepareCap(1, SoundCapDst_ChBNormal, SoundCapSrc_Mixer, false, SoundCapFmt_Pcm16, s_cap[1], samples / 2);
	soundStart((1u << 1) | (1u << 3) | (3u << 16));
	s_capEndVbl = timing::vblanks() + (uint32_t)((uint64_t)samples * 60 / hz) + 6;
	return true;
}

bool captureBusy() { return s_cap[0] && timing::vblanks() < s_capEndVbl; }

bool captureSave(const char* path) {
	if (!s_cap[0] || captureBusy()) return false;
	soundStop((1u << 1) | (1u << 3) | (3u << 16));
	s_sfxMask = (1u << SFX_CHANNELS) - 1;
	DC_InvalidateRange(s_cap[0], s_capSamples * 2);
	DC_InvalidateRange(s_cap[1], s_capSamples * 2);
	FILE* f = fopen(path, "wb");
	if (f) {
		const uint32_t data = s_capSamples * 4;
		const uint32_t hdr[11] = { 0x46464952u, 36 + data, 0x45564157u, 0x20746D66u, 16, 0x00020001u, s_capHz,
		                           s_capHz * 4, 0x00100004u, 0x61746164u, data };
		fwrite(hdr, 4, 11, f);
		int16_t buf[512];
		for (uint32_t i = 0; i < s_capSamples; i += 256) {
			const uint32_t n = s_capSamples - i < 256 ? s_capSamples - i : 256;
			for (uint32_t k = 0; k < n; k++) {
				buf[k * 2] = s_cap[0][i + k];
				buf[k * 2 + 1] = s_cap[1][i + k];
			}
			fwrite(buf, 4, n, f);
		}
		fclose(f);
	}
	mem::free(s_cap[0]);
	mem::free(s_cap[1]);
	s_cap[0] = s_cap[1] = nullptr;
	return f != nullptr;
}
#endif

}
