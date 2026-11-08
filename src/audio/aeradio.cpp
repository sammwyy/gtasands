#include "audio/aeradio.h"

#include <string.h>

#include "audio/aeambience.h"
#include "audio/aestream.h"
#include "game/cutscene.h"
#include "game/debugopts.h"
#include "game/menu/menu.h"
#include "game/player.h"
#include "game/vehicle.h"
#include "hud/hud.h"
#include "platform/timing.h"

namespace aeradio {

static const uint16_t k_songs[][31] = {
	{ 231, 238, 245, 252, 259, 266, 273, 280, 287, 294, 301, 308 },
	{ 365, 372, 379, 386, 393, 400, 407, 414, 421, 428, 435, 442, 449, 456, 463 },
	{ 513, 520, 527, 534, 541, 547, 554, 561, 564, 570, 577, 584, 591, 598, 605, 612, 619 },
	{ 827, 834, 841, 848, 855, 862, 869, 876, 883, 890, 897, 904, 911, 918, 925, 932, 939 },
	{ 981, 986, 991, 996, 1001, 1006, 1011, 1016, 1021, 1026, 1031, 1036, 1041, 1046, 1051, 1056 },
	{ 1111, 1118, 1125, 1132, 1139, 1146, 1152, 1158, 1164, 1169, 1176, 1183, 1190, 1195, 1202, 1208 },
	{ 1259, 1266, 1273, 1280, 1287, 1294, 1301, 1308, 1315, 1322, 1329, 1336, 1343, 1350, 1357 },
	{ 1399, 1406, 1413, 1420, 1427, 1434, 1441, 1448, 1455, 1462, 1469, 1476, 1483 },
	{ 1542, 1549, 1556, 1563, 1570, 1577, 1584, 1591, 1598, 1605, 1612, 1619, 1625, 1632, 1639, 1645 },
	{ 1706, 1713, 1720, 1727, 1734, 1741, 1748, 1754, 1761, 1768, 1775, 1782, 1787, 1790, 1796, 1803, 1809, 1814 },
	{ 1829, 1832, 1835, 1838, 1841, 1844, 1847, 1850, 1853, 1856, 1859, 1862, 1865, 1868, 1871, 1874,
	  1877, 1880, 1883, 1886, 1889, 1892, 1895, 1898, 1901, 1904, 1907, 1910, 1913, 1916, 1919 },
};
static const char* const k_names[] = { "Playback FM", "K-Rose", "K-DST", "Bounce FM", "SF-UR",
	"Radio Los Santos", "Radio X", "CSR 103.9", "K-Jah West", "Master Sounds 98.3", "WCTR", "RADIO OFF" };

static uint32_t s_durations[11][31], s_totals[11];
static uint8_t s_counts[11], s_loading;
static int s_car = -1, s_station = OFF, s_last = OFF;
static uint32_t s_failedTrack = ~0u, s_retryAt;
static uint32_t s_lastVblank, s_savedMs, s_savedGameMinutes;
static uint64_t s_ticks;
static bool s_on, s_ready, s_cheatActive, s_previewing;

bool initStep() {
	if (!s_loading) {
		s_ready = false;
		memset(s_durations, 0, sizeof s_durations);
		memset(s_totals, 0, sizeof s_totals);
		memset(s_counts, 0, sizeof s_counts);
		s_car = -1;
		s_failedTrack = ~0u;
		s_retryAt = 0;
		s_station = s_last = OFF;
		s_cheatActive = false;
		s_previewing = false;
		s_ticks = 0;
		s_lastVblank = timing::vblanks();
	}
	const unsigned i = s_loading++;
	if (!aestream::radioPack(i)) {
		if (i == 0) hud::log("radio: SD packs unavailable");
	} else {
		for (unsigned j = 0; j < 31 && k_songs[i][j]; j++) {
			const uint32_t ms = aestream::radioDurationMs(i, k_songs[i][j]);
			if (!ms) continue;
			s_durations[i][j] = ms;
			s_totals[i] += ms;
			s_counts[i] = j + 1;
		}
		s_ready |= s_totals[i] != 0;
	}
	if (s_loading == 11) {
		hud::log("radio: %s", s_ready ? "11 SD stations" : "no SD stations");
		s_loading = 0;
		return true;
	}
	return false;
}

void reset() {
	if (s_on) aestream::stop();
	s_on = false;
	s_station = OFF;
	s_car = -1;
	s_previewing = false;
}

int station() { return s_station; }

void selectStation(int station) {
	if ((s_car < 0 && !dbg::opts.alwaysRadio && !FrontEndMenuManager.radioPreviewActive()) ||
	    station < 1 || station > OFF) return;
	s_station = station;
	s_failedTrack = ~0u;
	s_retryAt = 0;
	if (veh::Vehicle* v = veh::get(s_car)) v->radioStation = s_station;
	hud::log("radio: %s", k_names[s_station - 1]);
}

void nextStation() {
	if (s_car < 0 && !dbg::opts.alwaysRadio) return;
	const int next = s_station >= OFF ? 1 : s_station + 1;
	FrontEndMenuManager.m_nRadioStation = next;
	selectStation(next);
}

bool service(bool paused, bool preview) {
	const uint32_t now = timing::vblanks();
	if ((!paused || preview) && !cutscene::active()) s_ticks += (uint32_t)(now - s_lastVblank);
	s_lastVblank = now;
	if (s_previewing && !preview) {
		if (s_on) aestream::stop();
		s_on = s_previewing = false;
		if (s_car < 0) s_station = dbg::opts.alwaysRadio ? FrontEndMenuManager.m_nRadioStation : OFF;
	}
	if (paused && !preview) {
		if (s_on) aestream::setFreqFactor(0.0f);
		return s_on;
	}
	s_previewing = preview;
	if (preview) s_station = FrontEndMenuManager.m_nRadioStation;
	const uint32_t ms = (uint32_t)(s_ticks * 1000 / 60);
	const int car = preview ? s_car : player::driving() ? player::vehicleId() : -1;
	const bool cheat = dbg::opts.alwaysRadio;
	if (car != s_car) {
		if (s_car >= 0) {
			s_last = s_station;
			s_savedMs = ms;
			s_savedGameMinutes = player::gameMinutesPassed();
		}
		s_car = car;
		if (car >= 0 && car < 8) {
			const int selected = FrontEndMenuManager.m_nRadioStation;
			veh::Vehicle* v = veh::get(car);
			const bool retune = FrontEndMenuManager.m_bRadioAutoSelect && s_last != OFF &&
				ms - s_savedMs < 60000 && player::gameMinutesPassed() - s_savedGameMinutes <= 300;
			s_station = v && v->radioStation ? v->radioStation : retune ? s_last : selected;
			if (s_station < 1 || s_station > OFF) s_station = 1;
			if (v) v->radioStation = s_station;
			FrontEndMenuManager.m_nRadioStation = s_station;
		} else if (!cheat) s_station = OFF;
	}
	if (!preview && cheat != s_cheatActive && car < 0)
		s_station = cheat ? FrontEndMenuManager.m_nRadioStation : OFF;
	s_cheatActive = cheat;
	if (!s_ready || s_station < 1 || s_station >= OFF || (car < 0 && !cheat && !preview) || car >= 8 ||
	    !s_totals[s_station - 1]) {
		if (s_on) aestream::stop();
		s_on = false;
		return false;
	}
	aestream::setFreqFactor(1.0f);
	const int st = s_station - 1;
	uint32_t position = ms % s_totals[st];
	for (unsigned i = 0; i < s_counts[st]; i++) {
		if (position < s_durations[st][i]) {
			const uint32_t id = k_songs[st][i];
			if (!s_on || aestream::track() != id) {
				if (id == s_failedTrack && (int32_t)(now - s_retryAt) < 0) return false;
				if (!s_on) aeambience::reset();
				s_on = aestream::play(id, -2.0f, false, false, position);
				if (!s_on) {
					s_failedTrack = id;
					s_retryAt = now + 60;
				}
			}
			return s_on;
		}
		position -= s_durations[st][i];
	}
	return false;
}

}
