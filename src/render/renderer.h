#pragma once

// Top screen 3D renderer. Placeholder scene until the RW -> DS GX backend
// (gta-reversed RenderWare calls mapped onto the DS geometry engine) lands.
namespace renderer {

void init();
void draw(float camYaw, float camPitch, float camDist);
int polyCount();
int vertCount();

}
