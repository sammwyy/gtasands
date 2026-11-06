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
| `game/vehicledata.*`, `game/vehiclemodel.*`, `game/vehicle.*`, `game/vehphys.cpp`, `game/vehicleint.h`, `game/fx.*` | vehicles (section 6d): DVEH tables, model streaming + per-atomic lit GX lists (animated parts), fixed-point physics for every type, damage / fire / explosion / debris / lights / draw |
| `game/peddata.*`, `game/pedmodel.*`, `game/pedanim.*`, `game/peds.*` | random peds (section 6e): DPED tables + ped path graph, skinned ped model streaming (bone segments), per-ped anim slots, CPopulation / CPopCycle / ped tasks / interactions |
| `game/waterlevel.*`, `game/skidmarks.*` | CWaterLevel (water.dat polygons, level query, flat surfaces), CSkidmarks |
| `game/collision.*` | map collision: per-model COL records streamed from world.dwld, near list, segment cast / ground probe / sphere push, debug overlay (section 6c) |
| `game/loadscreen.*` | CLoadingScreen + resumable init jobs |
| `game/zones.*` | info.zon -> zone GXT names |
| `game/sfx.*`, `game/intro.*` | frontend audio entity (menu beeps, whoosh, loading tune) on `src/audio`; DVID movie player |
| `audio/` | audio engine (section 8b): hardware channels, bank/sound cache, sound manager, stream channel, ped / weapon / weather / vehicle entities, ambience tracks, audio zones |
| `game/menu/` | CMenuManager port (PC screens table minus Quit in title/pause) |
| `game/text/` | CText (GXT), CFont |
| `game/hud/` | CHud/CRadar ports on a CPU framebuffer (`subfb`), `hudtex`, `hudfont` |
| `game/debugopts.*` | `dbg::opts` runtime switches edited from the debug UI |
| `hud/hud.*` | bottom-screen owner: GAME (chud) vs DEBUG (console), log ring, DEBUG hudok marker |
| `hud/debugui.*` | touch tabs LOG / PERF / MEM / GAME / REND / SKY / CHTS |

Other dirs: `tools/` (pipeline, `hosthud/` host HUD harness, `dldi/`, `dsshot.py` DEBUG screenshot -> PNG), `game/` (gitignored PC
files), `nitrofs/` (gitignored output), `build/` (objects, `assets_cache/`, reports, previews).

## 3. Frame loop (`main.cpp`)

