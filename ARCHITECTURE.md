# gtasads architecture

Reference map of the code base so work can start without re-reading every file.
Build/run/test commands and working rules live in `AGENTS.md`; this file is the
"what is where and how does it work" part. Keep it current when a system changes.

## 1. Big picture

```
PC install --tools/assets.py sync--> game/ --tools/assets.py build--> nitrofs/ --ndstool--> gtasads.nds
                                                                         |  (or sd:/gtasads/ with FAT=1)
ARM9 runtime (src/):  boot -> intro movies -> title -> frontend menu -> loading jobs -> INGAME loop
```

- Top screen = main engine, 3D (`render/`). Bottom screen = sub engine, 2D: either the
  software-rendered GTA HUD (`game/hud/`) or the text debug console (`hud/debugui`), SELECT toggles.
- Code is a port of gta-reversed (`third_party/gta-reversed/source/game_sa/`, reference only,
  never compiled). Same class/function names in comments (`CTimeCycle::CalcColoursForPoint`) so
  the original can be found by grep. RenderWare calls are replaced by our GX code + DS formats.

## 2. Source layout (`src/`, every subdir auto-discovered, object basenames must be unique)

| Path | What |
|---|---|
| `main.cpp` | AppState machine (TITLE/MENU/LOADING/INGAME), per-frame order (see 3.) |
| `platform/video.*` | screen/VRAM setup, projection constants (`FOV_Y`, `NEAR_PLANE`, `FAR_PLANE`, `TAN_HALF_FOV_*`), per-frame render environment (fog, clear colour, manual translucent sort) applied in vblank, `depth15()` z-buffer helpers, DEBUG `requestCapture()` screenshots |
| `platform/input.*` | key/touch polling, edges (`consumeEdges`) |
| `platform/fs.*` | NitroFS or `nitro:`->`fat:` redirect (FAT=1), debug `fsok` marker |
| `platform/timing.*` | timers 0+1, `vblanks()`, `ticks()`, `usSince()`, per-section CPU (`SEC_3D/SIM/HUD`) |
| `mem/` | memory manager (budget.h regions, TLSF, scratch stack, perm arena, `Pool<T>`) |
| `assets/ds_formats.h` | every binary layout (mirrors the Python writers, static_asserts) |
| `assets/pack.*` | DSPK reader (sorted FNV-1a TOC) |
| `assets/texture.*` | DTEX -> VRAM, `tex::Group` (FRONTEND / GAME / WORLD) for bulk unloads |
| `assets/txdfile.*` | loose DTXD read per entry into scratch (never whole) |
| `assets/model.*` | DMDL parsed view (`mdl::Model`, geometries, skin) |
| `render/renderer.*` | top-screen frame: chase camera, sky, CJ, streamed map (`world::draw`), placeholder ground/boxes (only without world.dwld or REND toggle), weather FX |
| `render/sky.*` | sky polys, sun/moon/stars/clouds/rainbow/flares, fog setup, rain/sand streaks |
| `render/gxlist.*` | packed GX display list builder + DMA `callList` |
| `render/sprite2d.*` | 2D quads on the 3D engine (frontend, loading screen), virtual 640x480 |
| `render/overlay.*` | debug geometry on top of everything with a 1-px dark outline (skeleton, collision), section 6c |
| `game/player.*` | player state snapshot + CClock + controls glue, chase camera, weapons, jump/fight |
| `game/pedmove.*` | locomotion (PlayerControlZelda, SetRealMoveAnim-style blends), reusable for AI |
| `game/pad.*` | CPad-lite (PC keyboard mode mapping) |
| `game/cj.*` | CJ assembly (CClothesBuilder), GX skinning, anim slots, ped light tint |
| `game/anim.*`, `game/animstore.*` | DANM sampling/blending; clip streaming + LRU |
| `game/timecycle.*` | CTimeCycle/CColourSet port (timecyc.dat) |
| `game/weather.*` | CWeather port |
| `game/streaming.*` | open packs (`g_models` gta3, `g_player`, `g_anim`) |
| `game/world.*` | streamed map: DWLD sector window, map model/texture streaming, GX list compile, culling + draw (section 6b) |
| `game/collision.*` | map collision: per-model COL records streamed from world.dwld, near list, segment cast / ground probe / sphere push, debug overlay (section 6c) |
| `game/loadscreen.*` | CLoadingScreen + resumable init jobs |
| `game/zones.*` | info.zon -> zone GXT names |
| `game/sfx.*`, `game/intro.*` | frontend SFX/loading tune; DVID movie player |
| `game/menu/` | CMenuManager port (screens table verbatim) |
| `game/text/` | CText (GXT), CFont |
| `game/hud/` | CHud/CRadar ports on a CPU framebuffer (`subfb`), `hudtex`, `hudfont` |
| `game/debugopts.*` | `dbg::opts` runtime switches edited from the debug UI |
| `hud/hud.*` | bottom-screen owner: GAME (chud) vs DEBUG (console), log ring, DEBUG hudok marker |
| `hud/debugui.*` | touch tabs LOG / PERF / MEM / GAME / REND / SKY |

