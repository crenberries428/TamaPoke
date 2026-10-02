#pragma once
#include <Arduino.h>
#include "dex.h"
#include "noart.h"

// TPK1 animated sprite (legacy format, fallback path). The project uses
// PMD/TPK2 (PmdMon) for everything; this path stays inactive if there is no NNN.bin on the SD.
// The indexed data lives in PSRAM; the palette is RGB565.
struct SdMon {
  bool loaded = false;
  uint16_t w = 0, h = 0, frames = 0, frameMs = 100;
  uint8_t scale = 2;       // integer zoom factor when drawing
  uint16_t palCount = 0;
  uint16_t pal[256];
  uint8_t *data = nullptr;  // frames * w * h indices (0xFF = transparent)

  bool load(int16_t dexNum, bool shiny = false);
  void unload();
};

// actions of the PMD sprites (TPK2 format)
enum : uint8_t {
  PMD_IDLE = 0, PMD_WALKL, PMD_WALKR, PMD_SLEEP, PMD_EAT, PMD_HURT,
  PMD_ATTACK, PMD_POSE, PMD_HOP, PMD_NOD, PMD_BREATH, PMD_SIT,
  PMD_NACTS
};

struct PmdAct {
  uint8_t w = 0, h = 0, frames = 0;
  uint8_t base = 0;  // row+1 of the lowest pixel (anchor by the feet, not the canvas)
  uint16_t ms[24];
  const uint8_t *data = nullptr;  // frames * w * h in the blob
};

// multi-action PMD sprite loaded from the SD into PSRAM
struct PmdMon {
  bool loaded = false;
  // WHICH species is actually in here. It exists so a test can prove the file
  // that got opened matches the dex that was asked for: dexNum was a uint8_t,
  // so every species past 255 wrapped -- 258 MARSHTOMP loaded p002.bin and a
  // Hoenn creature appeared on screen as IVYSAUR.
  int16_t dex = 0;
  uint16_t palCount = 0;
  uint16_t pal[256];
  uint8_t *blob = nullptr;
  PmdAct acts[PMD_NACTS];

  bool load(int16_t dexNum, bool shiny = false);
  void unload();
  bool has(uint8_t a) const { return loaded && a < PMD_NACTS && acts[a].frames > 0; }
};

// gallery thumbnails (the whole thumbs.bin in PSRAM)
struct SdThumbs {
  bool loaded = false;
  uint8_t *data = nullptr;
  uint16_t count = 0;
  bool load();
  const uint8_t *get(int16_t dex) const;  // blob: w,h,palCount,pal[],idx[]
};
extern SdThumbs thumbs;

bool sdBegin();                 // mounts the SD (SDMMC 1-bit), true if there is a card
// The three species sdScanRegionArt() looks for to decide a region's pack is on
// the card. Inline and free of any SD dependency so the tests can check them --
// A PROBE MUST LAND ON A SPECIES THAT IS ACTUALLY PACKED. Alola's midpoint is
// 765 ORANGURU, which SpriteCollab has no sprite for, so p765.bin exists in no
// pack and the region reported NEEDS PACK however completely it was installed.
// i is 0..2: the first, middle and last packed species of the region.
static inline int16_t sdRegionProbe(uint8_t r, uint8_t i) {
  const RegionInfo &rg = REGIONS[r % REGION_COUNT];
  int16_t want = (i == 0) ? rg.lo : (i == 1) ? (int16_t)((rg.lo + rg.hi) / 2) : rg.hi;
  for (int16_t d = want; d <= rg.hi; d++) if (speciesHasArt(d)) return d;
  for (int16_t d = want; d >= rg.lo; d--) if (speciesHasArt(d)) return d;
  return want;                      // a region with no art at all cannot happen
}

// Narrows gRegionArt to the packs actually present. `verbose` logs one line per
// region, which is what the boot report wants; the runtime rescan passes false so
// its output cannot interleave with the PUT transfer protocol the host is parsing.
void sdScanRegionArt(bool verbose = true);
bool sdSerialCommand(const String &line);  // PUT/LS over USB; true if it handles it
extern bool sdReady;
extern bool sdDirty;  // true after receiving files: reload the sprite
// A region's pack can arrive AFTER the card was mounted -- the web installer
// streams it over PUT into the running firmware -- and gRegionArt was computed
// once in sdBegin(). Without this the region stayed locked reading NEEDS PACK
// until the board was rebooted, which looked exactly like the download failing.
// The main loop rescans when this is set; the transfer itself is never delayed.
extern bool sdArtDirty;
