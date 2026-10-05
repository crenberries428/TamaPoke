#!/bin/bash
# Regenerates the screenshots in docs/screens/ that the README shows.
#
#   bash tools/make_screens.sh
#
# They come out of the emulator's headless --shot mode, so they are exactly what
# the firmware draws on the 466x466 panel -- not mockups, and not photographs of
# a screen. Re-run this after changing any screen, or the README slowly starts
# advertising a version of the UI that no longer exists.
set -e
cd "$(dirname "$0")/.."

OUT=docs/screens
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

# Keep this list in step with the tables in README.md § Screens.
SHOTS="main region starter starterj btlmenu btlmoves gympick gymsj dexpick gallery gallery2 player player2 box egg lanready pick moves win"

# name:screen:extra flags -- shots that need a clock hour, a language, a step count or a
# particular species (the biome shots: one species per biome, by day and by night)
EXTRA="set_time:clock0: set_volume:clock1: set_bright:clock2: set_lang:clock3: set_lang_zh:clock3:--lang_zh
sky_day:main:--hour_12 sky_night:main:--hour_23 sky_sunset:main:--hour_19 steps:main:--steps_6240 splash:splash:
biome_meadow_day:main:--dex_25_--hour_12 biome_meadow_night:main:--dex_25_--hour_23
biome_beach_day:main:--dex_7_--hour_12 biome_beach_night:main:--dex_7_--hour_23
biome_forest_day:main:--dex_1_--hour_12 biome_forest_night:main:--dex_1_--hour_23
biome_volcano_day:main:--dex_4_--hour_12 biome_volcano_night:main:--dex_4_--hour_23
biome_mountain_day:main:--dex_74_--hour_12 biome_mountain_night:main:--dex_74_--hour_23
biome_snow_day:main:--dex_124_--hour_12 biome_snow_night:main:--dex_124_--hour_23
biome_graveyard_day:main:--dex_92_--hour_12 biome_graveyard_night:main:--dex_92_--hour_23"

echo "Building the emulator..."
bash tools/emu/build.sh >/dev/null

for s in $SHOTS; do
  tools/emu/tamapoke-emu --shot "$s" --out "$TMP/$s.ppm" >/dev/null 2>&1 || {
    echo "  $s: FAILED"; continue; }
  if command -v sips >/dev/null; then
    sips -s format png "$TMP/$s.ppm" --out "$OUT/$s.png" >/dev/null
  elif command -v convert >/dev/null; then
    convert "$TMP/$s.ppm" "$OUT/$s.png"
  else
    echo "need sips (macOS) or ImageMagick to convert"; exit 1
  fi
  printf '  %-12s %s KB\n' "$s" "$(( $(wc -c < "$OUT/$s.png") / 1024 ))"
done

ZH=6   # LANG_ZH in i18n.h
for e in $EXTRA; do
  IFS=: read -r name screen flags <<< "$e"
  flags=${flags//_/ }
  [ "$flags" = "--lang zh" ] && flags="--lang $ZH"
  tools/emu/tamapoke-emu --shot "$screen" $flags --out "$TMP/$name.ppm" >/dev/null 2>&1 || {
    echo "  $name: FAILED"; continue; }
  sips -s format png "$TMP/$name.ppm" --out "$OUT/$name.png" >/dev/null
  printf '  %-12s %s KB\n' "$name" "$(( $(wc -c < "$OUT/$name.png") / 1024 ))"
done

echo "wrote $(ls "$OUT" | wc -l | tr -d ' ') screenshots to $OUT"
