// A battle must not overwrite the player's creature.
//
// foeFromSpecies() and the BATTLE console command build their opponent as a
// throwaway Pet and hatch it:
//
//     Pet foe; foe.dbgHatchAs(dex, false);
//
// hatch() ends with save(), and EVERY Pet shares the one "tamapoke" NVS
// namespace -- so that landed on the player's STORED creature, and
// registerSpecies() added the opponent to their Pokedex on the way past. The
// live object in RAM was untouched, so the game looked normal until the board
// rebooted or was flashed and came up with the FOE as the player's pet.
//
// Reported twice from real boards: once as "flashed and my current mon got
// replaced with an older one", and once here, where a single BATTLE command
// turned a player's creature into a level 1 ONIX.
#include "Arduino.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include <cstdio>
uint32_t g_seed=77; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false; bool wasPressed=false;
uint32_t millis(){return 0;} void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;} String FakeSerial::readStringUntil(char){return String("");}
void sfxPlay(uint8_t){}
static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

int main(){
  // a player, raising something, saved
  {
    Pet p; p.begin();
    p.dbgHatchAs(6, false);              // CHARIZARD
    p.ageMinutes = 60UL*72;              // level 73
    p.ivAtk=30; p.ivDef=29; p.ivSpe=28; p.ivHp=27;
    p.rename("REAL");
    p.saveNow();
  }
  // the battle builds its opponent exactly as the firmware does
  {
    Pet foe;
    foe.dbgHatchAs(95, false);           // ONIX, as Brock brings
    foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = 16;
    foe.ageMinutes = 60UL*13;
    foe.relearnFromLevel();
  }
  // reboot: what does the board come up with?
  {
    Pet p; p.begin();
    ck(p.speciesId == 6, "after a battle the stored creature is still the PLAYER's");
    ck(p.level() == 73, "at its own level, not the opponent's");
    ck(p.ivAtk==30 && p.ivDef==29 && p.ivSpe==28 && p.ivHp==27, "with its own IVs");
    ck(!strcmp(p.nick,"REAL"), "and its own name");
    ck(!p.isRegistered(95), "and the opponent is NOT added to the Pokedex for being fought");
    ck(p.isRegistered(6), "while the creature actually raised still is");
  }
  // and the guard must not break a real save
  {
    Pet p; p.begin();
    p.ageMinutes = 60UL*80;
    p.saveNow();
    Pet q; q.begin();
    ck(q.level() == 81, "an OPENED pet still saves normally (the guard is not too wide)");
  }
  // COPYING the live pet must not kill its save. buildSquad() does
  // `Pet tmp = pet;` to apply a level cap, and a copied Preferences shares the
  // NVS handle: ~Preferences() closed it, so every save after the FIRST gym fight
  // of a boot silently failed and the next reboot rolled the game back -- badges
  // won that session simply vanished.
  {
    Pet p; p.begin();
    p.winBadge(0, 0, false);
    {
      Pet tmp = p;                       // exactly what buildSquad() does
      tmp.ageMinutes = 60UL*5;           // ...and its level cap
      tmp.saveNow();                     // a copy must not write either
    }
    p.winBadge(0, 1, false);             // won AFTER the copy died
    p.winBadge(1, 2, true);
    p.ageMinutes = 60UL*90;
    p.saveNow();
    Pet q; q.begin();                    // the reboot
    ck(q.hasBadge(0, 0, false) && q.hasBadge(0, 1, false),
       "badges won after a Pet copy was made still reach NVS");
    ck(q.hasBadge(1, 2, true), "...including a non-Kanto hard badge");
    ck(q.level() == 91, "...and the live pet's other fields still save too");
  }
  printf("%s\n", bad?"FAILURES":"all good");
  return bad?1:0;
}
