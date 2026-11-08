#include "audio/aestream.h"

#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets/ds_formats.h"
#include "audio/aeaudio.h"
#include "audio/aehardware.h"
#include "audio/aesoundmanager.h"
#include "hud/hud.h"
#include "mem/mem.h"
#include "platform/fs.h"
#include "platform/timing.h"

namespace aestream {

static constexpr uint32_t RING_SAMPLES = mem::budget::AUDIO_STREAM_RING / 2;
static constexpr uint32_t LEAD_SAMPLES = RING_SAMPLES * 3 / 4;   // decoded ahead of the play head
static constexpr uint32_t BUS_HZ = 33513982;                       // timing::ticks() rate

static const char* const k_packs[] = {
	"nitro:/audio/streams/ambience.dsst", "nitro:/audio/streams/aa.dsst",
	"nitro:/audio/streams/cutscene.dsst"
};
static const char* const k_radioPacks[] = { "ch", "co", "cr", "ds", "hc", "mh", "mr", "nj", "re", "rg", "tk" };
static const uint16_t k_radioFirst[] = { 231, 365, 513, 827, 981, 1111, 1259, 1399, 1542, 1706, 1829, 1922 };
static constexpr int BASE_PACKS = sizeof k_packs / sizeof k_packs[0];
static constexpr int NUM_PACKS = BASE_PACKS + sizeof k_radioPacks / sizeof k_radioPacks[0];

struct Pack {
	FILE* f;
	ds::StreamTrack* tracks;
	uint16_t n;
	bool seekable;
	bool tried;
};
static Pack s_packs[NUM_PACKS];

// IMA-ADPCM (the DS mixer's variant: nibbles low first, clamp to +-0x7FFF)
static const int16_t k_step[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
	107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
	5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
	27086, 29794, 32767 };
static const int8_t k_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// Decoder position; the read buffer is separate so a stopped track's state
// can be kept for the resume without a copy of the buffer.
struct Decoder {
	FILE* f;
	uint32_t dataPos;        // file offset of the nibbles (after the 4-byte ADPCM header)
	uint32_t numSamples, done;
	int32_t pred0, pred;
	int idx0, idx;
};

static Decoder s_dec;
static uint8_t s_buf[2048];          // ~0.25 s of 16 kHz ADPCM per read
static uint16_t s_bufLen, s_bufPos;
static bool s_hi;                    // next nibble is the high one of s_buf[s_bufPos]
static int16_t* s_ring;
static uint32_t s_trackId = ~0u, s_lastTrack = ~0u;
static uint32_t s_rate, s_timer;
static uint32_t s_written;           // samples written into the ring since the start
static uint32_t s_playedBase;        // samples played before the last timer change
static uint64_t s_playedTicks;       // CPU ticks since then
static uint32_t s_lastTick;
static bool s_loop, s_on;
static bool s_cutscene, s_radio;
static float s_volDb, s_freq = 1.0f, s_musicDb;
static uint16_t s_hwVol;
static Stats s_stats = { 0, 0, 0, ~0u };
// resume point of the last stopped track
static Decoder s_saved;
static bool s_haveSaved;

static Pack* openPack(int i) {
	Pack& p = s_packs[i];
	if (p.tried) return p.f ? &p : nullptr;
	p.tried = true;
	char path[112];
	const char* name = i < BASE_PACKS ? k_packs[i] : nullptr;
	if (!name) {
		const char* root = fs::radioRoot();
		if (!root) return nullptr;
		snprintf(path, sizeof path, "%s/%s.dsst", root, k_radioPacks[i - BASE_PACKS]);
		name = path;
	}
	p.f = fopen(name, "rb");
	ds::StreamPackHeader h;
	if (p.f && fread(&h, sizeof h, 1, p.f) == 1 && h.magic == DS_FOURCC('D', 'S', 'S', 'T') &&
	    h.numTracks <= 256 && h.version == (i < BASE_PACKS ? 1 : 2)) {
		p.seekable = h.version == 2;
		const unsigned stride = p.seekable ? sizeof(ds::StreamTrackSeek) : sizeof(ds::StreamTrack);
		p.tracks = (ds::StreamTrack*)mem::alloc(h.numTracks * stride, mem::TAG_AUDIO);
		if (p.tracks && fread(p.tracks, stride, h.numTracks, p.f) == h.numTracks) p.n = h.numTracks;
	}
	if (!p.n && p.f) {
		fclose(p.f);
		p.f = nullptr;
		mem::free(p.tracks);
		p.tracks = nullptr;
	}
	return p.f ? &p : nullptr;
}

static const ds::StreamTrack* findTrack(const Pack& p, uint32_t id) {
	for (unsigned k = 0; k < p.n; k++) {
		const ds::StreamTrack* t = p.seekable
			? (const ds::StreamTrack*)((const ds::StreamTrackSeek*)p.tracks + k) : p.tracks + k;
		if (t->trackId == id) return t;
	}
	return nullptr;
}

bool radioPack(int station) {
	return (unsigned)station < sizeof k_radioPacks / sizeof k_radioPacks[0] && openPack(BASE_PACKS + station);
}

uint32_t radioDurationMs(int station, uint32_t trackId) {
	if (!radioPack(station)) return 0;
	Pack& p = s_packs[BASE_PACKS + station];
	const ds::StreamTrack* t = findTrack(p, trackId);
	ds::SoundHeader h;
	if (!t || fseek(p.f, t->offset, SEEK_SET) || fread(&h, sizeof h, 1, p.f) != 1 ||
	    h.magic != DS_FOURCC('D', 'S', 'N', 'D') || !h.sampleRate) return 0;
	return (uint32_t)((uint64_t)h.numSamples * 1000 / h.sampleRate);
}

// Positions the file + buffer at sample d.done (nibble d.done of the track).
static void seekTo(Decoder& d) {
	fseek(d.f, d.dataPos + d.done / 2, SEEK_SET);
	s_bufLen = (uint16_t)fread(s_buf, 1, sizeof s_buf, d.f);
	s_bufPos = 0;
	s_hi = (d.done & 1) != 0;
}

static void restart(Decoder& d) {
	d.done = 0;
	d.pred = d.pred0;
	d.idx = d.idx0;
	seekTo(d);
}

// Decodes n samples into out (IMA-ADPCM, the mixer's variant); past the end
// it loops or writes silence. Runs a buffer's worth at a time with the
// state in registers: ~16k samples/s is the whole cost of a stream.
static void decode(Decoder& d, int16_t* out, uint32_t n) {
	while (n) {
		if (d.done >= d.numSamples) {
			if (!s_loop) break;
			restart(d);
		}
		if (s_bufPos >= s_bufLen) {
			s_bufLen = (uint16_t)fread(s_buf, 1, sizeof s_buf, d.f);
			s_bufPos = 0;
			if (!s_bufLen) break;
		}
		uint32_t m = (uint32_t)(s_bufLen - s_bufPos) * 2 - (s_hi ? 1 : 0);
		if (m > n) m = n;
		if (m > d.numSamples - d.done) m = d.numSamples - d.done;
		const uint8_t* src = s_buf + s_bufPos;
		int32_t pred = d.pred;
		int idx = d.idx;
		bool hi = s_hi;
		for (uint32_t i = 0; i < m; i++) {
			int code;
			if (hi) code = *src++ >> 4;
			else code = *src & 15;
			hi = !hi;
			const int step = k_step[idx];
			int diff = step >> 3;
			if (code & 4) diff += step;
			if (code & 2) diff += step >> 1;
			if (code & 1) diff += step >> 2;
			pred += code & 8 ? -diff : diff;
			if (pred > 32767) pred = 32767;
			else if (pred < -32767) pred = -32767;
			idx += k_index[code & 7];
			if (idx < 0) idx = 0;
			else if (idx > 88) idx = 88;
			out[i] = (int16_t)pred;
		}
		d.pred = pred;
		d.idx = idx;
		s_hi = hi;
		s_bufPos = (uint16_t)(src - s_buf);
		d.done += m;
		out += m;
		n -= m;
	}
	if (n) memset(out, 0, n * 2);
}

static uint32_t playedSamples() {
	return s_playedBase + (uint32_t)(s_playedTicks / (2ull * s_timer));
}

static void writeAhead(uint32_t upto) {
	while (s_written < upto) {
		const uint32_t at = s_written % RING_SAMPLES;
		uint32_t n = upto - s_written;
		if (n > RING_SAMPLES - at) n = RING_SAMPLES - at;
		decode(s_dec, s_ring + at, n);
		if (s_radio)
			for (uint32_t i = 0; i < n; i++) {
				const int v = s_ring[at + i];
				const int mag = v < 0 ? -v : v;
				const int boosted = mag <= 18000 ? mag + mag / 2 : 27000 + (mag - 18000) / 4;
				s_ring[at + i] = (int16_t)(v < 0 ? -boosted : boosted);
			}
		DC_FlushRange(s_ring + at, n * 2);   // the mixer DMA reads RAM
		s_written += n;
		s_stats.decoded += n;
	}
}

static unsigned hwVolume() {
	return s_freq <= 0.0f ? 0 : aesound::volumeFromDb(s_volDb + (s_cutscene ? 0.0f : s_musicDb));
}

static unsigned hwHz() {
	const float f = s_freq > 0.0625f ? s_freq : 0.0625f;
	return (unsigned)((float)s_rate * f);
}

bool play(uint32_t trackId, float volumeDb, bool loop, bool resume, uint32_t startMs) {
	if (s_on && s_trackId == trackId && !startMs) return true;
	stop();
	if (!resume) s_haveSaved = false;
	Pack* pack = nullptr;
	const ds::StreamTrack* t = nullptr;
	const bool radio = trackId >= k_radioFirst[0] && trackId < k_radioFirst[11] && trackId != 706 && trackId != 708;
	int first = 0, last = BASE_PACKS;
	if (radio) {
		for (int i = 0; i < 11; i++)
			if (trackId >= k_radioFirst[i] && trackId < k_radioFirst[i + 1]) {
				first = BASE_PACKS + i;
				last = first + 1;
				break;
			}
	}
	for (int i = first; i < last && !t; i++) {
		Pack* p = openPack(i);
		if (p && (t = findTrack(*p, trackId))) pack = p;
	}
	if (!t) {
		hud::log("\x1b[33mstream: track %lu missing\x1b[39m", (unsigned long)trackId);
		return false;
	}
	ds::SoundHeader h;
	uint8_t adpcmHdr[4];
	if (fseek(pack->f, t->offset, SEEK_SET) != 0 || fread(&h, sizeof h, 1, pack->f) != 1 ||
	    h.magic != DS_FOURCC('D', 'S', 'N', 'D') || h.format != ds::SND_ADPCM || fread(adpcmHdr, 4, 1, pack->f) != 1)
		return false;
	if (!reserve()) return false;
	if (s_haveSaved && s_lastTrack == trackId) {
		s_dec = s_saved;     // m_PrevAmbiencePlayTimeMs: carry on from where it stopped
		s_dec.f = pack->f;
		seekTo(s_dec);
	} else {
		s_dec = {};
		s_dec.f = pack->f;
		s_dec.dataPos = t->offset + sizeof h + 4;
		s_dec.numSamples = h.numSamples;
		s_dec.pred0 = (int16_t)(adpcmHdr[0] | adpcmHdr[1] << 8);
		s_dec.idx0 = adpcmHdr[2] > 88 ? 88 : adpcmHdr[2];
		restart(s_dec);
	}
	if (startMs && pack->seekable) {
		const auto* st = (const ds::StreamTrackSeek*)t;
		uint32_t sample = (uint32_t)((uint64_t)startMs * h.sampleRate / 1000);
		if (sample >= h.numSamples) sample = h.numSamples ? h.numSamples - 1 : 0;
		const uint32_t ck = sample / 8192;
		uint8_t state[4];
		if (ck < st->seekCount && fseek(pack->f, st->seekOffset + ck * 4, SEEK_SET) == 0 &&
		    fread(state, sizeof state, 1, pack->f) == 1) {
			s_dec.done = ck * 8192;
			s_dec.pred = (int16_t)(state[0] | state[1] << 8);
			s_dec.idx = state[2] > 88 ? 88 : state[2];
			seekTo(s_dec);
			int16_t discard[256];
			while (s_dec.done < sample) {
				const uint32_t n = sample - s_dec.done < 256 ? sample - s_dec.done : 256;
				decode(s_dec, discard, n);
			}
		}
	}
	s_loop = loop;
	s_cutscene = trackId == 706 || trackId == 708;
	s_radio = radio;
	s_rate = h.sampleRate;
	s_volDb = volumeDb;
	s_trackId = trackId;
	s_stats.trackId = trackId;
	s_written = 0;
	writeAhead(LEAD_SAMPLES);
	s_timer = soundTimerFromHz(hwHz());
	s_playedBase = 0;
	s_playedTicks = 0;
	s_lastTick = timing::ticks();
	s_hwVol = (uint16_t)hwVolume();
	aehw::playRing(aehw::STREAM_CHANNEL, s_ring, RING_SAMPLES * 2, s_hwVol, 64, hwHz());
	s_on = true;
	return true;
}

bool reserve() {
	if (!s_ring) s_ring = (int16_t*)mem::alloc(RING_SAMPLES * 2, mem::TAG_AUDIO, 32);
	return s_ring != nullptr;
}

void stop() {
	if (!s_on) return;
	aehw::stop(aehw::STREAM_CHANNEL);
	// remember the decoder where the listener left it (roughly: the ring's
	// lead is dropped, the next start re-decodes from the write head)
	s_saved = s_dec;
	s_haveSaved = true;
	s_lastTrack = s_trackId;
	s_on = false;
	s_trackId = ~0u;
	s_stats.trackId = ~0u;
	s_stats.serviceUs = 0;
}

bool playing() { return s_on; }
uint32_t track() { return s_trackId; }
void setVolume(float db) { s_volDb = db; }
void setFreqFactor(float f) { s_freq = f; }
void setMusicVolume(int vol64) { s_musicDb = vol64 <= 0 ? ae::VOLUME_SILENCE : 20.0f * log10f((float)vol64 / 64.0f); }
int playTimeMs() { return s_on && s_rate ? (int)((uint64_t)playedSamples() * 1000u / s_rate) : -1; }

void service() {
	if (!s_on) return;
	const u32 t0 = timing::ticks();
	s_playedTicks += (u32)(t0 - s_lastTick);
	s_lastTick = t0;
	const unsigned timer = soundTimerFromHz(hwHz());
	if (timer != s_timer) {
		s_playedBase = playedSamples();
		s_playedTicks = 0;
		s_timer = timer;
		aehw::setFreq(aehw::STREAM_CHANNEL, hwHz());
	}
	const uint32_t played = playedSamples();
	if (played + 64 > s_written) {
		// the play head caught up (a long stall): skip ahead, it will click once
		s_stats.underruns++;
		s_written = played;
	}
	writeAhead(played + LEAD_SAMPLES);
	const unsigned vol = hwVolume();
	if (vol != s_hwVol) {
		aehw::setVolume(aehw::STREAM_CHANNEL, vol);
		s_hwVol = (uint16_t)vol;
	}
	s_stats.serviceUs = timing::usSince(t0);
}

const Stats& stats() { return s_stats; }

}
