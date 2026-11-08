# gtasads - GTA San Andreas port to Nintendo DS

C++17 ARM9 homebrew (devkitARM + libnds/calico), porting the gta-reversed decompilation, with a
Python asset pipeline that converts the PC game files into DS-native formats packed in NitroFS.

**Read `ARCHITECTURE.md` first** - it maps every directory/system (frame loop, render engine and
DS hardware rules, time cycle/weather/sky, player/camera/anims, HUD, loading jobs, memory,
asset pipeline and formats, optimisation strategies, porting map). Use it instead of re-reading
the code base; open source files only for the parts you are changing, and update
ARCHITECTURE.md when a system changes or a new one lands.

Current state: frontend + loading screen + intro, audio engine (SFX + ambience), CJ (clothes, anims, locomotion, jump/fight/aim),
bottom-screen GTA HUD with radar, debug UI, time cycle + weather + sky/fog/rain, the streamed
San Andreas map including IPL interiors (area-aware sector/model/texture streaming, linked HD/LOD switching, culling,
poly budget) and map collision (COL baked into world.dwld: CJ walks on the real
ground, walls stop him, falls off ledges, the camera is pulled in front of walls; REND tab
overlays: skeleton + collision wireframe on top with an outline), vehicles (ARCHITECTURE 6d: every IDE type - cars, bikes/BMX/quads,
boats on water.dat, helis, planes, trains on tracks.dat, trailers, RC - spawned by type or model id from the debug
GAME tab, enter/exit per type, fixed-point physics, damage panels, burst tyres, fire, explosion, skidmarks, head /
tail lights, engine / horn / skid / door / crash sounds), random peds (ARCHITECTURE 6e: popcycle/zone driven
population on the ped paths, streamed skinned models, wander / flee / fight / shoot / hit / knock-down / die / dive,
gangs + cops, CWanted chaos, money drops; ped/road paths paged from the global graph). ~28 fps
at the spawn (~15-25 while driving).

## Toolchain
- devkitPro at `~/devkitpro` (`source ~/devkitpro/devkitarm.sh`).
- Emulator: melonDS at `~/bin/melonds` (AppImage wrapper). Override with `EMU=...`. It ignores
  SIGTERM sometimes: use `timeout -s INT` and `pkill -f -i melonds` if a run hangs around.
- Python 3.12 stdlib only (no pip/venv, no numpy/PIL). `ffmpeg` is used for OGG streams/movies.
- PC game: `/mnt/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto San Andreas`
  (heavily modded: SAMP, CLEO, modloader, ENB; `gta3.img` stock, some `data/` files are not,
  e.g. `timecyc.dat`).

## Build / run
- `make build -j16` -> `build/fat-gtasads.nds`, `build/fat-gtasads.dsi`,
  `build/gtasads.nds`, `build/gtasads.dsi`, `build/gtasads/` (SD assets).
  `fat-` means assets embedded in the ROM; the unprefixed variants need `build/gtasads/`
  copied to the SD root. `.dsi` boots DSi mode and gets ~16 MB RAM (the existing
  heap manager uses the extra RAM) and the faster ARM9 clock. Runs the asset build
  first if `resources.toml` or `tools/**.py`
  changed. **Must stay warning-free** (`-Wall -Wextra`, libnds headers are `-isystem`).
- `make ASSETS=0 build` skips the asset pipeline (fast code iteration).
  `make ASSETS=0 build-sd-dsi` builds one variant; other targets are `build-ds`,
  `build-dsi`, `build-sd-ds`. `DEBUG=1` = `-Og -DGTASADS_DEBUG`.
- `make build-quick` (= `QUICKTEST=1`): skips intro movies, auto-starts the game, forces the GAME
  HUD screen. Per-variant `build/tmp/*/.optsig` forces recompilation when flags change (no `make clean` needed).
  Note: every make invocation must repeat the same flags, or it rebuilds with the default ones.
- `STORAGE=sd`: assets not embedded, read from `sd:/gtasads/` (`nitro:` -> `fat:` redirect);
  ~450 KB .nds that boots under TWiLight Menu++ (the ~400 MB NitroFS build crashes nds-bootstrap).
