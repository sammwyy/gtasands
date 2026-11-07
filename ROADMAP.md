# Technical Roadmap — GTA San Andreas for Nintendo DS

## Purpose and scope

This roadmap tracks the playable DS sandbox and the remaining work toward a
stable open-world port. The first target is not full PC parity: it is a
measurable vertical slice where the player can explore a streamed section of Los
Santos on foot and by vehicle.

The port uses user-supplied original game assets. The gta-reversed submodule is
a behavioral and format reference only; DS code belongs in src and must not
attempt to compile RenderWare or the PC game as-is.

| Delivery level | Outcome |
| --- | --- |
| L1 — playable slice | CJ can walk, collide, render a real Los Santos zone, use the HUD, and drive one vehicle. Implemented; hardware/performance validation remains. |
| L2 — sandbox | Sector streaming, limited traffic/peds, combat, basic audio, save/load. Save/load remains. |
| L3 — game loop | A small SCM opcode subset supports simple missions and persistent progression. |
| L4 — expansion | Islands/interiors are streamed; broader scripts, radio, activities, and fidelity work remain. |

L1 functionality is present. Prove its frame-time budgets on hardware before
expanding simulation breadth further.

## Verified baseline

### Implemented

- [x] ARM9 C++17 build, NitroFS, FAT/DLDI SD backend, and QuickTest path in the
  Makefile and platform filesystem layer.
- [x] Asset pipeline for IMG, TXD, DFF, COL, IFP, SFX, streams, and intro video.
- [x] Intro movies, frontend SFX, title/loading screen, start menu, and pause menu.
- [x] GXT loading, proportional fonts, frontend sprites, and top-screen 2D renderer.
- [x] Bottom-screen software GTA HUD, rotating radar, debug console, and host HUD
  renderer.
- [x] DSPK, DMDL, DTEX and DANM readers; animated CJ with streamed outfits.
- [x] Streamed exterior/interior map with HD/LOD, real collision and chase camera.
- [x] Fixed-point player movement, weapons/combat, vehicles, population, paths,
  time/weather, and positional world audio.

### Remaining gaps

- [ ] SCM VM, missions, and persistent save/load.
- [ ] Eliminate driving/streaming frame spikes and verify sustained performance on DS hardware.
- [ ] Complete radio, mission-created content, and remaining PC fidelity systems.

## Non-negotiable DS constraints

- ARM9 runs at 67 MHz without an FPU. Use fixed point in simulation, culling,
  animation hot paths, and world rendering. Existing floats are prototype code.
- Main RAM is 4 MB. Every long-lived allocation needs an owner, fixed budget,
  high-water metric, and deterministic failure path.
- Texture VRAM is primarily 256 KB in banks A+B, with palette bank E and bank D
  governed by an explicit policy. The practical target is 2048 polygons and
  6144 vertices per frame.
- SA world coordinates exceed GX signed 20.12 limits. Emit geometry relative to
  the player/camera origin, never at absolute SA coordinates.
- The bottom HUD uses a 96 KB RAM framebuffer; radar tiles are streamed.
  Measure heap pressure before adding large caches.
- Runtime paths and pack lookups must consistently lowercase data-file names.

## Target architecture

    converted assets
          |
          v
    pack I/O -> streaming scheduler -> RAM and VRAM caches
          |                                |
          v                                v
    gta.dat/IDE/IPL -> offline DWLD -> GX renderer <- DMDL/DTXD/DCOL/DANM
                            |
                            v
              entities, collision, physics, AI, audio, future SCM
                            |
                            v
                     HUD, pause menu, future save/load

### Cross-cutting rules

1. New systems expose init, reset, update(dt60), and where needed render.
   main.cpp only orchestrates application states.
2. Rendering performs neither filesystem I/O nor heap allocation. Streaming
   publishes fully loaded resources between frames.
3. Entities retain ids and handles, never raw pointers to evictable asset blobs.
4. Use explicit fixed pools/arenas for world, streaming, entities, effects, and
   audio. Log pool exhaustion and degrade predictably.
5. Any binary-layout change updates the Python writer, ds_formats.h, format
   version, and a host-side validation test together.
