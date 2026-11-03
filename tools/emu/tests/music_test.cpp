// The Game Boy music in music.h, checked as data.
//
// What can go wrong in the converter is TIMING and CONTROL FLOW: a skipped
// sound_call or a wrong loop point leaves one channel running ahead of the
// other, and the melody drifts off its own accompaniment. That is measurable
// without ears -- the two channels of a tune must line up. Whether a tune
// sounds right is still `tamapoke-emu --wav out.wav --demo <name>`.
#include "music.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

static uint32_t span(const MusicNote *n, uint16_t from, uint16_t to) {
  uint32_t ms = 0;
  for (uint16_t i = from; i < to; i++) ms += n[i].ms;
  return ms;
}

// true if a is a whole multiple of b, within the rounding each note adds
static bool multipleOf(uint32_t a, uint32_t b, uint32_t slack) {
  if (!b) return false;
  uint32_t k = (a + b / 2) / b;
  return k >= 1 && (uint32_t)abs((int)(a - k * b)) <= slack;
}

int main() {
  char w[160];
  // audio.cpp plays MUSIC_TBL[0] in a fight and [3] on a win, by index
  ck(MUSIC_TRACKS >= 4 && !strcmp(MUSIC_TBL[0].name, "gymleaderbattle"),
     "track 0 is still the gym battle theme");
  ck(MUSIC_TRACKS >= 4 && !strcmp(MUSIC_TBL[3].name, "defeatedgymleader"),
     "track 3 is still the victory fanfare");

  for (int t = 0; t < MUSIC_TRACKS; t++) {
    const MusicTrack &m = MUSIC_TBL[t];
    snprintf(w, sizeof w, "%s: loop points inside both channels", m.name);
    ck(m.loop1 < m.n1 && m.loop2 < m.n2, w);

    // The intro and the looped body must each line up across the channels.
    // A channel may loop a shorter figure on its own (Lavender Town's Ch2 is
    // four notes forever), so the longer body has to be a multiple of the
    // shorter -- not equal to it.
    uint32_t b1 = span(m.ch1, m.loop1, m.n1), b2 = span(m.ch2, m.loop2, m.n2);
    uint32_t i1 = span(m.ch1, 0, m.loop1), i2 = span(m.ch2, 0, m.loop2);
    // ms are rounded from a running frame count, so they do not drift; the
    // slack only covers rounding where a short figure repeats (Lavender's ~7)
    const uint32_t slack = 12;
    bool body = (b1 >= b2) ? multipleOf(b1, b2, slack) : multipleOf(b2, b1, slack);
    snprintf(w, sizeof w, "%s: channel bodies line up (%u / %u ms)", m.name, b1, b2);
    ck(body, w);
    // an intro that differs must be made up by the shorter channel's figure
    uint32_t di = i1 > i2 ? i1 - i2 : i2 - i1;
    uint32_t fig = (i1 > i2) ? b2 : b1;
    snprintf(w, sizeof w, "%s: intros line up (%u / %u ms)", m.name, i1, i2);
    ck(di <= slack || multipleOf(di, fig, slack), w);
  }

  // Real tempo, not a flat ms per unit: the gym theme opens on 16ths at
  // tempo 104, speed 12 -> 4.875 frames = ~82 ms. The old converter gave 216.
  uint16_t first = 0;
  while (first < MUSIC_TBL[0].n1 && !MUSIC_TBL[0].ch1[first].freq) first++;
  snprintf(w, sizeof w, "gym intro 16th note is ~82 ms (got %u)", MUSIC_TBL[0].ch1[first].ms);
  ck(first < MUSIC_TBL[0].n1 && MUSIC_TBL[0].ch1[first].ms >= 78 && MUSIC_TBL[0].ch1[first].ms <= 86, w);

  printf(bad ? "%d FAILED\n" : "all passed\n", bad);
  return bad ? 1 : 0;
}
