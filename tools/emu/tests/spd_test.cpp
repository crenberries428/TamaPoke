// The dodge run. What is pinned here is the RULES, driven through spdStep()
// and spdTap() themselves: a row you stay in hurts, a row you leave is a point,
// every spawned row can be dodged, a stun cannot be chained, and the reward
// goes through the real trainer from POINTS.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include <cstdio>
uint32_t g_seed=55; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void startSpeedGame(); void spdStep(uint32_t now); void spdTap(int16_t,int16_t);
void spdSpawnRow(uint32_t now); void leaveSpeed();
extern Pet pet;
struct SpdRow { float y; uint8_t mask; bool live; bool done; };
extern SpdRow spdRows[];
extern float spdPetX;
extern uint8_t spdLane, spdStreak;
extern uint16_t spdPts, spdHits;
extern uint32_t spdStunUntil, spdUntil, spdOverUntil, spdNextSpawn;
extern bool spdOpen;

static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

#define MAXROWS 8
static const int CX_ = 233, DX = 95;
static int laneX(int l){ return CX_ + (l - 1) * DX; }

static uint32_t T;                      // the test's own clock
static void fresh(){
  startSpeedGame();
  T = millis();
  for (int i = 0; i < MAXROWS; i++) spdRows[i].live = false;
  spdNextSpawn = T + 100000;            // no random rows: the test places its own
}
static void put(int slot, float y, uint8_t mask){ spdRows[slot] = { y, mask, true, false }; }
static void run(int ms){ for (int t = 0; t < ms; t += 40) { T += 40; spdStep(T); } }
static void goLane(int l){ spdLane = l; spdPetX = laneX(l); }

int main(){
  setup();
  pet.dbgHatchAs(6, false);
  pet.ivAtk = pet.ivDef = pet.ivSpe = pet.ivHp = 31;
  pet.trAtk = pet.trDef = pet.trSpe = 0;

  // --- staying in a blocked lane is a crash
  fresh(); goLane(1); put(0, 200, 0b010);
  run(1500);
  ck(spdPts == 0 && spdHits == 0, "a row that hits the pet is not a point");
  ck(spdStunUntil > T - 700 && spdStreak == 0, "it stuns the pet");

  // --- leaving the lane dodges the same row
  fresh(); goLane(0); put(0, 200, 0b010);
  run(1500);
  ck(spdPts == 3 && spdHits == 1, "the same row from another lane is dodged for 3 points");

  // --- the near-miss: a row one lane over cannot hit
  fresh(); goLane(2); put(0, 200, 0b001);
  run(1500);
  ck(spdPts == 3 && spdStreak == 1, "a row two lanes away never touches the pet");

  // --- a stun is not chained: two rows in the band on one stun, one crash
  fresh(); goLane(1); put(0, 200, 0b010); put(1, 140, 0b010);
  run(2300);
  ck(spdHits == 0 && spdPts == 0, "neither row of a back-to-back pair paid");
  // (the second row passes through the stun invulnerably, so it is neither a hit
  //  nor a point: it must not have broken a streak that was already zero)
  ck(spdStreak == 0, "and the stun absorbs the second row");

  // --- taps: thirds of the panel, and nothing while stunned
  fresh();
  spdTap(100, 300);  ck(spdLane == 0, "a tap on the left third picks lane 0");
  spdTap(233, 300);  ck(spdLane == 1, "the middle third picks lane 1");
  spdTap(380, 300);  ck(spdLane == 2, "the right third picks lane 2");
  goLane(1); put(0, 200, 0b010); run(1500);          // crash, stunned
  uint8_t before = spdLane;
  spdStunUntil = millis() + 100000;
  spdTap(100, 300);
  ck(spdLane == before, "taps are ignored while the pet is stunned");

  // --- a clean streak pays more
  fresh(); goLane(0);
  for (int k = 0; k < 7; k++) { put(k, 200, 0b010); run(1500); for (int i=0;i<MAXROWS;i++) spdRows[i].live=false; }
  // each dodge pays 3, plus a streak/3 bonus (0,0,0,1,1,1,2 over seven)
  ck(spdHits == 7, "seven clean rows are seven dodges");
  ck(spdPts == 3 * 7 + 5, "and the streak bonus kicks in from the fourth");

  // --- every row the game spawns leaves a lane open
  {
    bool ok = true;
    for (int n = 0; n < 600; n++) {
      fresh();
      spdSpawnRow(T);
      uint8_t m = 0;
      for (int i = 0; i < MAXROWS; i++) if (spdRows[i].live) m = spdRows[i].mask;
      if (m == 7 || m == 0) { ok = false; break; }
    }
    ck(ok, "600 spawned rows each block 1 or 2 lanes, never all three, never none");
  }

  // --- the reward is wired to POINTS, through the real trainer, and capped
  fresh(); spdPts = 20; spdHits = 3;
  uint8_t b = pet.trSpe;
  leaveSpeed();
  ck(pet.trSpe == b + 10, "leaving banks spdPts/2 training, not the dodge count");
  ck(!spdOpen, "and closes the game");
  startSpeedGame(); spdPts = 900; b = pet.trSpe;
  leaveSpeed();
  ck(pet.trSpe - b <= 18, "a huge score is still capped at 18 per session");

  printf(bad ? "FAILED %d\n" : "all good\n", bad);
  return bad ? 1 : 0;
}