6. Profile on real DS and melonDS: CPU/frame, RAM/VRAM, polygons, vertices,
   sector state, cache misses, I/O bytes, and audio underruns.

## Phase 0 — Engineering foundation and observability

**Goal:** make later work reproducible, measurable, and safe under memory limits.

- [x] Add tagged SYS/SCRATCH/PERM/STREAM arenas, fixed entity pools, memory
  high-water statistics, and bounded world/audio caches.
- [ ] Expand timing into scoped per-system profiling. Debug HUD must show CPU,
  free/peak RAM, VRAM, I/O, resident models/textures, active sectors, collision
  objects, and streaming queue depth.
- [x] Add app states for title, frontend, loading, in-game and pause/resume.
- [ ] Add deterministic debug modes: spawn sector, teleport, streaming stress,
  asset fallback, render-budget limits, and audio disabled.
- [ ] Create host validators for DSPK, DMDL, DCOL, and DANM offsets, versions,
  hashes, and malformed data; retain the existing host HUD renderer for visual
  regression.
- [x] Use fixed-point world/collision queries and vehicle physics (`fx::q`).
- [ ] Unify fixed-point world position, angle, velocity and sector coordinate
  wrappers across systems to prevent unit and fraction mistakes.

**Exit criteria:** warning-free QuickTest build; NitroFS and FAT smoke boots do
not crash; debug output reports memory and streaming budgets.

## Phase 1 — Game data and world database

**Goal:** translate GTA data files into compact runtime ids and spatial records.

- [x] Parse gta.dat offline for IMG, IDE, IPL, COLFILE, and relevant paths;
  bake the result into streamed runtime data instead of loading it all at boot.
- [x] Bake IDE objs/tobj and vehicle/ped metadata into compact world/vehicle/ped
  tables keyed by model id, including draw distances, TXD names, LOD and time windows.
- [ ] Add missing IDE weapon and 2dfx model metadata.
- [x] Parse textual IPLs and binary bnry IPL entries inside DSPK. Store transform,
  model id, interior, LOD id, and spatial-sector membership.
- [x] Consume timecyc.dat at runtime and bake water.dat, surface.dat,
  handling.cfg, carcols.dat and ped population data for runtime use.
- [ ] Consume remaining weapon.dat, default.dat and pedstats.dat fields as
  their runtime systems need them.
- [ ] Validate missing DFF/TXD/DCOL references, duplicate model ids, bad LOD chains,
  and out-of-range IPL records. Generate a host-side sector report.
- [ ] Introduce asset profiles in resources.toml: build/test Grove Street and Los
  Santos before adding broader islands or optional systems.

**Exit criteria:** Grove Street resolves compact instance records, model metadata,
LOD links, and collision references entirely by ids.

## Phase 2 — Runtime resources and real world renderer

**Goal:** replace the grid with textured DFF map geometry under strict budgets.

### Resource layer

- [ ] Extend Pack with validation, range reads, diagnostics, and name-aware hash
  collision handling. Avoid full-blob copies when an arena/range read suffices.
- [x] Stream world textures with a VRAM budget, TXD lookup, refcounts, LRU,
  fallback and eviction.
- [ ] Replace the global texture pool with TextureCache: VRAM byte budget,
  TXD/texture key, LRU, pinning, fallback texture, and safe eviction.
- [x] Cache streamed static DMDLs and compiled GX lists separately from
  skinned player/ped and vehicle models.
- [ ] Add ModelCache for source blob, parsed DMDL view, GX representation, and
  ownership state. Keep skinned and static resources distinct.
- [ ] Resolve every geometry material to its named TXD texture. Support untextured
  material color, color-zero alpha, translucent ordering, and visible
  missing-resource fallback.
- [x] Compile static DMDLs into GX lists on stream-in; CJ and peds use
  bone-matrix skinning through GX lists.

### Renderer layer

- [ ] Create a world renderer with a render queue: opaque batches by
  material/texture, alpha-test, then sorted transparent batches.
- [x] Apply IPL transforms in fixed point. Support UVs, vertex colors, material
  color, culling, and ambient/prelight rendering first.
