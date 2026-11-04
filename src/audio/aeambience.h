#pragma once

// CAEAmbienceTrackManager (Audio/Managers/AEAmbienceTrackManager.cpp): picks
// the ambience track for the sandstorm or the active audio zone and fades
// it in / out on the stream channel. The PC version also starts the radio
// for the club / shop zones (no radio yet) and the water track (no water).
namespace aeambience {

void reset();
void service(bool paused);   // once per frame in game
int wantedTrack();           // last requested track (-1 none), debug

}
