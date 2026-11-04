#pragma once

namespace intro {

// Plays a DVID video (paletted frames + interleaved PCM16) fullscreen on the
// top screen with streamed audio. Returns false if the file is missing.
// Any button skips. Restores 3D video mode before returning.
bool play(const char* path);

}