- [x] Add frustum culling by sphere/AABB, distance culling, and pre-requested LOD
  selection through ModelInfo.
- [ ] Set quotas for static world, entities, and transparent effects. On overflow,
  drop farthest eligible content before emitting GX commands.
- [x] Load timecyc for clear color, fog and day/night lighting; draw flat
  surfaces from water.dat.

**Exit criteria:** Grove Street displays real terrain/buildings/textures, no
synthetic grid remains, and sustained camera movement stays at or above 30 FPS
within polygon and vertex budgets.

## Phase 3 — Spatial streaming and world lifetime

**Goal:** move through the map without loading it all or pausing unpredictably.

- [x] Partition IPL instances and collision into 50 m world sectors; stream
  nearby ped and road graph cells, preload inward-first and discard distant sectors.
- [ ] Implement resource state machines: unloaded, queued, reading, ready, failed,
  and evictable. Coalesce duplicate requests.
- [ ] Give filesystem reads, parse/decode, VRAM upload, and eviction each a
  frame-time and byte budget. Renderer sees only ready resources.
- [ ] Pin visible-sector and active-entity resources. Evict only unpinned assets in
  an explicit VRAM, GX, then RAM order.
- [x] Stream static instances per sector with tobj time-of-day activation
  and area-aware interior filtering.
- [ ] Add a general fixed-pool dynamic object system.
- [x] Replace loading-screen fake steps with actual global-data, spawn-sector,
  critical model/TXD/DCOL, HUD, and audio progress.
- [ ] Build a teleport/stress mode that records misses, evictions, bytes/second,
  longest frame, and failed requests.

**Exit criteria:** moving through multiple Los Santos sectors creates no loading
screen, OOM, duplicate reads, or visible resource thrashing.

## Phase 4 — Collision, player, animation, and camera

**Goal:** replace demo movement with a player that inhabits the streamed world.

- [x] Add a DCOL runtime loader and sector index for triangles, spheres, and boxes.
  Preserve compact converted vertices; do not expand collision meshes into
  float structures.
- [x] Implement ground query, segment cast, sphere wall push, step handling,
  gravity, wall response and surface lighting lookup.
- [ ] Add full capsule overlap and physical surface material response for CJ.
- [ ] Create minimal entity and ped equivalents with fixed transform, bounds,
  model/animation handles, flags, and sector membership. Player state becomes
  an HUD snapshot, not gameplay authority.
- [x] Port idle, walk, run, sprint, jump, fall and land locomotion with
  animation blending/root displacement and map collision.
- [ ] Implement swimming and full water locomotion.
- [x] Generalize CJ's GX bone-matrix approach into streamed skinned ped
  models; support CJ outfit presets and body-shape scaling.
- [ ] Expand clothing customization and unsupported skeleton variants.
- [x] Implement a third-person camera with touch pitch/yaw, follow,
  wall raycast, auto-center and menu-controlled touch sensitivity.
- [ ] Add menu-controlled camera inversion and FOV if needed.
- [x] Feed health, armour, weapon/ammo, money, wanted, clock and zone/vehicle
  names from gameplay state into the HUD; show extracted service radar icons.
- [ ] Add gameplay breath/swimming and mission-owned radar blips.

**Exit criteria:** CJ can walk, run, jump, and collide around Grove Street; the
camera does not clip through buildings; HUD and animation reflect real state.

## Phase 5 — Interaction, combat, and vehicles

**Goal:** finish the smallest meaningful free-roam loop.

### Interaction and combat

- [ ] Add fixed-pool pickups, markers, checkpoints, doors, and blips with distance
  activation.
- [x] Implement weapon inventory/ammunition, DS controls, hitscan,
  impact raycasts, health/damage/death/respawn, and visual/audio feedback.
- [ ] Add projectile weapons beyond the current hitscan subset.
- [x] Implement minimal wanted behavior: crimes, level/timer, stars/blips, and
  limited police response.

### Vehicles

- [x] Load useful handling and vehicles IDE fields: mass, acceleration, braking,
  steering, traction, wheel placement, and seat.
