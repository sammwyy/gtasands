#pragma once

// Screen layout:
//   top    = main engine, 3D (game world render)
//   bottom = sub engine, 2D (HUD: radar, weapon, money, touch UI)
//
// VRAM plan:
//   A, B -> 3D textures (256 KB)      D -> free (more textures / capture)
//   C    -> sub BG (HUD, 128 KB)      E -> 3D texture palettes
//   F..I -> free (sprites / ext palettes)
namespace video {

void init();
void beginFrame();
void endFrame();

}
