#include "cry.h"
#include "cries.h"

bool cryHas(int16_t dex) {
  return dex >= 1 && dex <= CRY_DEX && CRY_MON[dex - 1].base < CRY_BASES &&
         (CRY_BASE_TBL[CRY_MON[dex - 1].base].n1 || CRY_BASE_TBL[CRY_MON[dex - 1].base].n2 ||
          CRY_BASE_TBL[CRY_MON[dex - 1].base].nn);
}

// One hardware frame is 1/59.73 s. The pulse channels' note length is scaled by
// tempo / 256 -- 256 is "as written" (Gen 1's byte is stored as length + 0x80) --
// and the noise channel is not.
static uint32_t frameMs(uint8_t frames, uint16_t tempo) {
  return (uint32_t)(((uint64_t)frames * tempo * 1000u) / (256u * 5973u / 100u));
}

bool CryPlayer::begin(int16_t dex) {
  active = false;
  if (!cryHas(dex)) return false;
  const CryMon &m = CRY_MON[dex - 1];
  const CryBase &b = CRY_BASE_TBL[m.base];
  seq[0] = b.p1; n[0] = b.n1;
  seq[1] = b.p2; n[1] = b.n2;
  seq[2] = b.nz; n[2] = b.nn;
  for (int i = 0; i < 3; i++) { idx[i] = 0; at[i] = 0; }
  clock = 0;
  pitch = m.pitch;
  tempo = m.tempo;
  base = &b;
  active = true;
  return true;
}

uint32_t CryPlayer::step(GbSynth &syn) {
  if (!active) return 0;
  uint32_t next = 0xFFFFFFFFu;
  bool any = false;
  for (int c = 0; c < 3; c++) {
    if (idx[c] > n[c]) continue;               // idx == n+1 means finished
    any = true;
    if (clock >= at[c]) {
      if (idx[c] >= n[c]) {                    // ran off the end: this voice is done
        idx[c] = n[c] + 1;
        continue;
      }
      const CryNote &e = ((const CryNote *)seq[c])[idx[c]++];
      uint32_t ms = (c == 2) ? frameMs(e.frames, 256) : frameMs(e.frames, tempo);
      if (!ms) ms = 1;
      if (c == 2) {
        if (e.vol) syn.noise(e.vol, e.envDir, e.envPeriod, ms, e.freq);
        else syn.silence(2);
      } else {
        // the pitch modifier is added to the 11-bit register, wrapping; a
        // channel with its own pitch_offset ignores the species' pitch
        int16_t set = c ? base->set2 : base->set1;
        uint16_t f = (uint16_t)((e.freq + (set >= 0 ? set : pitch)) & 0x7FF);
        if (e.vol) syn.note((uint8_t)c, f, e.duty, e.vol, e.envDir, e.envPeriod, ms);
        else syn.silence((uint8_t)c);
      }
      at[c] = clock + ms;
    }
    if (idx[c] <= n[c] && at[c] < next) next = at[c];
  }
  if (!any) { active = false; return 0; }
  if (next == 0xFFFFFFFFu) {                   // every voice just ended
    uint32_t last = 0;
    for (int c = 0; c < 3; c++) if (at[c] > last) last = at[c];
    uint32_t tail = last > clock ? last - clock : 0;
    clock = last;
    active = false;
    return tail;
  }
  if (next <= clock) next = clock + 1;
  uint32_t d = next - clock;
  clock = next;
  return d;
}

uint32_t CryPlayer::lengthMs(int16_t dex) {
  CryPlayer p;
  if (!p.begin(dex)) return 0;
  GbSynth s;
  uint32_t total = 0, d;
  while ((d = p.step(s)) != 0) total += d;
  return total;
}