Other dirs: `tools/` (pipeline, `hosthud/` host HUD harness, `dldi/`, `dsshot.py` DEBUG screenshot -> PNG), `game/` (gitignored PC
files), `nitrofs/` (gitignored output), `build/` (objects, `assets_cache/`, reports, previews).

## 3. Frame loop (`main.cpp`)

```
timing::frameStart; input::poll
state transitions (title -> menu -> loading -> ingame), SELECT = toggle bottom screen, START = pause menu
video::beginFrame()                 // resets render env: no fog, default clear, auto sort
  menu | loadscreen::draw | renderer::draw      -> SEC_3D
INGAME && !menu: col::update; player::update; weather::update; timecycle::update; world::update -> SEC_SIM
hud::draw()  (CPU framebuffer in RAM / debug console)                  -> SEC_HUD
timing::frameEnd
video::endFrame()   // glFlush(manual sort?) ; swiWaitForVBlank ; apply fog/clear registers
world::vblank()     // staged map textures -> VRAM while the 3D engine is idle (any state)
hud::present()      // DMA bottom framebuffer to VRAM C inside vblank
```
Rendering happens before the simulation step, so draw uses last frame's sim/timecycle state.
Simulation is fixed-step 60 Hz (one `updateMove` per elapsed vblank, max 4).

## 4. Render engine (top screen)

### Coordinates and ranges
- SA world: x east, y north, z up, metres. GL: x east, y up, z south -> `GL = (x, z, -y)`.
- DS matrices are s20.12 and `v16` vertices are 4.12 (+-8). Never emit absolute SA coords:
  everything is drawn in **player-local** space (origin = player x,y at ground z). Larger things
  use a scale matrix (ground cells: `glScalef(8)`, weather FX: `glScalef(4)`).
- Chase camera (`renderer::draw`): yaw 0 looks north, eye south of CJ on a sphere of
  `player::camDist()` (= `dbg::opts.camDist` shortened by the camera collision) around
  `viewZ() + CAM_LOOK_H`; pitch > 0 looks down. View = `Rx(pitch) Ry(-yaw) T(-eye)`.
  The GL origin is (player x, y, `player::viewZ()`): CJ's feet z, smoothed.
  Camera basis in GL world space: right `(cos y,0,-sin y)`, up `(-sin p sin y, cos p, -sin p cos y)`,
  forward `(-cos p sin y, -sin p, -cos p cos y)` (see `sky::draw`).
- Projection: `gluPerspective(60, 4:3, 0.1, 200)` (`video::` constants; `sprite2d::endFrame`
  restores it). Z-buffering (not W): depth15 = z/w * 0x4000 + 0x3FFF, very non-linear.

### Draw order in `renderer::draw`
1. `sky::draw(view)` - latches fog/clear/manual sort, draws sky polys + sprites with
   constant-depth projections (see 5.), restores the projection.
2. world light (`timecycle::worldLight`, darkened by `WetRoads`) for map/placeholder geometry,
   ped light (`timecycle::pedLight`) -> `cj::setLight`.
3. view matrix, `cj::draw` (first, so the polygon budget never drops CJ; skeleton overlay inside),
   `col::drawDebug` (REND collision overlay, before the map so the map budget yields to it),
   light0 = world light, `world::draw` (section 6b), placeholder ground grid + boxes only if there
   is no world, then `sky::drawWeatherFx`.

### GX display lists (CJ)
- Each CJ part is compiled once (`cj.cpp buildPart`) into a packed list: POLY_ATTR, COLOR,
  BEGIN, then per vertex MTX_RESTORE (when the bone slot changes), TEXCOORD, VTX_16.