- SD ROMs are DLDI-patched automatically. `make sdcard` builds both SD ROMs and stages
  `build/gtasads/`. `make run` launches DSi embedded; `make run-ds` DS embedded;
  `make run-sd` DSi SD; `make run-sd-ds` DS SD. Run profiles live in
  `build/emulator/`; DSi emulation reads locally provided `biosdsi9.bin`,
  `biosdsi7.bin`, `dsifirmware.bin` and `dsinand.bin` from
  `build/emulator/firmware/` (or configured `[DSi]` paths from
  `~/.config/melonDS/melonDS.toml`, overridable via `MELONDS_CONFIG`).
  The profiles copy the writable NAND and firmware; source dumps remain untouched.
  Keep melonDS's DSi SD emulation enabled for both DSi variants: without it,
  `run` stalls on a black/blue screen even when all assets are embedded in the ROM.
  The embedded variant uses an empty SD root and reads assets from NitroFS.
- `make sync` / `make assets` wrap `tools/assets.py sync|build`. `make compdb`, `make clean`,
  `make distclean` (also removes `nitrofs/`).

## Verification
- Smoke: `timeout -s INT 8 melonds build/fat-gtasads.nds` must not crash (exit 124 = still running).
  You can't see the screen: ask the user for screenshots when visuals matter.
- Headless numbers (preferred): DEBUG builds write `fat:/gtasads_hudok.txt` after 120 in-game
  frames: hud stats, mem summary, fps/cpu per section, per-tag bytes, VRAM, a `sky:` line
  (weather, sky colours, far/fog, sun z, polys) and the last log lines.
  ```
  F="ASSETS=0 QUICKTEST=1 DEBUG=1"
  make $F -j16 build-sd-ds && make $F stage-sd
  timeout -s INT 35 make $F run-sd-ds
  strings build/emulator/ds-sd/dldi.bin | grep -E "hudok|^sky:|fps=|^log:" | grep -v %
  ```
  `tools/run.py` creates an isolated melonDS profile with `[DLDI] Enable=true,
  FolderSync=true, FolderPath=<repo>/build/emulator/ds-sd/sd-root, ImagePath=<repo>/build/emulator/ds-sd/dldi.bin` (SIGINT, not TERM:
  melonDS only writes the image on a clean exit). Recreate it if /tmp was wiped.
  For DSi use `build-sd-dsi` / `run-sd`; a verified DSi DEBUG run reported
  `str 2134/14782K ... heapok=1` in
  `build/emulator/dsi-sd/sd-root/gtasads_hudok.txt`.
