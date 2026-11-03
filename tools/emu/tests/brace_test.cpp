// Brace and block, the defence trainer. What is pinned is the RULES, driven
// through stepGame() and braceRaise() themselves: a shield stops an attack from
// ITS side, inside its window; a parry pays more; a shield cannot be raised
// again at once; a hit costs a heart; and the reward is wired through the real
// trainer from POINTS.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include <cstdio>
uint32_t g_seed=66; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void startGame(); void leaveGame(); void stepGame(uint32_t now);
void braceRaise(uint8_t zone, uint32_t now); void braceSpawn(uint32_t now);
extern Pet pet;
struct BraceAtk { uint32_t impact; uint16_t flight; uint8_t zone; bool live; };
extern BraceAtk braceAtk[];
extern uint8_t gameScore, gameMisses, gameGain, braceStreak;
extern uint32_t gameUntil, gameOverUntil, braceNextSpawn, braceCoolUntil;
extern bool gameOpen;

static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

enum { L = 0, R = 1, TOP = 2 };
static uint32_t T0;
static void fresh(){
  startGame();
  T0 = millis();
  for (int i = 0; i < 4; i++) braceAtk[i].live = false;
  braceNextSpawn = T0 + 1000000;         // no random attacks: the test places its own
  braceCoolUntil = 0;
}
static void atk(int slot, uint32_t impact, uint8_t zone){ braceAtk[slot] = { impact, 1000, zone, true }; }

int main(){
  setup();
  pet.dbgHatchAs(6, false);
  pet.ivAtk = pet.ivDef = pet.ivSpe = pet.ivHp = 31;
  pet.trDef = 0;

  // --- a shield on the right side stops a right-side attack
  fresh(); atk(0, T0 + 1000, R);
  braceRaise(R, T0 + 800);                 // early: a plain block
  stepGame(T0 + 1010);
  ck(gameScore == 2 && gameMisses == 0, "a shield raised 200 ms early blocks for 2");

  // --- and a parry, raised at the last moment, pays more
  fresh(); atk(0, T0 + 1000, L);
  braceRaise(L, T0 + 960);
  stepGame(T0 + 1010);
  ck(gameScore == 3, "a shield raised 40 ms before impact is a PARRY: 3");

  // --- the wrong side does nothing
  fresh(); atk(0, T0 + 1000, L);
  braceRaise(R, T0 + 960);
  stepGame(T0 + 1010);
  ck(gameScore == 0 && gameMisses == 1, "a shield on the wrong side does not block, and costs a heart");

  // --- too early: the shield has dropped by impact
  fresh(); atk(0, T0 + 1000, TOP);
  braceRaise(TOP, T0 + 500);               // 500 ms early, shield holds 320
  stepGame(T0 + 1010);
  ck(gameMisses == 1, "a shield raised 500 ms early has already dropped");

  // --- too late: raised after the impact
  fresh(); atk(0, T0 + 1000, TOP);
  braceRaise(TOP, T0 + 1030);
  stepGame(T0 + 1040);
  ck(gameMisses == 1 && gameScore == 0, "a shield raised after the impact is too late");

  // --- a frame can land after the next shield was raised: judged at impact
  fresh(); atk(0, T0 + 1000, L);
  braceRaise(L, T0 + 960);
  braceRaise(R, T0 + 1210);                // the player moved on; no frame ran in between
  stepGame(T0 + 1250);
  ck(gameScore == 3 && gameMisses == 0, "an attack is judged by the shield up AT impact, not the one up now");

  // --- the cooldown: a second shield cannot go up at once
  fresh();
  braceRaise(L, T0 + 100);
  braceRaise(R, T0 + 200);                 // 100 ms later: still cooling
  atk(0, T0 + 250, R);
  stepGame(T0 + 260);
  ck(gameMisses == 1, "a second shield inside the cooldown is ignored, so the other side stays open");

  // --- mashing every side in turn cannot cover them all
  fresh();
  uint32_t t = T0;
  int taps = 0;
  for (int i = 0; i < 12; i++) { t += 130; braceRaise(i % 3, t); taps++; }
  ck(braceCoolUntil > T0, "the mashing really ran (not vacuous)");
  atk(0, t + 400, L); atk(1, t + 700, R); atk(2, t + 1000, TOP);
  braceRaise(L, t + 380); stepGame(t + 410);                 // one of three covered
  stepGame(t + 1100);
  ck(gameMisses >= 2, "covering one side leaves the other two to land");

  // --- streak bonus and a pair of attacks from two sides
  fresh();
  atk(0, T0 + 1000, L); atk(1, T0 + 1380, R);
  braceRaise(L, T0 + 990);                  // parry the first
  braceRaise(R, T0 + 1340);                 // 350 ms later: cooldown (240) is over
  stepGame(T0 + 1400);
  ck(gameMisses == 0 && gameScore == 3 + 3, "a pair from two sides can both be parried");
  ck(braceStreak == 2, "and both count toward the streak");

  fresh();
  for (int k = 0; k < 8; k++) {             // 8 clean parries
    uint32_t imp = T0 + 1000 + k * 600;
    atk(0, imp, L); braceRaise(L, imp - 40); stepGame(imp + 10);
  }
  // 3,3,3,3 then +1 for 4..7 (streak 4-7), then +2 from the 9th... streak reaches 8 after the 8th
  ck(gameScore == 3*4 + 4*4, "a streak of four or more adds +1 per block");

  // --- three hits end the run, and it pays out exactly once
  fresh(); pet.trDef = 0; gameScore = 20;
  for (int k = 0; k < 4; k++) { atk(0, T0 + 1000 + k * 500, L); stepGame(T0 + 1010 + k * 500); }
  ck(gameMisses == 3 + 1 && gameOverUntil, "four hits end the run");
  uint8_t after = pet.trDef;
  ck(after == 12, "and the points train defence: 20 points = 12 training");
  stepGame(T0 + 5000); leaveGame();
  ck(pet.trDef == after, "ending twice does not pay twice");

  // --- the clock ends it too
  fresh();
  stepGame(gameUntil + 50);
  ck(gameOverUntil != 0, "the clock ends the run");

  // --- every spawned attack comes from a real side, and pairs differ
  {
    bool ok = true;
    for (int n = 0; n < 400 && ok; n++) {
      fresh(); braceSpawn(gameUntil - 1000);   // late in the run: pairs are possible
      int live = 0; uint8_t z0 = 255;
      for (int i = 0; i < 4; i++) if (braceAtk[i].live) {
        live++;
        if (braceAtk[i].zone > 2) ok = false;
        if (z0 != 255 && braceAtk[i].zone == z0) ok = false;
        z0 = braceAtk[i].zone;
      }
      if (live < 1 || live > 2) ok = false;
    }
    ck(ok, "400 spawns: one or two attacks, valid sides, a pair never from one side");
  }

  // --- quitting banks what was earned, once
  fresh(); pet.trDef = 0; gameScore = 10;
  leaveGame();
  ck(pet.trDef == 6 && !gameOpen, "leaving banks the score through the real trainer");

  printf(bad ? "FAILED %d\n" : "all good\n", bad);
  return bad ? 1 : 0;
}
