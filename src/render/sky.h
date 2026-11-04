#pragma once

// Sky, celestial sprites, fog and weather effects on the top screen.
// Ports CClouds::RenderSkyPolys + CClouds::Render (moon, R* stars, low
// clouds, rainbow, shooting stars), CCoronas::DoSunAndMoon (sun core/corona +
// lens flare) and CWeather::RenderRainStreaks / AddSandStormParticles.
//
// DS tricks (no depth-write toggle, no additive blending):
// - Sky geometry goes through a projection whose z row is a constant fraction
//   of w, so every sky vertex lands at depth ~0.9998 regardless of distance:
//   behind the whole world, in front of the clear plane, never clipped. Sky
//   polys and sprites are tiny (2 units from the eye) to stay inside v16.
// - Additive sprites use luminance-as-alpha textures (pipeline `additive`)
//   and the additive colour is split into polygon alpha * normalised colour.
// - Translucent sprites need distinct polygon IDs to blend over each other
//   and manual translucent sort to keep the original layering.
namespace sky {

struct View {
	float camX, camY, camZ;   // eye, SA world coords
	float yaw, pitch;         // renderer chase camera (yaw 0 = north, pitch > 0 looks down)
};

// Loading-screen job: particle.txd sprites (coronastar, coronamoon, lunar,
// cloud1) into VRAM, ~3 KB. True when done.
bool loadStep();

// Sky + sprites, first thing in the frame. Also latches the frame's fog,
// clear colour and manual translucent sort (video::). Leaves the modelview
// matrix undefined; the projection is restored.
void draw(const View& v);
// Rain / sandstorm streaks, after the world (depth-tested against it). Uses
// the world projection; sets up its own modelview from the view.
void drawWeatherFx(const View& v);

}
