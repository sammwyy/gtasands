#!/usr/bin/env bash
# Builds the NitroFS tree from the original PC game files.
# Usage: tools/build_assets.sh <game_dir> <nitro_dir>
#
# For now it only copies the small text/config files that the engine will parse
# first. Heavy assets (gta3.img, txd, dff, col, ifp) must be converted to DS
# formats here (downscaled textures, fixed-point meshes, streamed sectors)
# because a DS cart tops out at 512 MB.
set -euo pipefail

GAME="${1:-game}"
OUT="${2:-nitrofs}"

if [ ! -d "$GAME/data" ]; then
	echo "build_assets: '$GAME/data' not found, see AGENTS.md (game files)" >&2
	mkdir -p "$OUT"
	touch "$OUT/.stamp"
	exit 0
fi

mkdir -p "$OUT/data" "$OUT/text"

cp -u "$GAME"/data/*.dat "$GAME"/data/*.ide "$GAME"/data/handling.cfg "$OUT/data/"
cp -u "$GAME"/text/american.gxt "$OUT/text/"

touch "$OUT/.stamp"
echo "build_assets: $(du -sh "$OUT" | cut -f1) in $OUT"