- [x] Implement fixed-point car suspension, wheel traction, body collision,
  orientation, damage/fire and explosions; also drive bikes, boats, aircraft
  and trains with type-specific physics.
- [ ] Add towing and more complete buoyancy, aircraft and bike behavior.
- [x] Add enter/exit, vehicle camera, engine/skid sounds, persistence while active,
  and real HUD vehicle name.
- [x] Render steering/wheels, animated vehicle parts, head/tail lights and
  damage variants.
- [ ] Render passengers and vehicle customization.

**Exit criteria (L1):** player enters a nearby car, drives on real streets with
collision, exits it, and receives correct camera, HUD, sound, and damage state.

## Phase 6 — Population, traffic, and world simulation

**Goal:** make the sandbox feel alive without breaking pools or frame time.

- [ ] Define fixed, measured pool sizes for peds, vehicles, dynamic objects,
  pickups, projectiles, and effects.
- [x] Convert/load pedestrian and road graph cells on demand, with basic
  path-following and local steering.
- [x] Spawn and despawn peds/traffic deterministically by active sector, time,
  density, and distance. Prefer assets already resident in cache.
- [x] Implement ped wander, flee, follow, fight/shoot, hit, fall and death,
  plus basic path-following traffic and wanted cop response.
- [ ] Add ped vehicle enter/exit, richer traffic avoidance/stops and police pursuit.
- [x] Throttle distant/offscreen ped AI and pose updates; despawn distant
  population outside the visible set.
- [ ] Add far-ped model LODs and further population update optimization.

**Exit criteria:** a bounded population runs for ten minutes with stable memory,
no runaway spawns, and the target frame rate.

## Phase 7 — In-game audio

**Goal:** evolve frontend-only sound into a budgeted world audio system.

- [x] Mix frontend, player, vehicle and weather/zone ambience through the
  sound manager; wire the menu SFX slider to the effects mix.
- [ ] Add complete voice/music categories and volume/focus handling.
- [x] Resolve banks.dsbl and DSFX by bank/id; decode/cache short ADPCM sounds off
  the event-critical frame; apply distance attenuation and pan.
- [x] Add surface footsteps, melee/gun impact, weapon, engine, horn,
  crash, skid, door, weather and ped voice sounds.
- [ ] Add sirens, pickup/water and missing HUD event sounds.
- [x] Stream DSST ambience through a PCM ring buffer.
- [ ] Add a radio station and ensure audio streaming yields to visible assets.
- [ ] Track voice limits, fades, pause/focus behavior, and underruns in debug HUD.

**Exit criteria:** player/vehicle/ambient audio is positional and continuous;
streamed ambience/radio does not cause render or streaming stutters.

## Phase 8 — SCM missions and save system

**Goal:** turn the sandbox into a persistent game loop incrementally.

### Scripts

- [ ] Implement a compact DS SCM VM rather than porting the PC interpreter blindly:
  header, globals, locals, threads, bounded stacks, dispatch table, and no
  per-opcode dynamic allocation.
- [ ] Start with a vertical-mission opcode subset: entity creation/control, timers,
  text, markers, cameras, objectives, weapons, money, wanted, and conditions.
- [ ] Add opcode groups with host bytecode tests and unknown-opcode diagnostics that
  include opcode id, offset, and thread context.
- [x] Keep a no-SCM free-roam mode for engine debugging.

### Save/load

- [ ] Define a versioned DS save with CRC and optional chunks: progress, player,
  clock, inventory, persistent vehicles, mission state, and preferences. Do not
  target PC save compatibility.
- [ ] Implement robust FAT slot I/O, temporary-file/rename where available,
  corruption detection, metadata, and confirmation screens.
- [ ] Only persist frontend preferences after their runtime behavior exists.

**Exit criteria (L3 start):** a simple mission can be started, saved, loaded, and
completed; unsupported scripts fail visibly without corrupting state.

## Phase 9 — UI, content, and fidelity expansion

**Goal:** expand presentation only after the core game is stable.

- [x] Add radar legend, pause map, IPL service/property icons and a
  menu-controlled minimap legend.
- [ ] Add touch waypoint, mission-owned blips and remaining radar mode wiring.
- [ ] Make HUD conditional on real settings and game events: subtitles, messages,
  help boxes, weapon/status changes, and hide-HUD preference.
