#include "audio/aehardware.h"

#include <nds.h>

#include "assets/ds_formats.h"
#include "platform/timing.h"
#ifdef GTASADS_DEBUG
#include <stdio.h>
#include <string.h>

#include "mem/mem.h"
#endif

namespace aehw {

static bool s_on;
static uint32_t s_sfxMask = (1u << SFX_CHANNELS) - 1;
struct Channel {
	int id = -1;
	uint32_t remaining, started;
	unsigned hz;
	bool loop;
};
static Channel s_channels[NUM_CHANNELS];

static unsigned frequency(unsigned hz) {
	return hz < 1000 ? 1000 : hz > 65000 ? 65000 : hz;
}
static unsigned mixerVolume(unsigned vol) {
	return (vol > VOL_MAX ? VOL_MAX : vol) * 127 / VOL_MAX;
}
static bool active(Channel& c) {
	if (c.id < 0) return false;
	if (c.loop) return true;
	const uint32_t played = (uint32_t)((uint64_t)(timing::vblanks() - c.started) * c.hz / 60);
	if (played >= c.remaining) { c.id = -1; return false; }
	return true;
}

void init() {
	if (s_on) return;
	s_on = true;
	soundEnable();
	soundSetMasterVolume(127);
}

void play(int ch, const Sample& s, unsigned vol, unsigned pan, unsigned hz) {
	if (ch < 0 || ch >= NUM_CHANNELS || !s.data) return;
	stop(ch);
	const unsigned words = s.bytes / 4;
	const SoundFormat fmt = s.fmt == ds::SND_ADPCM ? SoundFormat_ADPCM :
	                        s.fmt == ds::SND_PCM16 ? SoundFormat_16Bit : SoundFormat_8Bit;
	unsigned pnt = 0;
	bool loop = false;
	if (s.loopStart >= 0) {
		// loop point in words. ADPCM: after the header word, at a whole word
		// of nibbles (8 samples); the mixer saves the decoder state when it
		// first passes it and restores it on every repeat
		const uint32_t ls = (uint32_t)s.loopStart;
		pnt = s.fmt == ds::SND_ADPCM ? 1 + ls / 8 : s.fmt == ds::SND_PCM16 ? ls / 2 : ls / 4;
		loop = pnt + 1 < words && pnt <= UINT16_MAX;
	}
	Channel& c = s_channels[ch];
	c.hz = frequency(hz);
	c.remaining = s.numSamples;
	c.started = timing::vblanks();
	c.loop = loop;
	c.id = soundPlaySampleChannel(ch, s.data, fmt, s.bytes, c.hz, mixerVolume(vol), pan & 127, loop, loop ? pnt : 0);
}

void playRing(int ch, const void* pcm16, uint32_t bytes, unsigned vol, unsigned pan, unsigned hz) {
	if (ch < 0 || ch >= NUM_CHANNELS || !pcm16) return;
	stop(ch);
	Channel& c = s_channels[ch];
	c.hz = frequency(hz);
	c.loop = true;
	c.id = soundPlaySampleChannel(ch, pcm16, SoundFormat_16Bit, bytes, c.hz, mixerVolume(vol), pan & 127, true, 0);
}

void setVolume(int ch, unsigned vol) {
	if (ch >= 0 && ch < NUM_CHANNELS && active(s_channels[ch]))
		soundSetVolume(s_channels[ch].id, mixerVolume(vol));
}
void setPan(int ch, unsigned pan) {
	if (ch >= 0 && ch < NUM_CHANNELS && active(s_channels[ch]))
		soundSetPan(s_channels[ch].id, pan & 127);
}
void setFreq(int ch, unsigned hz) {
	if (ch < 0 || ch >= NUM_CHANNELS || !active(s_channels[ch])) return;
	Channel& c = s_channels[ch];
	if (!c.loop) {
		c.remaining -= (uint32_t)((uint64_t)(timing::vblanks() - c.started) * c.hz / 60);
		c.started = timing::vblanks();
	}
	c.hz = frequency(hz);
	soundSetFreq(c.id, c.hz);
}
void stop(int ch) {
	if (ch < 0 || ch >= NUM_CHANNELS) return;
	Channel& c = s_channels[ch];
	if (active(c)) soundKill(c.id);
	c.id = -1;
}
uint32_t activeMask() {
	uint32_t mask = 0;
	if (s_on) for (int i = 0; i < NUM_CHANNELS; i++)
		if (active(s_channels[i])) mask |= 1u << i;
	return mask;
}
void setMasterVolume(unsigned v) { soundSetMasterVolume(v > 127 ? 127 : v); }
uint32_t sfxMask() { return s_sfxMask; }

#ifdef GTASADS_DEBUG
static int16_t* s_cap[2];
static uint32_t s_capSamples, s_capHz, s_capEndVbl;
static int16_t s_silence[16] __attribute__((aligned(4)));

bool captureStart(uint32_t samples, unsigned hz) {
	if (s_cap[0]) return false;
	while (samples >= 8192) {
		s_cap[0] = (int16_t*)mem::zalloc(samples * 2, mem::TAG_TEMP);
		s_cap[1] = (int16_t*)mem::zalloc(samples * 2, mem::TAG_TEMP);
		if (s_cap[0] && s_cap[1]) break;
		mem::free(s_cap[0]);
		mem::free(s_cap[1]);
		s_cap[0] = s_cap[1] = nullptr;
		samples /= 2;
	}
	if (!s_cap[0]) return false;
	for (int i = 0; i < 2; i++) DC_FlushRange(s_cap[i], samples * 2);
	s_capSamples = samples;
	s_capHz = hz;
	// the sound manager gives up 1 and 3: their timers clock the capture
	s_sfxMask &= ~((1u << 1) | (1u << 3));
	static const int k_capCh[2] = { 1, 3 };
	for (int ch : k_capCh) {
		stop(ch);
		s_channels[ch].hz = frequency(hz);
		s_channels[ch].loop = true;
		s_channels[ch].id = soundPlaySampleChannel(ch, s_silence, SoundFormat_16Bit, sizeof s_silence,
		                                          s_channels[ch].hz, 0, 64, true, 0);
	}
	soundCaptureStart(s_cap[0], samples / 2, 0, false, true, false, SoundCaptureFormat_16Bit);
	soundCaptureStart(s_cap[1], samples / 2, 1, false, true, false, SoundCaptureFormat_16Bit);
	s_capEndVbl = timing::vblanks() + (uint32_t)((uint64_t)samples * 60 / hz) + 6;
	return true;
}

bool captureBusy() { return s_cap[0] && timing::vblanks() < s_capEndVbl; }

bool captureSave(const char* path) {
	if (!s_cap[0] || captureBusy()) return false;
	soundCaptureStop(0);
	soundCaptureStop(1);
	stop(1);
	stop(3);
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