- Matrix stack slots: bones from slot 30 down (max 16 per part), entity matrix in slot 14,
  `glPushMatrix` uses the bottom. Per frame the CPU only computes `entity*world*invBone` per bone
  and stores it; the list is DMA'd (`gx::callList`, FIFO DMA, waits for other DMA channels).
- `gx::ListBuilder::words()` gives the index of the last parameter -> lists can be patched in place
  (CJ material colours for lighting: `colorAt[]`, then `DC_FlushRange` of the word).
- Skinning = one dominant bone per vertex. After the pose blend, `shoulderBoneRotation` is applied.
- CJ (and the map) are CCW-front like RW (checked on the dffs): `POLY_CULL_BACK`, which halved CJ
  from ~1200 to ~450 drawn polys. Sky/sprites stay `CULL_NONE`.
- Matrix slot 13 holds the view while the map draws (`MATRIX_RESTORE` + `MATRIX_MULT4x3` per
  instance), slot 12 while the collision overlay draws; CJ uses 14..30.
- Lists that write `TEXIMAGE_PARAM`/`PLTT_BASE` themselves (map) must leave both registers at 0 and
  call `glBindTexture(0, 0)` afterwards: libnds caches the bound texture and skips redundant binds.

### Translucency / hardware rules worth remembering
- Polygons with alpha 1..30 or an A3I5/A5I3 texture are translucent: drawn after opaque, do not
  write depth (POLY_ATTR bit 11 off), and a translucent pixel is NOT drawn over a translucent pixel
  with the same polygon ID -> give overlapping sprites different `POLY_ID`s.
- Default translucent order is hardware Y-sort; `video::setManualSort(true)` (renderer does it)
  makes it submission order.
- DISP3DCNT (fog enable), fog table/colour/offset and clear colour are read while the engine
  renders the previous flush -> never toggle them mid-frame; use `video::setFog/setClearColor`
  (written right after the vblank in `endFrame`).
- Fog needs `POLY_FOG` per polygon (ground, boxes, CJ lists have it; sky/sprites do not).
- No additive blending on DS. PC additive sprites are converted by the pipeline
  (`texture.additive`: alpha = max(r,g,b), colour normalised) and drawn with polygon alpha =
  max channel and normalised vertex colour (`sky.cpp additive()`), ~ dst + c over dark skies.
- A triangle with a repeated vertex renders as a 1-px line (skeleton overlay, rain, streaks);
  polygon alpha 0 = wireframe (collision overlay: one polygon per outlined triangle/quad).
- Budget: 2048 polys / 6144 verts per frame. Grove Street spawn: sky ~100 + CJ ~450, map up to
  `dbg::opts.worldPolys` (1800 total), weather FX after it.

## 5. Time cycle, weather and sky

- **Clock** (`player.cpp`, CClock): 60 vblanks = 1 game minute at `dbg::opts.clockRate` 1
  (0 = frozen, 10, 60). `player::clockSeconds()` (0..59), `minutesToday()` (fractional),
  `setClock(h, m)`. New game 12:00, day 1/1.
- **CWeather** (`game/weather.*`): state in `weather::g`. Region from position every 16 frames
  (LA/SF/LV/DESERT/DEFAULT, `findRegion` = original boxes); 64-entry lists per region (bytes,
  verbatim from `Data/Weather.def.h`). `interpolation = (sec/60 + min)/60`; when it wraps (new
  hour) old = new and new = forced type or the next list entry. Derived each frame: rain,
  sandstorm (step_to with time-varying intensity), cloudCoverage, foggyness(SF), extraSunnyness,
  rainbow, sunGlare, heatHaze, wetRoads, wind/windClipped/windDir (gust tables), lightning
  bursts (rainy old+new), traffic light brightness, rain particle strength.
  API: `forceWeather` (next hour), `forceWeatherNow`, `releaseWeather`, `debugFlash`.
