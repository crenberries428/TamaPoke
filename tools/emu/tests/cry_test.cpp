// Gen 1 cries, checked by rendering them.
//
// A cry is three channels of notes bent by two per-species bytes (pitch,
// length), so what can be asserted is: every Kanto species HAS one, it makes
// sound, it ends, the modifiers actually move the output, and nothing past
// Kanto claims one. Whether it sounds like a Pidgey is ears-only -- that is
// what `tamapoke-emu --wav out.wav --demo cry` is for.
#include "cry.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int bad = 0;
static void ck(bool ok, const char *w) { printf("%s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) bad++; }

static std::vector<int16_t> render(int16_t dex, uint32_t maxMs = 4000) {
  std::vector<int16_t> out;
  CryPlayer p; GbSynth s;
  if (!p.begin(dex)) return out;
  uint32_t ms, total = 0;
  while ((ms = p.step(s)) != 0 && total < maxMs) {
    size_t n = (size_t)((uint64_t)ms * GB_RATE / 1000), at = out.size();
    out.resize(at + n);
    s.render(out.data() + at, n, 80);
    total += ms;
  }
  return out;
}

static int crossings(const std::vector<int16_t> &b, size_t from, size_t to) {
  int c = 0;
  for (size_t i = from + 1; i < to && i < b.size(); i++)
    if (b[i - 1] <= 0 && b[i] > 0) c++;
  return c;
}

int main() {
  int none = 0, silent = 0, runaway = 0, tooShort = 0;
  uint32_t lo = 99999, hi = 0;
  for (int d = 1; d <= 251; d++) {
    if (!cryHas(d)) { none++; continue; }
    auto b = render(d);
    int peak = 0;
    for (int16_t v : b) if (abs(v) > peak) peak = abs(v);
    if (!peak) silent++;
    uint32_t ms = CryPlayer::lengthMs(d);
    if (ms >= 3500) runaway++;
    if (ms < 100) tooShort++;
    if (ms < lo) lo = ms;
    if (ms > hi) hi = ms;
  }
  printf("      cry lengths span %u..%u ms\n", lo, hi);
  ck(none == 0, "all 251 Kanto and Johto species have a cry");
  ck(silent == 0, "and every one of them makes sound");
  ck(runaway == 0, "and every one ends");
  ck(tooShort == 0, "and none is a click");

  ck(!cryHas(0) && !cryHas(-1), "dex 0 and negative have no cry");
  ck(!cryHas(252) && !cryHas(386) && !cryHas(649), "nothing past Johto claims one");
  { CryPlayer p; ck(!p.begin(252) && p.done(), "begin() refuses a species with none"); }
  ck(cryHas(197) && cryHas(152) && cryHas(251), "UMBREON, CHIKORITA and CELEBI are Gen 2 and have one");

  // Gen 2 species each carry their own sequence; two with different data must
  // not render the same, or the table is pointing them at one base cry.
  ck(render(152) != render(155) && render(197) != render(196),
     "Gen 2 species sound different from one another");

  // dex 1/2/3 share a base cry (0x0F) with pitch 128/32/0 and length 1/128/192.
  // If the modifiers were ignored these would be identical.
  uint32_t l1 = CryPlayer::lengthMs(1), l2 = CryPlayer::lengthMs(2), l3 = CryPlayer::lengthMs(3);
  printf("      lengths %u / %u / %u ms\n", l1, l2, l3);
  // the noise channel ignores the modifier, and it is the longest voice for the
  // shortened one, so 1 and 2 can tie; only the lengthened one must stand out
  ck(l1 <= l2 && l2 < l3, "the length modifier lengthens the cry (same base cry, 1 <= 2 < 3)");

  auto b2 = render(2), b3 = render(3);
  int c2 = crossings(b2, 0, 800), c3 = crossings(b3, 0, 800);
  printf("      first 50 ms: %d vs %d zero crossings\n", c2, c3);
  ck(c2 != c3, "the pitch modifier changes the pitch (same base cry, pitch 32 vs 0)");

  // rendering is deterministic: the same species twice is the same samples
  auto a = render(25), c = render(25);
  ck(!a.empty() && a == c, "a cry renders identically every time");
  ck(render(25) != render(26), "different species sound different");

  printf("\n%s\n", bad ? "FAILED" : "ALL PASS");
  return bad ? 1 : 0;
}
