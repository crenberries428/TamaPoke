// The punching bag is a timing game: a hit only scores as the bag crosses the
// line, so mashing earns nothing. What is pinned here is the RULE, driven
// through sackTap() itself rather than a restatement of its windows.
//
// The phase is placed by hand (sackPhase is the pendulum angle's argument and
// a crossing of the line is any multiple of pi), which makes the test
// independent of wall-clock time instead of racing the swing.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include <cstdio>
#include <cmath>
uint32_t g_seed=77; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void startSack(); void sackTap(); void leaveSack();
extern Pet pet;
extern bool sackOpen;
extern uint16_t sackHits, sackPts;
extern uint8_t sackCombo;
extern float sackPhase;
extern uint32_t sackLastMs, sackLockUntil, sackUntil;
extern int16_t sackNextK, sackLastK;

static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

static const float PI_F = 3.14159265f;
// put the bag `ms` milliseconds past the line (0 = exactly on it) and make the
// crossings before it count as already resolved
static void place(float ms, int k){
  sackLastMs = millis();
  sackPhase = k * PI_F + ms * (2.0f * PI_F / 1500.0f);   // 1500 ms period at the start
  sackNextK = k;
}

static void fresh(){
  startSack();
  sackLockUntil = 0;
  sackLastK = -1;
  sackCombo = 0;
  sackPts = sackHits = 0;
}

int main(){
  setup();
  pet.dbgHatchAs(6, false);
  pet.ivAtk = pet.ivDef = pet.ivSpe = pet.ivHp = 31;
  pet.trAtk = pet.trDef = pet.trSpe = 0;

  startSack();
  ck(sackOpen, "the bag opens for a hatched creature");

  // --- a tap on the line scores, and a second one on the same pass does not
  fresh(); place(0, 1);
  sackTap();
  ck(sackHits == 1 && sackPts == 5, "a tap dead on the line is PERFECT: 5 points");
  sackTap();
  ck(sackHits == 1 && sackPts == 5, "a second tap on the same pass is ignored");

  // --- a tap inside GOOD but outside PERFECT is worth less
  fresh(); place(90, 2);
  sackTap();
  ck(sackHits == 1 && sackPts == 3, "a tap 90 ms late is GOOD: 3 points");

  // --- a tap between passes is a miss: nothing scored, combo gone, taps locked
  fresh(); place(0, 1); sackTap();                       // build a combo of 1
  ck(sackCombo == 1, "a hit starts a streak");
  place(375, 1);                                         // a quarter period out
  sackTap();
  ck(sackPts == 5 && sackHits == 1, "a mistimed tap scores nothing");
  ck(sackCombo == 0, "and breaks the streak");
  ck(sackLockUntil > millis(), "and locks the bag for a moment");
  place(0, 2);                                           // a perfect line, but locked
  sackTap();
  ck(sackHits == 1, "a tap during the lockout does not score even on the line");

  // --- mashing earns nothing
  fresh(); place(375, 1);
  for (int i = 0; i < 40; i++) { sackLockUntil = 0; sackTap(); }
  ck(sackPts == 0 && sackHits == 0, "40 taps at the wrong moment score 0");

  // --- streaks pay, and a pass with no tap breaks them
  fresh();
  for (int k = 1; k <= 4; k++) { place(0, k); sackLastK = k - 1; sackTap(); }
  ck(sackCombo == 4, "four clean passes make a streak of 4");
  ck(sackPts == 5 + 5 + 5 + 6, "and the 4th pass pays a streak bonus");
  place(100, 5); sackLastK = 4;
  sackTap();
  ck(sackCombo == 5, "a late-but-good tap still extends the streak");

  // a crossing that goes by with no tap at all breaks the streak: the render
  // loop is what notices, so drive it
  fresh(); sackCombo = 4;
  sackNextK = 5; sackLastK = 4;
  sackPhase = 5 * PI_F - 0.05f;
  sackLastMs = millis() - 300;                           // time passes: the line is crossed
  render();
  ck(sackNextK == 6, "the unscored crossing is resolved");
  ck(sackCombo == 0, "and an untapped pass breaks the streak");

  // --- the reward is wired to POINTS, through the real trainer
  fresh();
  sackPts = 39; sackHits = 9;
  uint8_t before = pet.trAtk;
  leaveSack();
  ck(pet.trAtk == before + 13, "leaving banks sackPts/3 training, not the hit count");
  ck(!sackOpen, "and closes the bag");

  startSack();
  sackPts = 500;                                         // an absurd score
  before = pet.trAtk;
  leaveSack();
  ck(pet.trAtk - before <= 18, "a huge score is still capped at 18 per session");

  printf(bad ? "FAILED %d\n" : "all good\n", bad);
  return bad ? 1 : 0;
}
