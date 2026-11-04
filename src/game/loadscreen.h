#pragma once

// CLoadingScreen port (LoadingScreen.cpp): splash artwork on the top screen
// with the bottom-left loading bar while the in-game assets load, plus the
// PC boot title screen (title_pc_US, the "copyright" splash in the OG code).
// Progress is driven by an internal step list; NewChunkLoaded on PC counted
// file chunks, here each init step reports its share instead.
namespace loadscreen {

void beginTitle(); // title_pc_US splash: intro -> fade in -> hold -> fade out -> menu
void begin();      // pick a random loadscN splash, start the init step list
bool update();     // run pending steps + timers; true when fully finished (faded out)
void draw();       // splash + bar (call inside the frame, after beginFrame)
void end();        // release the splash texture + stop the tune

}