- Headless screenshots (DEBUG): the hudok block captures the 3D screen at frames 120 + k*90
  (`build/emulator/ds-sd/sd-root/gtasads_shot0..5.dscr`: N, W, S, E, bird's eye, 150 m west after a teleport) and writes
  `gtasads_hudok2.txt` after the teleport. Convert + look at them: `python3 tools/dsshot.py
  build/emulator/ds-sd/sd-root/gtasads_shot0.dscr /tmp/shot0.png`, then read the PNG. Needs ~100 s of emulator time
  (`timeout -s INT 100`). The capture borrows VRAM D (textures there are garbage afterwards).
- To test a specific state headless (time, weather...), add a temporary `#ifdef` hook in a loading
  job and pass it through `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DMYHOOK=..."`;
  remove it afterwards.
- Fat-backend probe: `strings dldi.bin | grep probe` -> `probe ok`.
- Collision test (DEBUG): `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DGTASADS_COLTEST"`
  turns the skeleton + collision overlays on for all shots, then after the screenshot sequence
  runs CJ north / west with a held d-pad (`input::debugHold`), drops him from 6 m and writes
  `gtasads_coltest.txt` (pos, camera distance, near list per 20 frames) + shot6 / shot7.
  Needs `timeout -s INT 140`. `tools/dsshot.py IN OUT --scale 3` zooms to check 1-px outlines.
- Audio test (DEBUG): `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DGTASADS_AUDTEST"`
  runs, jumps, punches, rain + lightning, M4 bursts, sandstorm after the screenshot sequence;
  `gtasads_audtest.txt` = `aud:` stats per phase, `gtasads_aud_<phase>.wav` = the real mixer output
  (capture units). `python3 tools/audwav.py build/emulator/ds-sd/sd-root/gtasads_aud_run.wav [--png out.png]` prints
  peak/RMS/clipping. Needs `timeout -s INT 170`; don't rebuild the ROM while it runs, and keep the
  melonDS window unfocused (key presses there move CJ and skew the numbers).
- Vehicle test (DEBUG): `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DGTASADS_VEHTEST"` spawns a
  Greenwood, presses Y (walk to door, open, getin, close), drives west into a second car, brakes, gets out,
  sets the health under 250 (fire -> explosion). `gtasads_vehtest.txt` (car/CJ state per 20 frames, crash damage
  lines), shot6..shot10. Needs `timeout -s INT 200`.
- Ped test (DEBUG): `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DGTASADS_PEDTEST"` places a ped in
  front of CJ (shot0), punches it (its gang mate joins), gives a pistol and shoots the other; `gtasads_pedtest.txt` =
  CJ + every ped's task / health / distance per 20 frames. Needs `timeout -s INT 110`. Peds look flat in captures
  (their textures are in VRAM D, borrowed by the capture).
- World/path test (DEBUG): `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DGTASADS_WORLDTEST"`
  teleports from Ganton through San Fierro, Las Venturas, its northern outskirts and a countryside town;
  `gtasads_worldtest.txt` records loaded path/road nodes, population, traffic, render and memory counts,
  including the path-read time and `heapok`. Allow `timeout -s INT 200` and inspect `gtasads_world_*.dscr`.
- Cutscene skip regression: add `-DGTASADS_CUTSKIPTEST` to a DEBUG QUICKTEST SD build's
  `OPT` (with the appropriate DS/DSi flags). It starts PROLOG1 despite a
  pre-existing save, skips at frame 40, and the usual `gtasads_hudok.txt` /
  `gtasads_shot0.dscr` should show exterior world models at CJ's restored spawn.
- World flicker regression: add `-DGTASADS_FLICKERTEST` to a DEBUG QUICKTEST SD
  build's `OPT`, plus `-DGTASADS_FAT` and `-DGTASADS_DSI` for DSi. Run the
  matching `run-sd-ds` or `run-sd` target with the same flags for ~100 s,
  interrupting melonDS with SIGINT so the virtual SD syncs. The test holds
  westward movement from frame 100 to 800, stops an opening cutscene if one
  started, and records frames 100-850 in
  `build/emulator/{ds,dsi}-sd/sd-root/gtasads_flickertest.txt`: model reloads,
  evictions, previously drawn instances dropped by the polygon budget, and
  STREAM free/largest-block sizes. `gtasads_flickershot.dscr` captures the
  final view. This does not prove that every object stays
  visible: the 2048-polygon hardware limit remains the same on DS and DSi.
- Skid regression (DEBUG): the same flags with `-DGTASADS_SKIDTEST` write `gtasads_skidtest.txt` after
  a normal Greenwood turn and straight driving; both should report `skid=0.00`.
- What stopped CJ there? `python3 tools/colnear.py X Y Z [R]` lists the baked collision
  (faces with world n.z, boxes) around a point.

## Asset pipeline commands
```
python3 tools/assets.py sync                      # PC install -> game/ (only what enabled sections need)
python3 tools/assets.py build [SECTION..]         # game/ -> nitrofs/ ; -j N, --force, --limit N
python3 tools/assets.py report                    # sizes per section/type vs budget
python3 tools/assets.py list gta3_img -n 40       # biggest entries + resolved params / REMOVED
python3 tools/assets.py info models/gta3.img:infernus.dff   # inspect img/txd/dff/col/ifp
python3 tools/assets.py preview models/gta3.img:infernus.txd [TEX] --section gta3_img
```
Full cold build ~6 min (16 cores); `models_loose` alone ~20 s. Use single sections / `--limit N`
while tuning. Current total ~387 MB of the 448 MB budget.

## Hardware budget (keep in mind for every decision)
- ARM9 67 MHz, no FPU -> fixed point at runtime (20.12 / 4.12, libnds `f32`/`v16`/`t16`).
- 4 MB main RAM; nothing big stays resident, stream from NitroFS/SD.
- 3D: 2048 polys / 6144 verts per frame, 256 KB texture VRAM (A+B, D also mapped), palettes in E.
- Cart max 512 MB (`budget_mb = 448`). Top = 3D main engine, bottom = sub engine 2D.

## Working rules
- Port from `third_party/gta-reversed/source/game_sa/` (shallow submodule, reference only) with the
  original class/function names in comments; replace RW with our formats/GX, floats with fixed
  point in hot paths, keep pools small and explicit.
- Every allocation through `mem::` with a tag; temp reads in `mem::Scratch`; PERM only once per boot.
- New heavy init work = a resumable loading-screen job, never inline in a frame.
- Never write VRAM C outside vblank (bottom screen) and never toggle 3D control/fog/clear registers
  mid-frame (use `video::setFog/setClearColor/setManualSort`).
- Changing a binary layout: writer + `src/assets/ds_formats.h` + bump version.
- Source basenames must be unique (objects are flattened into `build/`).

## Roadmap (suggested order)
1. World data + streaming: exterior and interior IPLs with HD/LOD links (ARCHITECTURE 6b),
   area-aware collision (6c), ped and road graph cells paged on demand (6e). Next: triangle
   strips in compile() (lists are GE/DMA bound, ~8 ms for ~70 models), performance and extended
   travel tests; SCM-created and mission-specific content are not included yet.
2. Performance: the HUD costs ~8 ms/frame and the map draw ~8 ms -> 30 fps; strips + chaining
   the list DMAs from the DMA IRQ (CPU free while the GE works) would bring back 60.
3. Physics: ped vs world collision and all vehicle types are done (ARCHITECTURE 6c / 6d). Vehicle TODO:
   - [ ] Perf while driving (15-25 fps): `veh::step` is ~2 ms per call (DEBUG), half of it collision queries.
     Run 1-2 body spheres per step (rotate them, or only the leading ones by velocity) instead of all 6-8; cast the
     wheel lines against a per-step ground plane cached from the previous hits; skip sleeping-car pair tests by AABB;
     cap sim steps per frame at 2 for vehicles (MAX_STEPS 4 = death spiral at 15 fps); measure a release (-O2) build.
   - [ ] World streaming hitches while driving fast (`wu` spikes 10-45 ms in the vehtest log): `world.cpp startLoad`
     reads every texture of a model synchronously; queue the texture reads (TEX_QUEUED) and read them under the
     stream() budget, one sector read per frame, prefetch along the velocity.
   - [ ] Collision near list follows the player only (14 m): fast cars outrun it and planes have none at altitude;
     gather around the driven vehicle with a velocity lead.
   - [ ] Bikes: real CBike lean physics (now visual), wheelies / stoppies (Wheelie/StoppieAng), falling off, kick-stand.
   - [ ] Boats: wake / splashes, waves (water.dat wave floats), CJ swimming (exit into water), real mod_Buoyancy volumes.
   - [ ] Aircraft: landing gear up/down (GearUpR, gear frames), Hydra nozzle / vortex, rotor blade collisions,
     stall, plane audio states, parachute on bail-out.
   - [ ] Trains: carriages, stations / doors, track switching (tracks2-4 are separate loops), level crossings.
   - [ ] Trailers: towing (CTrailer / tow bar hitch), RC: CJ standing outside + RC camera instead of hiding him.
   - [ ] Lights: per-light damage, reverse lights, indicator / siren lights, VRAM headlight texture instead of the glow.
   - [ ] Enter/exit: passenger side + passengers, CAR anims for helis / planes with doors, jacking, bike get-off
     while moving, exit point checks against walls.
   - [ ] Traffic lights, carjacking / passengers, full CCarCtrl path strategies and
     vehicle LODs (`*_vlo` kept), number plates, carmods / paintjobs. Basic path traffic is in ARCHITECTURE 6d.
4. Peds (ARCHITECTURE 6e) TODO: ped speech (SPC_* paks are removed), peds driving / traffic, cops arresting
   (busted) and cop cars, carjacking, weapon models in hands, ped vs ped / CJ collision response with ragdoll-ish
   falls, attractors (benches, ATMs), chat pairs, pose cost (0.9 ms per 32-bone pose: compose on the GE or skip
   unused leaf bones), LODs for far peds.
5. Audio: engine done (ARCHITECTURE 8b: footsteps, weapons, weather bed, thunder, ambience
   stream). Next: collision sounds (CAECollisionAudioEntity), ped speech, radio on the stream channel.
6. Scripts: SCM VM (`data/script/main.scm`), start with mission-less free roam.
Profile constantly (PERF tab, hudok marker) and tune resources.toml to fit.