- **CTimeCycle** (`game/timecycle.*`): `timecyc.dat` = 23 weathers x 8 samples
  (0,5,6,7,12,19,20,22 h), 52 columns, parsed by a loading job (one weather per slice, PERM
  ~10 KB, parsed once per boot; short lines keep the previous values like the original).
  `Colours` is a flat byte struct + s16 farClip/fogStart so blends are an 8-bit integer loop:
  time blend per weather -> LA smog fades into the clear type with camera altitude -> weather
  blend. Also `vectorToSun()` (one turn per day), below-horizon grey, `dayNightBalance()`,
  `pedLight()` (GetLightingFromCol: avg ambient + street surface light day 15/night 5, times
  amb_obj + 0.75*illumination), `worldLight()`. Lightning flash -> white horizon/fog, full ped light.
  Not ported: timecycle boxes, extra colours, underwater/tunnel, post-FX.
- **Sky** (`render/sky.*`):
  - Constant-depth projection trick: z row = -k * z_eye, w = -z_eye, so every vertex has
    z/w = k (4095/4096 for sky polys, 4094/4096 for sprites): behind all world geometry
    (anything nearer than ~160 m), in front of the clear plane, never far-clipped. Geometry is
    placed 2 units from the eye (sizes in v16 range), perspective makes distance irrelevant.
  - `renderSkyPolys` = CClouds::RenderSkyPolys (top->horizon gradient, horizon band, below-horizon
    grey, ceiling, floor) oriented by the camera heading.
  - Sprites (coronastar/coronamoon/lunar/cloud1 from `models/particle.txd`, ~3 KB VRAM):
    moon (00:00-07:20, phase via the lunar mask drawn in sky colour), R* logo stars (22-06),
    12 low clouds, rainbow, shooting star (0-5 AM, extra sunny), sun core + corona (sizes from
    timecyc x10 values / (farClip*0.95)) and the 26-sprite sun lens flare. Sizes convert from
    CSprite::CalcScreenCoors units with `SPRITE_K = 2*tan(hfov/2)*dist`.
  - Fog: linear fogStart..farClip scaled to the DS view distance `dbg::opts.fogView` (default
    140 m), colour = sky bottom, 32-entry table built in depth15 space only when inputs change.
  - Weather FX: fixed-point (1/256 m) world-space drops in a box wrapping around the camera,
    gravity + wind, motion streaks, ground splashes; sandstorm dust uses the same loop.
    One BEGIN, one polygon ID, <= 72 lines + splashes.
- **Debug SKY tab**: clock (+/-h, rate || x1 x10 x60), weather old>new %, region, forced, pick +
  next hour/now/auto, presets (sun/cld/rain/fog/sand), live weather values, sky colours, fog view
  +/-, ped light, toggles sky/sprites/fog/weather fx/ped light, lightning test.

## 6. Player, camera, animation

- `player::update`: pad -> fixed 60 Hz steps of `updateMove` (aim, PlayerControlZelda via
  `pedmove::control`, sprint energy, weapons, `updateAction` for jump/punch/land state machine,
  root motion from `cj::rootDelta` rotated by heading) + `collide()` (section 6c), then `viewZ`
  smoothing and the camera collision, clock tick, stats, zone lookup. `g.z` = CJ's feet (the
  ped origin of the original is 1 m higher: the old SPAWN_Z 13.5 is the feet at 12.35).
- Camera (keyboard mode): touch drag orbits (yaw/pitch). `followCam` (beta speed spring) swings
  behind CJ while he moves, but only when `camFollows()`: with stick input, only if the stick has
  no backward component (walkUD >= 0); with no input, if |heading - camYaw| < 1.9 rad. Stick back
  makes CJ turn and walk at the camera, which does not spin. Never decide this from CJ's heading
  while the stick is held: the stick is camera-relative, so a turning camera drags the target
  heading and CJ walks in circles (feedback loop).
- Locomotion details: `pedmove.*` (blend = |stick|/60, idle/walk/run/sprint weights,
  phase-synced cycles, run_stop/R). Anim clips: `animstore` streams per clip from DANM v2
  blocks, refcounted, LRU over `budget::ANIM_CACHE_BYTES`; 27 on-foot clips are pinned (~100 KB).
- CJ assembly follows CClothesBuilder: torso clothing dff is the whole torso (+arms/neck),
  textured with player_torso + clothing composite (PlaceTextureOnTopOfTexture); hands share it.
  Outfit presets `cj::setOutfit`, fat/muscle scale bones and pick the motion group.

## 6b. Streamed map (`game/world.*`)

