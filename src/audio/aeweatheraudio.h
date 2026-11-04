#pragma once

// CAEWeatherAudioEntity (Audio/Entities/AEWeatherAudioEntity.cpp): the
// outdoor bed. City noise (FRONTEND_GAME 0, a left and a right copy half a
// loop apart) and wind (29 / 30, gusting with CWeather::WindClipped and the
// camera height), the rain twin loops with drop one-shots on every swap,
// and thunder after a lightning burst (CWeather::Update ->
// AddAudioEvent(AE_THUNDER)).
namespace aeweather {

void preload();       // StaticInitialise: rain + the city / wind loops + thunder
void reset();         // StaticReset
void service();       // CAEWeatherAudioEntity::Service, once per frame in game
void thunder();       // AddAudioEvent(AE_THUNDER)
float rainVolume();   // m_sfRainVolume (dB), debug

}
