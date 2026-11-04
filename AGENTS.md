# gtasads - GTA San Andreas port to Nintendo DS

C++17 ARM9 homebrew (devkitARM + libnds/calico), porting the gta-reversed decompilation, with a
Python asset pipeline that converts the PC game files into DS-native formats packed in NitroFS.

**Read `ARCHITECTURE.md` first** - it maps every directory/system (frame loop, render engine and
DS hardware rules, time cycle/weather/sky, player/camera/anims, HUD, loading jobs, memory,
asset pipeline and formats, optimisation strategies, porting map). Use it instead of re-reading
the code base; open source files only for the parts you are changing, and update
ARCHITECTURE.md when a system changes or a new one lands.

Current state: frontend + loading screen + intro, CJ (clothes, anims, locomotion, jump/fight/aim),
bottom-screen GTA HUD with radar, debug UI, time cycle + weather + sky/fog/rain on a placeholder
ground. No world streaming yet (see Roadmap).

## Toolchain
- devkitPro at `~/devkitpro` (`source ~/devkitpro/devkitarm.sh`).
- Emulator: melonDS at `~/bin/melonds` (AppImage wrapper). Override with `EMU=...`. It ignores
  SIGTERM sometimes: use `timeout -s INT` and `pkill -f -i melonds` if a run hangs around.
- Python 3.12 stdlib only (no pip/venv, no numpy/PIL). `ffmpeg` is used for OGG streams/movies.
- PC game: `/mnt/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto San Andreas`
  (heavily modded: SAMP, CLEO, modloader, ENB; `gta3.img` stock, some `data/` files are not,
  e.g. `timecyc.dat`).

## Build / run
- `make -j16` -> `gtasads.nds`. Runs the asset build first if `resources.toml` or `tools/**.py`
  changed. **Must stay warning-free** (`-Wall -Wextra`, libnds headers are `-isystem`).
- `make ASSETS=0` skip the asset pipeline (fast code iteration). `DEBUG=1` = `-Og -DGTASADS_DEBUG`.
- `make build-quick` (= `QUICKTEST=1`): skips intro movies, auto-starts the game, forces the GAME
  HUD screen. `build/.optsig` forces recompilation when flags change (no `make clean` needed).
  Note: every make invocation must repeat the same flags, or it rebuilds with the default ones.
- `make FAT=1`: assets not embedded, read from `sd:/gtasads/` (`nitro:` -> `fat:` redirect);
  ~450 KB .nds that boots under TWiLight Menu++ (the ~400 MB NitroFS build crashes nds-bootstrap).
- `make sdcard` stages `sdcard/gtasads/` + `sdcard/gtasads.nds`; `make dldi` patches the ROM with
  `tools/dldi/r4tfv2.dldi` for melonDS's virtual SD. `make sd` = clean FAT build + both.
- `make sync` / `make assets` wrap `tools/assets.py sync|build`. `make compdb`, `make clean`,
  `make distclean` (also removes `nitrofs/`).

## Verification
- Smoke: `timeout -s INT 8 melonds gtasads.nds` must not crash (exit 124 = still running).
  You can't see the screen: ask the user for screenshots when visuals matter.
- Headless numbers (preferred): DEBUG builds write `fat:/gtasads_hudok.txt` after 120 in-game
  frames: hud stats, mem summary, fps/cpu per section, per-tag bytes, VRAM, a `sky:` line
  (weather, sky colours, far/fog, sun z, polys) and the last log lines.
  ```
  F="ASSETS=0 QUICKTEST=1 FAT=1 DEBUG=1"
  make $F -j16 && make $F sdcard && make $F dldi
  XDG_CONFIG_HOME=/tmp/melcfg timeout -s INT 35 melonds gtasads.nds
  strings /tmp/melcfg/dldi.bin | grep -E "hudok|^sky:|fps=|^log:" | grep -v %
  ```
  `/tmp/melcfg/melonDS` is a copy of `~/.config/melonDS` with `[DLDI] Enable=true,
  FolderSync=true, FolderPath=<repo>/sdcard, ImagePath=/tmp/melcfg/dldi.bin` (SIGINT, not TERM:
  melonDS only writes the image on a clean exit). Recreate it if /tmp was wiped.
- To test a specific state headless (time, weather...), add a temporary `#ifdef` hook in a loading
  job and pass it through `OPT="-Og -DGTASADS_DEBUG -DGTASADS_FAT -DGTASADS_QUICKTEST -DMYHOOK=..."`;
  remove it afterwards.
- Fat-backend probe: `strings dldi.bin | grep probe` -> `probe ok`.

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
while tuning. Current total ~413 MB of the 448 MB budget.

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
1. World data: `data/gta.dat`, IDE (objs/tobj/cars/peds), IPL (text `data/maps` + binary `bnry`
   inside gta3.dspk) -> model info table by id.
2. Streaming: sector grid around the camera, fixed RAM budget, LOD (`lod*.dff`), draw distance tied
   to `dbg::opts.fogView`/timecyc far clip, per-frame poly budget. CStreaming/CRenderer.
   World materials then need `POLY_FOG` and the `timecycle::worldLight` tint.
3. Collision + simple physics (ped vs world -> real ground z, collision lighting for `pedLight`,
   rain splashes on real surfaces), then vehicles from handling.cfg.
4. Audio: DSND via the ARM7 sound API, SFX banks (thunder for lightning), ambience streams.
5. Scripts: SCM VM (`data/script/main.scm`), start with mission-less free roam.
Profile constantly (PERF tab, hudok marker) and tune resources.toml to fit.