CWorld + CStreaming + CRenderer, heavily reduced. CFileLoader runs offline (section 10): the
runtime only reads `data/world.dwld` (DWLD, `ds_formats.h`).
- **Data in RAM**: header, 8 B/model (draw distance, radius, tobj hours), the WorldTxd table
  (name + txdp parent), `slotOf[model]`. Model names are read (36 B) when a model loads.
- **Sector window**: 50 m sectors within `fogView + 40` m of the current sector's centre
  (re-evaluated only when the player crosses a sector edge, <= 2 sector reads/frame). Instances are
  32 B, rotation rows pre-converted to GL axes on load; each sector keeps a bounding sphere.
- **Scan** (in `draw`, ~1.2 ms for ~600 instances): sector sphere vs range/frustum, then per
  instance: tobj hour, distance (`d - radius < view && d < drawDist + radius`; no LODs yet, so HD
  models stay until the fog), sphere vs frustum (fixed point, 1/16 m), slot state. Not-loaded
  models go to an 8-entry nearest-first want list (out of view = lower priority). Drawn or
  within 40 m = protected from LRU.
- **Model streaming** (`world::update`, ~3 ms budget, 2 starts/frame): `startLoad` reads the DMDL
  into STREAM and requests its textures; textures are found in the model TXD or its txdp parents
  (4-entry TXD directory cache, entries read alone) and staged in RAM; `world::vblank` uploads
  <= 24 KB per vblank (evicting unreferenced textures LRU over `WORLD_TEX_VRAM`) and records
  TEXIMAGE_PARAM (+repeat) / PLTT_BASE. Once all textures are in, `compile()` builds one GX list.
- **compile()**: model-wide s16 positions (scale 2^e folded into the instance matrix), per
  material POLY_ATTR (alpha, cull back, fog, light0) + texture registers, triangle list with
  TEXCOORD (shifted per triangle by whole repeats so 12.4 texcoords stay in range) and the
  lighting trick: vertex colour (prelit * material colour) as AMBIENT + constant NORMAL with
  diffuse/specular/emission 0 -> GE outputs prelit * light0, light0 = world light per frame (no
  per-vertex CPU pass for the time of day). Emitted only when the colour changes. ~2.5 KB/model.
- **Draw**: 8 distance buckets near -> far, stop when the hardware polygon count + tris/2 passes
  `dbg::opts.worldPolys`. GE-bound: each `callList` waits for its DMA (~100 us per model).
- **Eviction**: slots LRU (`WORLD_MODEL_SLOTS`, `WORLD_LIST_BYTES`), textures refcounted + LRU.
  `world::suspend()` drops all lists/textures when the pause menu opens (VRAM); they stream back.
- **Preload** (loading job): sectors + every model in range around the player, no frustum, until
  idle or 10 s.
- Stats: PERF tab rows 15-18 (collision 19-20), `world:` / `col:` lines in hudok, REND tab toggle + poly budget.
- Grove Street numbers: ~110 models / 600 instances in the window, ~70 drawn, ~1600 polys,
  lists ~850 KB, map textures ~200 KB VRAM; draw ~8 ms (scan 1.2) -> 30 fps with the 8 ms HUD.

## 6c. Collision (`game/collision.*`, `render/overlay.*`)

CCollision / CColStore / CWorld::ProcessLineOfSight reduced to an on-foot player + camera.
- **Data** (offline, `world.py`): the COL of every DWLD model (first definition in the gta3.img
  `.col` archives), model space, 1/128 m s16 ("cu"). Faces re-wound to CCW-front
  (`(c-a)x(b-a)` in COL), unit normal 4.12 per face, degenerate ones dropped, then a BVH (median
  split, <= 6 faces per leaf, 16 B nodes); boxes + spheres kept as they are; per-model radius in
  the index. Grove box: 269 models, 24.7k faces, ~690 KB. Layout `ds::WorldCol*` (DWLD v4).
- **Runtime**: index resident (12 B/model). `col::update` (before `player::update`) rebuilds a
  near list every 8 frames / 2 m moved / cache change: instances within `GATHER_M` 14 m, their
  transform copied (sectors may be freed by world::update) + a world AABB of the record bbox.
  Records within `STREAM_M` 48 m stream in nearest first (1.5 ms/frame, anything within 14 m
  even past the budget), LRU over `COL_CACHE_BYTES` 192 KB. `pending()` = something near still
  missing or the sector window still loading: the player must not start falling then.
