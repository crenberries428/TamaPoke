#pragma once
// UTF-8 text for the zh/ko UI on top of the 5x7 ASCII bitmap font.
//
// ASCII keeps going through the stock font. Anything else is decoded here and
// drawn from the generated 12x12 table in cjkfont.h. A CJK glyph is two text
// columns wide (12 * size px, against 6 * size for ASCII), so a layout that
// centres on "columns" stays right if it counts with cjkCols(), not strlen().
#include <stdint.h>
#include "cjkfont.h"

struct CjkState {
  uint32_t cp = 0;
  uint8_t need = 0;
};

// Feeds one byte. 0 = partial sequence, 1 = plain ASCII (draw it normally),
// 2 = a complete codepoint is in st.cp.
static inline int cjkFeed(CjkState &st, uint8_t b) {
  if (b < 0x80) { st.need = 0; return 1; }
  if (b >= 0xC0) {
    if (b >= 0xF0) { st.need = 3; st.cp = b & 0x07; }
    else if (b >= 0xE0) { st.need = 2; st.cp = b & 0x0F; }
    else { st.need = 1; st.cp = b & 0x1F; }
    return 0;
  }
  if (!st.need) return 0;  // stray continuation byte
  st.cp = (st.cp << 6) | (b & 0x3F);
  return --st.need ? 0 : 2;
}

// the glyph's 12 rows, or null if the table has no such codepoint
static inline const uint16_t *cjkGlyph(uint32_t cp) {
  int lo = 0, hi = CJK_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (CJK_CP[mid] == cp) return CJK_BITS[mid];
    if (CJK_CP[mid] < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

// Draws one glyph with its top-left at (x, y). A codepoint with no glyph gets a
// hollow box, so a missing character is visible rather than a blank gap.
template <class G>
static void cjkDraw(G &g, int x, int y, int size, uint16_t color, uint32_t cp) {
  const uint16_t *rows = cjkGlyph(cp);
  if (!rows) {
    g.drawRect(x, y, 12 * size, 12 * size, color);
    return;
  }
  for (int r = 0; r < 12; r++) {
    int c = 0;
    while (c < 12) {
      if (!(rows[r] >> (11 - c) & 1)) { c++; continue; }
      int run = 1;  // merge a horizontal run into one rect
      while (c + run < 12 && (rows[r] >> (11 - c - run) & 1)) run++;
      g.fillRect(x + c * size, y + r * size, run * size, size, color);
      c += run;
    }
  }
}

// Display width in text columns: 1 for ASCII, 2 for anything else.
static inline int cjkCols(const char *s) {
  int n = 0;
  CjkState st;
  for (; *s; s++) {
    int r = cjkFeed(st, (uint8_t)*s);
    if (r == 1) n++;
    else if (r == 2) n += 2;
  }
  return n;
}
