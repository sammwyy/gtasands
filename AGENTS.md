# gtasads - GTA San Andreas port to Nintendo DS

C++17 ARM9 homebrew (devkitARM + libnds/calico), based on the gta-reversed decompilation.

## Toolchain
- devkitPro at `~/devkitpro` (`DEVKITPRO`/`DEVKITARM` must be exported; `source ~/devkitpro/devkitarm.sh`).
- Emulator: melonDS at `~/bin/melonds` (AppImage wrapper). Override with `EMU=...`.

## Build / run
- `make -j16` -> `gtasads.nds` (also runs `make assets` the first time). Must stay warning-free (`-Wall -Wextra`).
- `make run` - build and launch melonDS.
- `make assets` - regenerate `nitrofs/` from `game/` via `tools/build_assets.sh`.
- `DEBUG=1` for `-Og -DGTASADS_DEBUG`. `make compdb` for `compile_commands.json`.
- `make clean` / `make distclean` (also removes `nitrofs/`).

## Layout
- `src/` engine code; every subdir is auto-discovered. Object files are flattened into `build/`, so
  source basenames must be unique (Makefile errors on duplicates).
  - `platform/` video (screen/VRAM setup), input, fs (NitroFS), timing (profiling via timers 0+1)
  - `render/` top screen 3D (DS geometry engine, limits: 2048 polys / 6144 verts per frame)
  - `hud/` bottom screen 2D on the sub engine (console now; radar/widgets later)
- `third_party/gta-reversed/` git submodule (shallow), reference source for porting. Not compiled
  directly: code is ported into `src/` piece by piece. `git submodule update --init --depth 1`.
- `game/` original PC files copied from the Steam install (gitignored, copyrighted). Source:
  `/mnt/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto San Andreas`.
  Only `data/`, `models/` (minus cutscene.img and console button txds), `anim/ped.ifp`,
  `anim/anim.img`, `text/american.gxt`. Audio/movies/cutscenes not copied yet.
- `nitrofs/` generated, packed into the ROM. DS cart limit is 512 MB, so heavy assets must be
  converted (textures, meshes, collision) by `tools/` instead of copied raw.

## Screens / VRAM
- Top = main engine 3D, bottom = sub engine 2D HUD (`lcdMainOnTop()`).
- VRAM A,B textures; C sub BG; E texture palettes; D,F-I free.