```
timing::frameStart; input::poll
state transitions (title -> menu -> loading -> ingame), SELECT = toggle bottom screen, START = pause menu
video::beginFrame()                 // resets render env: no fog, default clear, auto sort
  menu | loadscreen::draw | renderer::draw      -> SEC_3D
INGAME && !menu: col::update; player::update (60 Hz steps: on foot / car task, veh::step);
                 weather::update; timecycle::update; world::update; veh::update      -> SEC_SIM
hud::draw()  (CPU framebuffer in RAM / debug console)                  -> SEC_HUD
timing::frameEnd
video::endFrame()   // glFlush(manual sort?) ; swiWaitForVBlank ; apply fog/clear registers
world::vblank()     // staged map textures -> VRAM while the 3D engine is idle (any state)
vehmdl::vblank()    // staged vehicle textures (asks world::evictTexture when VRAM is full)
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
3. view matrix, `cj::draw` (first, so the polygon budget never drops CJ; skeleton overlay inside;
   `cj::drawBasis` with the car matrix while CJ is attached to a car), `veh::draw` (own light0),
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
- Vehicles: slot 11 = view, slot 10 = car body (section 6d).
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
- Palettes: `tex::load` pads every palette to 8 colours (4 for PAL4) = the 16 B / 8 B palette
  alignment. libnds 2.0 `vramBlock_examineSpecial` underflows on a free gap smaller than the
  alignment padding and returns an address on top of a live palette (streamed map textures then
  overwrite each other's colours). A texture whose palette can't be allocated fails to load.
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
  (re-evaluated only when the player crosses a sector edge, <= 2 sector reads/frame, innermost
  rings first). Instances are 36 B (DWLD v5), rotation rows pre-converted to GL axes on load;
  each sector keeps a bounding sphere. Only a genuine jump by more than a window radius
  discards the obsolete model/texture cache. Crossing a sector while STREAM memory is tight
  never clears the whole world: old slots are evicted gradually, and smaller DMDLs can still
  load even if the largest free block is below 64 KB.
- **Scan** (in `draw`, ~1.7 ms for ~700 instances): sector sphere vs range/frustum, then per
  instance: tobj hour, distance (`d - radius < view && d < drawDist + radius`), linked HD/LOD
  switch at 100 m (LOD remains visible until HD is ready), sphere vs frustum
  (fixed point, 1/16 m), slot state. When the visible array is full, its least important
  entry is replaced instead of discarding subsequently scanned nearby instances. Not-loaded
  models go to an 8-entry nearest-first want list (out of view = lower priority). Drawn or
  within 40 m = protected from LRU.
- **Model streaming** (`world::update`, ~3 ms budget, 2 starts/frame): `startLoad` reads the DMDL
  into STREAM and queues its textures; textures are found in the model TXD or its txdp parents
  (4-entry TXD directory cache). At most eight DMDLs may wait for textures; each update
  reads up to two queued textures in nearest-visible-instance order, with at most 32 KB staged
  in RAM. Failed allocations defer a load rather than permanently marking a model missing.
  `world::vblank` uploads
  <= 24 KB per vblank, only while `video::textureUploadWindow()` (VCOUNT 192..201: libnds maps the
  banks to LCD during the copy, the 3D engine starts the next frame at ~214; the rest waits a
  vblank), evicting unreferenced textures LRU over `WORLD_TEX_VRAM` (or stale model slots when
  all texture entries are referenced), and records
  TEXIMAGE_PARAM (+repeat) / PLTT_BASE. Once all textures are in, `compile()` builds one GX list.
- **compile()**: walks atomics rather than unique geometries (shared meshes may be instanced
  more than once), bakes each atomic's parented DFF frame transform into the display-list
  vertices; model-wide s16 positions (scale 2^e folded into the instance matrix), per
  material POLY_ATTR (alpha, cull back, fog, light0) + texture registers, triangle list with
  TEXCOORD (shifted per triangle by whole repeats so 12.4 texcoords stay in range) and the
  lighting trick: vertex colour (prelit * material colour) as AMBIENT + constant NORMAL with
  diffuse/specular/emission 0 -> GE outputs prelit * light0, light0 = world light per frame (no
  per-vertex CPU pass for the time of day). Emitted only when the colour changes. ~2.5 KB/model.
  The worst-case list is assembled in `mem::Scratch`; only its actual size is copied to STREAM,
  so building one list no longer requires two large STREAM allocations. Its vertices also
  produce a tighter model-space sphere for frustum culling once the list is ready (used
  only when the original origin-centred radius overestimates the bound by more than 8 m).
- **Draw**: visible instances sorted by projected size (`radius << 10 / (near surface + 4 m)`,
  doubled for instances drawn last frame), a model is skipped when the hardware polygon count +
  tris/2 passes `dbg::opts.worldPolys` (+ `NEW_MARGIN` 96 for instances not drawn last frame);
  a separate worst-case full-triangle check reserves 64 of the hardware's 2048 polygon
  slots so a model cannot overflow the GX polygon RAM when backface culling removes few faces.
  The budget drops what looks smallest and the drawn set has hysteresis: with near->far buckets
  ~37 dropped models/frame flipped in and out (~300 pops per 2400 frames, now ~20, at the camera
  turns). Drawn set = 2 x 1024-entry pointer hash, swapped per draw. GE-bound: each `callList`
  waits for its DMA (~100 us per model).
- **Eviction**: slots LRU (`WORLD_MODEL_SLOTS`, `WORLD_LIST_BYTES`), textures refcounted + LRU.
  If texture VRAM cannot make space, upload waits briefly while stale models are evicted;
  after 20 frames it falls back to an untextured model rather than blocking geometry forever.
  `world::suspend()` drops all lists/textures when the pause menu opens (VRAM); they stream back.
- **Preload** (loading job): sectors + every model in range around the player, no frustum, until
  idle or 10 s.
- Stats: PERF tab rows 15-18 (collision 19-20), `world:` / `col:` lines in hudok, REND tab toggle + poly budget.
  `GTASADS_WORLDTEST` also reserves 24 KB and crosses a nearby sector at frame 1022; its
  `boundary: ... kept=1 pressure=1` line checks that resident models survive the crossing.
- Grove Street numbers: ~110 models / 600 instances in the window, ~70 drawn, ~1600 polys,
  lists ~850 KB, map textures ~200 KB VRAM; draw ~8 ms (scan 1.2) -> 30 fps with the 8 ms HUD.

## 6c. Collision (`game/collision.*`, `render/overlay.*`)

CCollision / CColStore / CWorld::ProcessLineOfSight reduced to an on-foot player + camera.
- **Data** (offline, `world.py`): the COL of every DWLD model (first definition in the gta3.img
  `.col` archives), model space, 1/128 m s16 ("cu"). Faces re-wound to CCW-front
  (`(c-a)x(b-a)` in COL), unit normal 4.12 per face, degenerate ones dropped, then a BVH (median
  split, <= 6 faces per leaf, 16 B nodes); boxes + spheres kept as they are; per-model radius in
  the index. Full exterior: 6,026 COL models, ~1 million faces, ~28 MB on disk.
  Layout `ds::WorldCol*` (DWLD v5).
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

## 6d. Vehicles (`game/vehicledata.*`, `game/vehiclemodel.*`, `game/vehicle.*`, `game/vehphys.cpp`, `audio/aevehicleaudio.*`)

CVehicleModelInfo / cHandlingDataMgr / CAutomobile / CBike / CBoat / CHeli / CPlane / CTrain / CPhysical /
CDamageManager / CAEVehicleAudioEntity, reduced; every IDE type spawns, pool of 8 (`veh::MAX`).
- **Data** (offline, `tools/gtasa/vehicles.py`, section `[vehicles]` -> `data/vehicles.dveh` v2, `ds::Veh*`):
  vehicles.ide (parsed like sscanf: commas or blanks, "dodo		dodo" has no comma) + handling.cfg through
  ConvertDataToGameUnits (speeds per 1/50 s step) + the `!` bike / `%` boat / `$` flying lines (VehInfo `ext[15]`,
  `fly[21]`, `extKind`, `hasFlying`) + carcols pairs + surface adhesion (surface.dat / surfinfo.dat) + the water.dat
  polygons (`WaterPoly`, 307) + the 4 tracks*.dat loops (`TrackInfo` / `TrackNode`, 1949 nodes, cumulative length).
  Runtime keeps a 4 B index per vehicle + colour/surface tables; a 380 B VehInfo is read on demand.
- **Classes** (`veh::Class` from the IDE type): AUTOMOBILE (car, mtruck, quad, kart, trailer, RC), BIKE (bike, bmx),
  BOAT (boats + the Skimmer: plane type with boat handling), HELI, PLANE, TRAIN.
- **Models** (`vehmdl`, 4 slots, refcounted): vehicle DFFs keep `*_dam` frames (`drop_damage = false` rule,
  ratio 0.25, max 320 tris/geometry). Streamed like map models: DMDL read + textures staged (model TXD, then
  `models/generic/vehicle.txd` as txd parent, 64-entry shared cache), uploaded in `vehmdl::vblank`, then one GX
  list per atomic: DIF_AMB per material (diffuse = colour, ambient 5/8), NORMAL per vertex, light0 = sun
  direction in the ped-light colour (dark for wrecks). Body colour placeholders (60,255,0 / 255,0,175) and the
  four light slots are DIF_AMB words patched in place (`setColours`, only on change). Atomics are classified by
  frame name: chassis, wheel (one geometry drawn at the 4 wheel dummies, scaled to the IDE wheel size, left
  side turned 180 deg), components with `_ok/_dam` under their `*_dummy` hinge (doors, bonnet, boot, bumpers,
  windscreen, wings); children follow their parent. **Animated frames** (`vehmdl::Anim`, up to two per part:
  outer model matrix `animM[m1]` x rotation x inner chain `animM[m2]` x rotation x `below`): forks_front /
  handlebars (steer), wheel_front / wheel_rear (bike wheel spin), moving/static_rotor(2) (heli; moving_* shown
  above 60% rotor speed, static_* below), moving/static_prop(2), rudder, elevator(_l/_r), aileron_l/r. Rotors and
  props are left out of the body bounds. Dummies: wheels (bikes: wheel_front / wheel_rear frames), ped_frontseat
  (driver = mirrored x), engine, exhaust, headlights / taillights / boat_lights (corona positions).
- **Physics** (`game/vehphys.cpp`, fixed point `game/fx.h`: Q8.24 `fx::q` for velocities / axes / offsets /
  coefficients, world positions Q24 in s64; `normalise` divides per component (1/len overflows for force vectors);
  sin/cos Taylor, asin A&S, ln/exp for the flying resistance powers; hardware divider / sqrt). Every force is per
  unit mass (`kTurn` = mass / turnMass); `publish()` writes the float `veh::Vehicle` view after every step, `pull()`
  imports it after a debug edit. One `col::focus()` broad phase per step (the swept box) restricts the wheel line
  casts (`col::castSegmentCu`) and body spheres (`col::sphereContactCu`) to the near instances under the vehicle.
  - AUTOMOBILE: ProcessControlInputs, ApplyGravity, 4 suspension lines, ApplySpringCollisionAlt + Dampening,
    ProcessCarWheelPair / ProcessWheel (surface adhesion, wet roads, traction bias, handbrake, burst tyre grip x0.4
    and the line raised by 0.4 r), cTransmission gears + CalculateDriveAcceleration, air resistance, Apply{Move,Turn}Speed,
    body spheres vs the map (e=0.05 + friction), vehicle vs vehicle sphere pairs.
  - BIKE: the same wheel code on two line pairs 0.22 m either side of the centre line, the lock shrinks with speed
    (SpeedSteer), a roll stabiliser keeps the body upright; the lean (MaxLean x steer x speed) is visual only: body,
    rider (`attachMatrix`) rotate about the ground line.
  - BOAT: 6 hull samples vs `water::levelCu` (buoyancy so the hull floats at percentSubmerged), keel / move / turn
    resistance from the boat handling, ProcessBoatControl thrust + rudder at the prop point, aquaplane lift; on land
    the body spheres carry it. Cars in deep water sink and stall.
  - HELI / PLANE (+ Skimmer): wheels / skids roll free (no drive), rotor / prop spin-up (`v.rotor` 0..1), then
    CVehicle::FlyingControl FLIGHTMODEL_HELI / PLANE ported term by term (thrust + fall-off, side slip, yaw/roll/pitch
    + stabilisers, attack / form lift, hover compensation, ceiling 800 m, turn / speed resistance, speed caps).
  - TRAIN: kinematic on the nearest tracks.dat loop (accel / brake along the track, bogie points give the axes).
  Vehicles > 10 m from the player are frozen (outside the collision near list), resting undriven ones sleep.
  Host harness used while porting: compile `vehphys.cpp` + `fx.cpp` with stubbed col / vehdata / water (flat
  ground or sea), see git history of this section; it reproduced 0-100 km/h ~4.5 s (Greenwood), heli hover/climb,
  Dodo take-off ~130 km/h, Speeder ~180 km/h.
- **Damage**: impulse above mass/1500*25 -> health loss (collisionDamageMult*0.6) + the component at the impact
  point (front: bumper -> bonnet -> windscreen, rear: bumper -> boot, sides: doors, top: windscreen): > 150 =
  `_dam` mesh, accumulated > 2500 = falls off (debris pool). Flipped and still -> health drains. Health < 250:
  engine fire, 5 s burn timer -> BlowUpCar (wreck, panels fly, fireball/smoke, blast on CJ / nearby vehicles).
  Bullets (`veh::bulletHit` from player::fireOne, along the aim, stopped by the map): a wheel sphere in the way
  bursts the tyre (CAutomobile::BurstTyre: pop sound, squashed wheel, less grip), the body loses health.
- **Lights / marks**: at night (20-7 h, engine on) head + tail coronas (glow sprites at the light dummies, mirrored),
  brake lights brighter, a glow pool on the road ahead (CShadows car light, cars / bikes); `dbg::opts.vehLights`.
  Skidmarks (`game/skidmarks`, CSkidmarks: 24 marks x 16 parts, new part every 100 ms, fade 2.5..20 s, colour by
  surface type, <= 140 polys/frame, `dbg::opts.skidmarks`) registered by locked tyres, wheelspin
  or wheels sliding while the body actually moves sideways (>0.08 game units/step). Ordinary
  steering without a sideways drift no longer triggers the skid sound or marks.
- **Water** (`game/waterlevel`): GetWaterLevel over the water.dat polygons (bilinear quads, flat triangles, last-hit
  cache, z = 0 outside the map square), one translucent timecyc-water-coloured quad per polygon clipped to the view
  square (`dbg::opts.water`). In the Grove box there is only a pond at (2310, -1416); the debug spawn moves CJ to
  the nearest water / track.
- **Player** (`player.cpp`, `chooseStyle`): cars: Y -> walk to door -> CAR_open/getin/closedoor -> CAR_sit + Drive_L/R
  (IS_LOW = L variants), exit with getout/close. Bikes / BMX / quads: the IDE anim group's block is opened
  (`cj::setVehicleBlock`, bikes/bikev/bikeh/biked/bmx/choppa/mtb/wayfarer/quad.ifp), jumponL / getoffLHS (quad
  geton_LHS / getoff_LHS) with the root extracted while the origin slides side point <-> seat, riding =
  `<P>Ride` + Left/Right (bicycles: `<P>pedal` while pedalling). Boats / aircraft / trains: walk next to the hull and
  sit at once (Drive_boat(_L/_R) or CAR_sit); exit steps off on a side with ground above the water. RC toys
  (`veh::isRemote`): CJ is hidden while driving (`player::hiddenInVehicle`). Controls: cars/bikes/boats d-pad L/R
  steer, UP/A gas, DOWN/B brake/reverse, R handbrake, L horn; helis/planes A throttle, B descend/brake, d-pad up/down
  pitch (up = nose down), left/right roll, L/R yaw. Camera: Process_FollowCar-ish, aircraft further back + pitch.
- **Audio** (`aeveh`, settings table `audio/aevehiclesettings.inc` from gVehicleAudioSettings): cars get the player
  engine (ID / AC / CRZ sweeps), every other type (bikes, boats, aircraft by rotor speed, trains) the dummy engine
  loops of its banks; BMX none. Horn, skid loop by surface, doors, engine start, crash thumps, ped hit, windscreen,
  part off, tyre burst, explosion, fire loop.
- **Debug**: GAME tab rows 15-23: pick within a type `<` `>`, `type<` `type>` (car, bike, bmx, quad, boat, heli,
  plane, train, trailer, rc), spawn / rnd, model id `-10 - + +10 go` (400..611), enter (any type), del all, repair,
  boom, flip, hp250, tyre (burst all), skid on/off, state / susp / damage+burst+lean+rotor / perf rows.
  `GTASADS_VEHTEST` (AGENTS.md; the log line has gas / brake / handbrake / skid and the frame sections).
- **Street traffic**: `peds.dped` v4 also carries compact vehicle path nodes/links from `data/Paths/nodes*.dat`
  within the world bounds + 60 m (`tools/gtasa/peds.py`, `peddata::roadNode/roadLink`). Water nodes
  are excluded; switched-off and roadblock nodes from the PC file are retained in the graph but excluded
  from random traffic spawn/routes (the SCM mission road switches are not implemented). `cargrp.dat` groups (18 civilian, 10 gang, dealers)
  are resolved against `vehicles.ide` at build time; each traffic spawn samples its own `info.zon` zone,
  gang strength and the weekday/time `popcycle.dat` row. Only cars/trucks are eligible (no bike riders yet).
  `veh::update` attempts an offscreen spawn 22..38 m away every 90 frames, up to two traffic cars and leaving two pool slots free. `veh::step` steers each
  NPC driver along connected links in the normal fixed-point physics inside the 10 m collision range;
  outside it moves along the route kinematically. Drivers look ahead to the next link and slow before
  bends and vehicles ahead; NPC skid audio requires actual wheel slip, not merely a transient wheel state. A driver car
  despawns after 90 frames offscreen and >32 m away; unseen new spawns have at most 180 frames
  to enter view. Empty, unclaimed cars despawn only when both offscreen and >45 m away; any car entered by
  CJ is marked player-owned and never distance-despawned. `peds::initStep` retains an already resident
  civilian ped model and samples `CAR_sit` / `CAR_Lsit` once during loading. `peds::draw` draws that skinned
  model at the vehicle's `ped_frontseat` dummy with `veh::attachMatrix` for up to two visible drivers within
  35 m, charged against the ped polygon budget; no pose evaluation or new model streaming occurs per frame.
  Drivers have no independent ped AI/collision; carjacking/traffic lights and full CCarCtrl routing are
  still absent. The DEBUG hudok markers include a `veh:` line with active/traffic/spawned/removed counts
  and `peds:` reports `drivers`.
- **Cost** (DEBUG -Og, melonDS, Greenwood + a second awake car): `veh::step` ~2 ms per call (was 3.5 ms in soft
  float), ~1 ms of it collision queries (`step=..us(qN)` in the vehtest log). Still 15-25 fps while driving: see the
  roadmap in AGENTS.md.
- Not done: see AGENTS.md roadmap (vehicles).

## 6e. Peds (`game/peddata.*`, `game/pedmodel.*`, `game/pedanim.*`, `game/peds.*`)

CPopulation / CPopCycle / the ped part of CStreaming / CPed + street tasks, reduced; pool of `budget::MAX_PEDS` 12.
- **Data** (offline, `tools/gtasa/peds.py`, section `[peds]` -> `data/peds.dped` v5, `ds::Ped*`): peds.ide (12 B
  resident record per model, names read on demand), pedstats, pedgrp.dat (57 ePopcyclePedGroup lines), popcycle.dat
  rows (read on demand), the walkcycle groups of animgrp.dat (index 0 = man; missing clips fall back to it at
  runtime), info.zon zones touching the exterior world with main.scm's new-game zone setup (SET_ZONE_POPULATION_TYPE /
  GANG_STRENGTH / DEALER_STRENGTH / POPULATION_RACE found by scanning the script for the opcodes with an inline label,
  first dense run only = the start-up thread; Ganton = GANGLAND + Grove 25). The global
  paths have 37,650 ped nodes / 80,686 links and 30,587 road nodes / 62,936 links, sorted by
  50 m cells. Node `firstLink` is 32 bits; node IDs still fit 16 bits, but ped tasks keep
  their IDs in signed 32-bit fields. A 5x5-cell window for each graph performs at most one
  file read per frame (also one per resumable loading-screen step), splitting metadata,
  nodes and links across frames; all other nodes/links stay on disk. Empty or not-yet-loaded
  cells yield no spawn candidates. Road spawns iterate local
  cells rather than the entire graph. No global node/link arrays are resident.
- **Models** (`pedmodel`, 6 slots, refcounted, LRU reuse): DMDL (wedge decimation to ~270 tris: clustering averaged
  UVs across islands, qem stalls at ~700) + TXD staged, uploaded in `peds::vblank` (map textures evicted when VRAM is
  full), compiled into lists of <= 24 dominant bones each (`SEG_BONES`, usually 1 segment); per draw a segment stores
  entity * world * invBone per bone in slots 30-k (entity 6, view 5) and DMAs its list. Texture registers in the list
  (+ repeat: RW UVs pass 1.0), COLOR words tinted by the ped light (patched on change). Only a compact skeleton stays.
  The feet lift is the lowest skinned vertex in IDLE_stance: a ped DFF's bind pose is neither upright nor
  pelvis-relative like CJ's parts (raw vertices gave 0.46 m instead of 0.97 and sank the legs).
- **Anim** (`pedanim`): cj.cpp's association rules per ped (4 slots keyed by name hash, blend-out, root extraction),
  driven by `pedmove::setRealMoveAnim` through a bound PoseApi. Poses are evaluated only for drawn peds (every frame
  < 10 m, every 2nd/3rd/4th further), cached per ped (40 bones). `anim::blendWorld` has a one-slot path and a Newton
  normalise (~0.9 ms per 32-bone pose in DEBUG; CJ got faster too).
- **Population** (`peds.cpp`): CPopCycle::Update every second (zone from `peddata::zoneAt`, weekday/weekend, 2 h slot,
  UpdatePercentages -> civ / gang / cop / dealer counts, `dbg::opts.pedDensity`), CStreaming::StreamZoneModels every
  1.5 s (the zone's two strongest gangs, a cop while one may spawn, civilians via PickPedMIToStreamInForCurrentZone,
  one civ swapped every 20 s), FindNewPedType + AddToPopulation every 6 frames (random point on a path link: on screen
  27.5..33 m, off screen 10..16.5 m = PC distances x0.65; spawn points with a different population/race/gang
  zone are rejected; gangs spawn 2-3 with followers), ManagePed (off screen past
  35.5 m, not needed 4 s outside 16 m and off screen, dead after 30 s offscreen). Visible peds are retained
  even at longer distances; the camera-frustum check is shared with vehicle population. Loading job `jobPeds`: tables,
  the first models, GeneratePedsAtStartOfGame.
- **Tasks**: wander (path graph, lane offset, pauses, side-steps peds / CJ), follow (gang formation), flee (panic sprint
  along nearby links selected away from the threat, walls near CJ), attack (melee FightA_1..3 combo + FIGHTIDLE,
  or gang / cop guns: Gun_stand, aim chance by distance,
  map line of sight), HIT_* reactions, KO_* knock-downs + getup / getup_front, death (KO_shot_* / KO_skid_* / KO_spin_*),
  EV_dive / EV_step out of a car's way, DUCK_cower, handsup. Events: CJ's gunshots (civilians flee / cower, hostile gangs
  attack, a cop nearby gives a star), aimed at (hands up / flee / attack), acquaintances (Ballas + Vagos hate CJ, Grove
  respect him, gangs fight each other within 10 m, cops chase while wanted), damage response by ped type + temper/fear,
  gang mates join, witnesses of a death flee. AI LOD: peds off screen and away from CJ think every 2nd / 4th frame.
- **Melee** (`game/melee.h`, melee.dat UNARMED_1 for CJ and the peds): FightA_1/2/3 (+ FightA_G on a body on the
  floor), strike at the move's hit frame, a press during a move queues the next one at its chain frame, damage
  6/9/15/25 (peds x attackStrength). Victims (peds and CJ, CTaskSimpleBeHit): HitA_n of the attacker's move from the
  front, HIT_L/back/R otherwise, the 3rd move knocks down (KO_skid_* + getup / getup_front), FLOOR_hit(_f) when kicked
  down, KO_* death. CJ: `player::meleeHitBy` (ACT_BEHIT / ACT_DOWN / ACT_GETUP / ACT_DEAD, controls locked) with his
  PAIN_A_CARL generic low / high / death voice; `player::isDown()` makes peds kick.
- **Interactions**: `player.cpp` calls `peds::meleeHit` at each move's hit frame (`peds::downedInFront` picks the kick),
  `peds::bulletHit` before `veh::bulletHit` (headshots x3), `peds::gunShot`, `peds::pushPlayer` in collide();
  `veh::pushPed(.., skip, &hit)` per ped (CPed::KillPedWithCar by speed), `peds::explosion` from BlowUpCar. CJ takes ped
  damage through `player::takeDamage` (armour first); CWanted chaos (`player::registerCrime`, ReportCrimeNow multipliers,
  stars at 50/180/550/1200/2400/4600, -1/s without a cop within 18 m, cleared on WASTED). Dead peds drop their money
  (walk over the body). Ped voices PAIN_A_MALE / FEMALE (generic, death, panic), punch / bullet impact sounds, footsteps
  for peds within 13 m.
- **Debug**: REND rows 15-22 (on/off, density, ped poly budget `dbg::opts.pedPolys` 500, spawn, kill all, zone/models,
  drawn/posed, nearest ped task), `peds:` line in hudok, `GTASADS_PEDTEST` (AGENTS.md). Screenshots show peds flat:
  their textures land in VRAM D, which the capture borrows during that very frame.
- **Cost** (DEBUG -Og, Ganton noon, 8-9 peds): update ~1.6-2 ms, draw ~0.8 ms + ~0.9 ms per pose; 27-29 fps at the spawn.
  `GTASADS_WORLDTEST` checks paths and traffic after SF -> LV -> northern LV -> countryside teleports:
  each populated test area recovered with visible geometry and nearby graph nodes; peds and traffic
  spawned in all four. The path streamer peaked at 8.5 ms for one SD read, then ~0.2 ms idle;
  heap validation passed. During a teleport the new path cells and ped models take several frames
  to arrive; sparse countryside without pedestrian nodes only spawns road traffic.

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
- `hud/debugui.*`: tabs (4 chars each, active in yellow): LOG, PERF (fps/cpu/sections/
  polys/loco), MEM (regions, tags, VRAM, anim cache), GAME (wanted/health/money/weapons/body/
  clock/outfit/respawn), REND (`dbg::opts` render/sim switches), SKY (section 5).
- CHTS: bottom category selector PLAYER / VEH / PEDS / TIME / WORLD / WARP, per-category page arrows
  and touch actions (vehicle picker by type/model, wanted 0..5, money, movement speed, weather,
  clock, population, simulation switches). WARP lists 14 direct-tap exterior locations across
  Los Santos, San Fierro, Las Venturas, countryside and desert; coordinates were checked against
  the baked world collision. `player::teleport` detaches CJ from vehicles, resets motion and camera
  position, then holds simulation until the destination sector/collision has streamed and the ground
  can be probed (300-frame fallback). The seven top tabs use four console columns each;
  active tabs are yellow. `dbg::opts.playerSpeedQ` scales CJ's on-foot root displacement
  in quarters (default 4 = x1), without changing vehicle or ped motion.

## 8. Frontend, loading, intro, audio

- Frontend = CMenuManager port (`game/menu/`), PC `aScreens` minus Quit in title/pause, virtual 640x480 through
  sprite2d. Menu textures are GROUP_FRONTEND, freed on Start Game, reloaded on pause.
- DS frontend draws large, scrollable options on the top screen; the bottom software framebuffer
  shows the panorama/title/control hints on black at boot and a north-up regional map around CJ
  (4 m/pixel, using the existing streamed radar.dsrt tile pool, no global-map allocation) with a small control strip
  while paused. The map submenu supports D-pad/touch panning, L/R legend pages with radar icon sprites
  on the top screen, and X for a full overview read in 4 KB slices into a temporary ~66 KB paletted image.
  `map_points` extracts exterior service entrances from the source IPL `enex` sections to
  `data/map_points.dpoi` v1 (s16 x/y + icon type); radar loads these during HUD init and draws
  them on both map views. The legend lists only types present in the current view, with labels
  resolved from the active GXT (`LG_*` keys from CRadar::GetBlipName), never fixed translations.
  The overview is freed on exiting the map; resuming invalidates the dirty-region HUD before it draws again.
  The top screen has no background art or control footer. Spaces in both font
  renderers advance the cursor without drawing the atlas's nonblank space cell. Menu art is
  decoded from fronten2.txd only at boot and released on game start; pause forces
  the bottom screen to GAME mode (SELECT is disabled until the menu closes).
- Loading screen (`loadscreen.cpp`): resumable jobs with a 10 ms/frame budget, order:
  free frontend, splash, loading tune, open gta3 + sparse TOC index, player/anim packs, ped anim
  block, CJ (part per slice), player init, weather init, timecyc (weather per slice), sky
  textures (+ first timecycle update), HUD init, zones, radar prefetch, world (open + preload),
  collision (index + records around CJ, then `player::placeOnGround`), mem report.
  **New heavy init work must be a job.**
- Intro: DVID (pal8 256x256 @10 fps + PCM16) on a bitmap BG (libnds `soundPlaySample`, before
  the audio engine starts). Frontend SFX: `game/sfx` on the engine below.

## 8b. Audio (`src/audio/`, CAudioEngine & co)

- **Hardware** (`aehardware`, CAEAudioHardware): calico's ARM7 driver directly (`soundPreparePcm`,
  auto-updated `soundGetActiveChannels`). Channels 0..13 = sound manager, 14 = stream. DSND
  IMA-ADPCM plays natively; loops restart at `1 + loopStart/8` words (the mixer saves the ADPCM
  state there). Volumes are linear 11-bit (`aesound::volumeFromDb`, quarter-dB table).
- **Bank loader** (`aebankloader`, CAEBankLoader): per *sound*, not per bank slot. `banks.dsbl`
  + pak bank tables resident (PERM), pak files stay open, bank offset tables in a 16-entry LRU.
  `find(bank, id)` = resident handle or -1 + a queued read (EnsureSoundBankIsLoaded returning
  false); `service()` reads the queue under a time budget. LRU over `AUDIO_CACHE_BYTES` 256 KB,
  entries played by a channel (refs) or pinned never go. In game ~55-140 KB.
- **Sound manager** (`aesoundmanager`, CAESoundManager/CAESound): 32 virtual sounds (`Params` =
  tSoundPlayParams: bank, id, pos, dB, rolloff, speed, variance, flags, owner, event, update
  callback). Per frame: owner `update(sound, playPosMs)` (REQUEST_UPDATES; -1 once at the end),
  CalculateVolume (volume - headroom + `gSoundDistAttenuationTable` (`aeattenuation.inc`) +
  directional mike), pan from the camera basis (FRONT_END: x -1..1). Channels go to the loudest
  (uncancellable keep theirs, frontend beeps first); a lost loop restarts later, a one-shot can only
  start within 100 ms (ADPCM can't be entered mid-stream). START_PERCENTAGE on loops = a start
  delay. Pool full -> the quietest one-shot is stolen. Mix: effects fader (menu SFX volume) +
  `MIX_GAIN_DB` 12 + a sum-of-gains limiter (gun fire). Paused (pause menu): non-UNPAUSABLE
  sounds go silent.
- **Listener**: `audio::service` builds it from the chase camera (SA axes) every frame;
  GetPositionRelativeToCamera's 0.5 m forward offset.
- **Entities**: `aepedaudio` (HandleFootstepEvent by surface from `data/surfaud.dat` -> FEET
  banks, landing / collapse, punch swing, CJ pain PAIN_A_CARL boxing / high fall), `aeweaponaudio`
  (PlayGunSounds table per gun, reload A/B), `aeweatheraudio` (city noise L/R, wind gusts, rain
  twin loops `aetwinloop` + drops, thunder after CWeather's WhenToPlayLightningSound). Raised from
  `player.cpp` (PlayFootSteps: lead cycle phase crossing 1/15 and 1/2+1/15 with > 0.5 cycle weight;
  ACT_LAND; ACT_PUNCH phase 0.2; fireOne / reload) and `weather.cpp`.
- **Stream** (`aestream`, CAEStreamingChannel): one mono track from `audio/streams/*.dsst`,
  software IMA-ADPCM into a 16 KB PCM16 ring (the mixer's ADPCM can't loop a ring); play position
  from the CPU timer and the channel timer. ~16k samples/s, ~0.5 ms/frame. Resumes the last track.
- **Ambience** (`aeambience`, CAEAmbienceTrackManager): sandstorm -> track 167, else the active
  audio zone's track (`audiozones`, `data/maps/audiozon.ipl`, all interiors so far); fades
  -0.4 dB/frame out, starts at -34 dB, +1 dB/frame in. No radio / water tracks.
- Loading job `jobAudio` (`audio::initGameStep`: surfaud, audio zones, preload). `EventVol.dat`
  (`[audio_config]` copy section) gives GetDefaultVolume per event.
- Cost: ~0.4 ms/frame idle (4 ambient loops), ~0.6 rain, ~0.9 under M4 fire; soft-float bound.
- Debug: PERF rows 21-22, MEM rows 17-18, `aud:` line in hudok / coltest; `GTASADS_AUDTEST`
  (AGENTS.md) records the mixer with the capture units to WAVs (`tools/audwav.py`).

## 9. Memory (`src/mem/`)

- Regions carved once from the heap (`budget.h`): SYS (TLSF for libc malloc/new), SCRATCH (LIFO
  `mem::Scratch` RAII, spills to STREAM), PERM (bump, never freed), STREAM (TLSF, everything else).
- Every allocation is tagged (`mem::alloc(n, TAG_x)`): sys text model tex anim audio radar hud
  pack world col ped temp. `mem::summary/stats/check`. `mem::Pool<T>` (CPool port) for fixed-count objects.
- Temp file reads go to scratch. Static BSS ~130 KB; STREAM ~2.9 MB. Typical in-game:
  STREAM ~600 KB used, PERM 231/320 KB.
- PERM is never freed: allocate there only once per boot (guard with a pointer, e.g. timecycle).
- libc `_malloc_r` & co are replaced in `mem.cpp` (newlib dlmalloc/sbrk are not linked).
  `tlsf.cpp` is portable and passed a 400k-op host stress test (git history); keep it portable.
- Pool budgets for peds/vehicles/objects are already declared in `budget.h`, plus the world ones
  (`WORLD_*`: 192 model slots, 720 KB lists, 320 texture entries, 280 KB map VRAM (rest: vehicles), 96 sectors),
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
  only, LODs retained (`lods = true`, 100 m linked HD/LOD switching), no `bounds` crop
  (all exterior San Andreas); only sectors around the player are resident. Ped/traffic
  paths also cover the exterior through a separate streamed DPED v5 graph (section 6e).
  ~45,050 instances (6,150 LODs), ~11,264 models, ~14,280 sectors and ~30 MB DWLD v5; quaternion
  negated + CMatrix::SetRotate like LoadObjectInstance; model radius from the dff spheres; the
  models' COL records (section 6c) appended after the instances. Render radii include
  each atomic's transformed geometry sphere, including its parent frame chain.
  Not cached per entry (~50 s for the full world; a dff parse per referenced model).
- Map models (rule `match_ide = "data/maps/**/*.ide"`): ratio 0.25 clustering with `min_area 0.85`
  (target grows until 85% of the surface survives) and `max_shift 0.5` m (grow the target
  rather than moving building corners too far, but stop at the existing `max_tris`/area
  output so the polygon budget is not raised without bound); mostly-horizontal geometry (`is_flat`,
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
Audio/ (AEBankLoader, AESoundManager, AEPed/Weapon/WeatherAudioEntity, AmbienceTrackManager,
AudioZones), Renderer.cpp, World.cpp, Entity/, Pools/, Camera.cpp,
Hud.cpp, Radar.cpp, TimeCycle.cpp, Weather.cpp, Clouds.cpp, Coronas.cpp, Clock.cpp, Skidmarks.cpp, WaterLevel.cpp,
`app/app_light.cpp` (lighting setup), Scripts/ (SCM VM), Core/. Vehicles: Automobile.cpp, Bike.cpp, Boat.cpp, Heli.cpp, Plane.cpp, Train.cpp, Vehicle.cpp (ProcessWheel, FlyingControl, ProcessBoatControl), Physical.cpp, cTransmission.cpp, cHandlingDataMgr.cpp,
Audio/Entities/AEVehicleAudioEntity.cpp (section 6d). Ported so far: Clock (in player),
TimeCycle, Weather, Clouds (sky polys + Render), Coronas (sun only), Hud, Radar, MenuManager,
LoadingScreen, Text/Font, ClothesBuilder (CJ), parts of Ped/PlayerPed control, FileLoader
(LoadObjectTypes/LoadObjectInstance/LoadCollisionFile, offline in world.py), a CStreaming/CRenderer
subset (world.cpp), CCollision line/sphere tests + CTempColModels ped spheres (collision.cpp), CPopulation /
CPopCycle / ped tasks / CWanted chaos (section 6e: Population.cpp, PopCycle.cpp, PathFind.cpp, Tasks/TaskTypes/*, Wanted.cpp).