- **Queries** (integer, model space, entry AABB -> model bbox -> BVH -> face bbox): `castSegment`
  (single sided: hits faces it crosses from the front, boxes by slabs, spheres in float),
  `groundAt` (vertical cast), `pushSphere` (faces with |n.z| < 0.6 + boxes + spheres, closest
  point, push out horizontally, Gauss-Seidel per contact). 64-bit divides go through the
  hardware divider (`div64`, libgcc's takes ~1000 cycles). ~0.4 ms/frame for 2 sim steps.
- **Player** (`player.cpp collide()` after every step): the three ms_colModelPed1 spheres
  (0.35 m at feet + 0.8/1.2/1.6) push CJ out of walls; feet follow the highest floor from
  `STEP_UP` 0.5 above down to `SNAP_DOWN` 0.4 below (steps up eased); deeper = fall
  (ACT_GLIDE with FALL_fall, lands on the first floor crossed, FALL_collapse past 12 m/s); no
  floor at all = probe from 60 m above (under the map after a teleport). The floor face's
  tColLighting feeds `timecycle::setSurfaceLight` (pedLight contact brightness). Camera: cast
  from CJ's head to the eye (+0.3 m margin), pulled in at once, eases out. REND `physics` off =
  the old flat SPAWN_Z ground.
- **Overlay** (`render/overlay`): the emitter runs 5x under projections with a constant near
  depth (z row = -k * z_eye, the sky trick at the near end): 4 black passes shifted 1 px, then
  the colour pass nearer still -> on top of the scene with a dark outline. Skeleton (CJ bones)
  and `col::drawDebug` (wireframe faces: green floor / orange wall / magenta ceiling, cyan
  boxes, blue spheres, purple ped spheres, white ground hit) use it; REND: colls, outline,
  range, poly budget (`colPolys`, 5x per face with the outline).

## 7. Bottom screen

- `game/hud/subfb.*`: 96 KB RAM backbuffer DMA'd to sub BG3 (B16 bitmap, VRAM C) in vblank.
  CPU never writes VRAM mid-frame (no tearing). Debug console printf is the only exception.
- `chud.*` (CHud): clock, money, wanted stars, weapon icon/ammo, bars, zone/vehicle names.
  Dirty-region drawing: only the radar repaints every frame; stat blocks repaint on a hash change
  (note: clockRate x60 changes the clock every frame -> HUD ~24 ms/frame while testing).
- `radar.*` (CRadar): pre-baked RGB15 tiles from `models/radar.dsrt`, two pools around the player,
  max 12 KB reads/frame, texel-space inner loop.
- `hudtex`, `hudfont` (CFont on the framebuffer). `tools/hosthud/` compiles the HUD sources on the
  host with an nds.h stub (`shim.cpp`) and dumps PPMs (`./hosthud [frames] [out.ppm]`,
  `./hosthud shots <prefix>`). Glyph quads are 32*sw wide but advance prop*sw: right-anchored text
  must account for the overhang (`hudfont::print RIGHT`).
- `chud::initStep()` is a loading job; `hud::setGameActive()` gates drawing to INGAME;
  `chud::invalidate()` forces a full repaint. `hud::log` keeps a 32-line ring (14 replayed when
  the console is re-initialised on switching back to DEBUG).
- `hud/debugui.*`: tabs (5 chars each, `>` marks the active one): LOG, PERF (fps/cpu/sections/
  polys/loco), MEM (regions, tags, VRAM, anim cache), GAME (wanted/health/money/weapons/body/
  clock/outfit/respawn), REND (`dbg::opts` render/sim switches), SKY (section 5).

## 8. Frontend, loading, intro, audio

- Frontend = CMenuManager port (`game/menu/`), `aScreens` verbatim, virtual 640x480 through
  sprite2d. Menu textures are GROUP_FRONTEND, freed on Start Game, reloaded on pause.
- Loading screen (`loadscreen.cpp`): resumable jobs with a 10 ms/frame budget, order:
  free frontend, splash, loading tune, open gta3 + sparse TOC index, player/anim packs, ped anim
  block, CJ (part per slice), player init, weather init, timecyc (weather per slice), sky
  textures (+ first timecycle update), HUD init, zones, radar prefetch, world (open + preload),
  collision (index + records around CJ, then `player::placeOnGround`), mem report.
  **New heavy init work must be a job.**
- Intro: DVID (pal8 256x256 @10 fps + PCM16) on a bitmap BG. SFX: `genrl.dsfx` banks 59/60
  (frontend), 82 (loading tunes), IMA-ADPCM decoded to PCM16.

## 9. Memory (`src/mem/`)

- Regions carved once from the heap (`budget.h`): SYS (TLSF for libc malloc/new), SCRATCH (LIFO
  `mem::Scratch` RAII, spills to STREAM), PERM (bump, never freed), STREAM (TLSF, everything else).
- Every allocation is tagged (`mem::alloc(n, TAG_x)`): sys text model tex anim audio radar hud
  pack world col temp. `mem::summary/stats/check`. `mem::Pool<T>` (CPool port) for fixed-count objects.
- Temp file reads go to scratch. Static BSS ~130 KB; STREAM ~2.9 MB. Typical in-game:
  STREAM ~600 KB used, PERM 231/320 KB.
- PERM is never freed: allocate there only once per boot (guard with a pointer, e.g. timecycle).
- libc `_malloc_r` & co are replaced in `mem.cpp` (newlib dlmalloc/sbrk are not linked).
  `tlsf.cpp` is portable and passed a 400k-op host stress test (git history); keep it portable.
- Pool budgets for peds/vehicles/objects are already declared in `budget.h`, plus the world ones
  (`WORLD_*`: 192 model slots, 900 KB lists, 320 texture entries, 300 KB map VRAM, 96 sectors),
  collision `COL_CACHE_BYTES` 192 KB / `COL_NEAR_MAX` 48 (tag `col`).
- `tex::` slots: 384 (PERM), most of them map textures.

## 10. Asset pipeline (`tools/`)

- `tools/assets.py sync|build|report|list|info|preview` (see AGENTS.md for invocations).
  Pure Python + multiprocessing, cached per entry (key = handler + params + source entry + hash of
  the handler's modules). Editing a handler module rebuilds that handler's outputs only.
- `resources.toml`: `[global]`, `[defaults.<handler>]`, sections (`kind` = img/files/copy/sfx/
  streams/...) with `[[section.rule]]` (`match`, `match_ide`, `match_texture`, `remove`, handler
  overrides). Resolution: defaults -> section -> rules -> global multipliers.
- Readers (`tools/gtasa/`): rw, img, txd (D3D8/9, DXT, PAL8...), dff, col (tolerant), ifp (ANP3),
  audio. Converters: `ds_texture.py` (pow2 from best mip, median cut, pal4/16/256, a3i5, a5i3,
  direct, `additive`), `ds_mesh.py` (cluster or QEM decimation, s16 quantisation, tris by
  material), `ds_misc.py` (DCOL, DANM), `ds_radar.py`, `intro.py`, `dsfmt.py` (fixed point, DSPK).
- Output formats (details in `ds_formats.h`): DSPK packs (`models/gta3.dspk`, `gta_int.dspk`,
  `player.dspk`, `anim/anim.dspk`) with original entry names; loose files keep lowercased paths;
  `data/**` and `text/*.gxt` verbatim (runtime must lowercase paths read from data files),
  `data/world.dwld` (map placement + collision, sections 6b / 6c);
  `audio/sfx/*.dsfx` + `banks.dsbl`, `audio/streams/*.dsst` (DSND, IMA-ADPCM).
- DSPK: entry names are the original ones (`infernus.dff`), payload in DS format (DMDL, DTXD,
  DCOL, DANM); `.ipl/.dat` inside IMGs copied raw. TOC sorted by FNV-1a(lowercase name),
  16 B/entry in RAM (gta3 = 256 KB, hence the sparse index). DSND = DS IMA-ADPCM (4-byte header,
  nibbles low first) or PCM8/16. `banks.dsbl` maps global bank id -> pak/index.
- World (`kind = "world"`, `tools/gtasa/world.py`): gta.dat -> IDE objs/tobj/anim/txdp, IPL inst
  + binary `<ipl>_stream<N>.ipl` from gta3.img (lod indices point into the text IPL), area 0/13
  only, LODs dropped (`lods = false`), optional `bounds` box (Grove Street area for now), quaternion
  negated + CMatrix::SetRotate like LoadObjectInstance; model radius from the dff spheres; the
  models' COL records (section 6c) appended after the instances.
  Not cached per entry (~1 s for the Grove box; a dff parse per referenced model).
- Map models (rule `match_ide = "data/maps/**/*.ide"`): ratio 0.25 clustering with `min_area 0.85`
  (target grows until 85% of the surface survives), mostly-horizontal geometry (`is_flat`,
  ground/roads/bridge decks) uses `decimate_wedge` instead (`ground_qem`): quadric half-edge
  collapses on the welded mesh, borders locked, seams only collapse wedge-to-wedge, folds rejected
  -> no holes/cracks (clustering kept the area with folded tris and showed the sky through).
  Textures 32 px, ground-ish names (`*road*`, `*pave*`, ...) 64 px. `match_ide` accepts globs.
- Decimation: vertex clustering by default (fast, moves seam verts); `model.decimate = "qem"`
  (player_img) = quadric half-edge collapses with open-edge/UV-seam verts locked and same-bone
  collapses only, so runtime-assembled CJ parts stay crack-free.
- CJ shoulders: after the pose blend `CPed::ShoulderBoneRotation` (bones 301/302 are the deltoids:
  upper-arm matrix with half the twist vs the clavicle). Skipping it = boxy shoulders.
- Intro converter note: ffmpeg's rawvideo pal8 output appends a 1024 B palette per frame; the
  converter detects the stride (slicing at W*H drifts 4 rows/frame).
- `tex4x4` is not used yet; `nds_texcompress` exists in `~/devkitpro/tools/bin`.
- Pipeline outputs: `build/assets_cache/` (safe to delete), `build/assets_report.json` (per-entry
  errors), `build/assets_manifest.json` (stale outputs pruned on full builds), `build/preview/`.
- Changing a binary layout: edit the writer AND `ds_formats.h`, bump that format's version.
- Known gaps: 2dfx, night vertex colours, second UV set, material effects, COL face groups and
  shadow meshes, IFP beats; GXT/FXP/SCM copied raw; no tex4x4; DSPK ignores hash collisions;
  `las.ifp` (ANPK) unsupported. Disabled by default: speech paks, radio, cutscenes.
- The source install is modded: `data/timecyc.dat` is a modded timecycle (both it and
  `_timecyc.dat` differ from stock); runtime uses `timecyc.dat` like the game.

## 11. Optimisation strategies used everywhere

- Fixed point in hot paths (4.12 / 20.12 libnds types); floats only for per-frame scalars
  (camera, a few dozen sprites, weather state). Byte-array integer lerps for table blends.
- Build once, replay many: GX display lists DMA'd per frame; CPU only touches matrices.
- Patch instead of rebuild: change a few list words (CJ colours) only when the value changes.
- Recompute on change: fog table, HUD blocks (hash), radar only region every frame.
- Never block a frame: loading jobs with a time budget; streaming reads capped per frame.
- Small resident sets: sparse TOC indexes + page LRU, per-entry TXD reads, pinned clip sets.
- Hardware tricks instead of CPU work: constant-depth sky projection, GE lines, hardware fog,
  manual translucent sort, vblank-latched registers.
- Measure: PERF tab (cpu, sections, polys), DEBUG `hudok` marker for headless numbers.

## 12. Porting map (gta-reversed `source/game_sa/`)

Streaming.cpp/StreamingInfo (model streaming), FileLoader.cpp (gta.dat, IDE, IPL), IplStore,
TxdStore, Models/ (ModelInfo), Collision/ (incl. ColourSet), Animation/ (AnimManager),
Audio/ (AEBankLoader, AETrackLoader), Renderer.cpp, World.cpp, Entity/, Pools/, Camera.cpp,
Hud.cpp, Radar.cpp, TimeCycle.cpp, Weather.cpp, Clouds.cpp, Coronas.cpp, Clock.cpp,
`app/app_light.cpp` (lighting setup), Scripts/ (SCM VM), Core/. Ported so far: Clock (in player),
TimeCycle, Weather, Clouds (sky polys + Render), Coronas (sun only), Hud, Radar, MenuManager,
LoadingScreen, Text/Font, ClothesBuilder (CJ), parts of Ped/PlayerPed control, FileLoader
(LoadObjectTypes/LoadObjectInstance/LoadCollisionFile, offline in world.py), a CStreaming/CRenderer
subset (world.cpp), CCollision line/sphere tests + CTempColModels ped spheres (collision.cpp).
