#pragma once

// Top screen 3D renderer: ground plane, placeholder landmarks and the player
// entity. The world is drawn in 1/8-cell units under a scale matrix so vertex
// data stays inside the DS's signed 4.12 range (+-8); CJ draws in model space
// under its own entity matrix.
namespace renderer {

void init();
// Chase camera on player::g and draws the scene (reads game/player.h).
void draw();
int polyCount();
int vertCount();

}