- [ ] Complete frontend backends: save slots, stats, brief, DS controls, language
  persistence, and unavailable-screen handling.
- [x] Add time/weather, rain and sand particles, sun/moon/coronas and
  flat water surfaces.
- [ ] Add shadows, 2dfx and quality fallbacks for every visual feature.
- [x] Stream clothing/ped models, animation clips, exterior islands and
  IPL interiors.
- [ ] Expand clothing, NPC types, radio catalog and missions incrementally.

## Phase 10 — Optimization, compatibility, and release

**Goal:** prove the port works on actual DS hardware, not only an emulator.

- [ ] Publish per-profile budgets: ROM, persistent RAM, peak RAM, texture VRAM,
  resident resources, audio buffers, and worst frame time. Tune resources.toml
  from measurements.
- [ ] Implement controlled degradation: radio, NPC density, draw distance/LOD, and
  transparencies/effects reduce before player/world collision is compromised.
- [ ] Optimize profiled hot paths: fixed-point transforms/skinning, display lists,
  sector culling, texture upload, decode buffers, and allocators.
- [ ] Test NitroFS and FAT/DLDI on melonDS, DSi/TWiLight, and target SD media;
  include return-to-menu, storage errors, corrupted saves, and stress traversal.
- [ ] Automate asset build/report, QuickTest smoke, host parser/format tests, and a
  deterministic benchmark scene with counters/captures.
- [ ] Audit distribution: no original assets in releases, documented asset sync,
  dependency licenses, and supported-install checks.

## Asset-pipeline roadmap

- [ ] Add zone/feature build profiles so L1 Los Santos, interiors, vehicles, and
  optional radio can be measured and installed independently.
- [x] Bake model IDs, bounds, LODs, material alpha, TXD dependencies and
  sector lookup tables instead of parsing GTA data files on ARM9.
- [ ] Add visual formats only after a runtime need is measured: tex4x4, night vertex
  colors, second UV, material effects/reflection, and 2dfx.
- [ ] Improve DFF decimation if building silhouette or material boundaries fail at
  visible distance; validate each LOD with sector previews and triangle counts.
- [x] Keep COL face groups/shadow meshes discarded until a collision or visual
  system needs them.
- [ ] Add per-sector host previews/reports: instances, models, LODs, textures,
  collision, triangle totals, conversion failures, and final bytes.

## Recommended immediate sequence

- [ ] Profile driving and remove vehicle collision/streaming frame spikes.
- [ ] Measure performance on release hardware and stress-test island/interior transitions.
- [ ] Implement SCM-created content, save/load, and mission progression.
- [ ] Add radio and the remaining vehicle, ped, UI, and visual fidelity features.

## Principal risks

| Risk | Mitigation |
| --- | --- |
| Radar + DSPK TOCs + world exceed RAM | Measure in Phase 0; compact TOCs or make radar cache per island before increasing population. |
| Texture VRAM thrashing | Pin visible assets, stream LODs, prefer resident assets for spawns, and never load complete TXDs indiscriminately. |
| CPU skinning exceeds frame budget | Limit nearby peds, use distance LOD/update throttling, and retain display lists for static geometry. |
| FAT read latency is variable | Use queued small reads, preload rings, byte budgets, and visible fallback assets; test DSi and DLDI early. |
| PC code scope grows uncontrollably | Port small DS-oriented interfaces/subsets; use gta-reversed as reference, not a compile-time dependency. |
| ROM exceeds cartridge budget | Asset profiles, LOD/quality tuning, optional content, and FAT distribution for large builds. |

## Global acceptance criteria

- [ ] Build remains warning-free under Wall and Wextra.
- [ ] Render and critical update paths allocate no heap memory and perform no I/O.
- [ ] Ten-minute traversal/stress sessions do not crash, leak, or exceed fixed pools.
- [ ] Each phase has a deterministic scene/test, CPU/RAM/VRAM metrics, and visible
  fallback behavior for unavailable assets.
- [ ] No enabled content profile exceeds the asset budget in resources.toml.
