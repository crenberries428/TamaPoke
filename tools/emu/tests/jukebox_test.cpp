// The jukebox: which songs unlock, what the screen lets you play, when the music
// stops on its own, and what listening is worth to the pet.
//
// Driven through the real paths -- onTap() for the menu and the rows, onSwipe()
// for paging, loop() for the auto-stop and for pet.musicOn, dbgTick() for the
// minutes -- so the CALLERS are tested, not just the rule. The listening joy is
// measured as the difference between two identical runs, one with a song on,
// so the awake drain cancels out instead of being restated here.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "audio.h"
#include <cstdio>
#include <cstring>
uint32_t g_seed=4242; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void onTap(int16_t x, int16_t y); void onSwipe(int dir); void onSwipeV(int dir);
uint8_t uiCurrentScreen();
extern const char *const SCREEN_NAME[];
extern Pet pet;
extern bool menuOpen, jukeOpen, screenOff;
extern uint8_t jukePage, dimStage;

static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}
static bool on(const char *scr){ return !strcmp(SCREEN_NAME[uiCurrentScreen()], scr); }

// geometry, as in the sketch: menu rows MENU_Y 75 + 16 + i*(44+6), jukebox
// rows 100 + i*56, 48 high. The jukebox is the third menu row.
static const int MENU_JUKE_Y = 75 + 16 + 2 * 50 + 22;
static int rowY(int i){ return 100 + i * 56 + 24; }

// one minute awake, with the needs pinned so the drain is the same every time
static void minute(){
  pet.fullness = 100; pet.hygiene = 100; pet.energy = 100; pet.poops = 0;
  pet.sleeping = false;
  pet.dbgTick();
}

// joy after n minutes from joy 50, with or without a song playing
static int joyAfter(int n, bool music){
  if (music) audioPlayTrack(TUNE_TITLE); else audioMusic(MUS_NONE);
  loop();                       // the sketch decides pet.musicOn from the audio
  pet.joy = 50;
  for (int i = 0; i < n; i++) minute();
  return pet.joy;
}

int main(){
  setup();
  for(int i=0;i<4;i++) render();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  dimStage = 0;
  const uint32_t DAY = 86400, T0 = 1767225600UL;   // 2026-01-01, noon below
  pet.setClock(T0 + 12 * 3600);

  // ---- unlocks, from a fresh player ----
  pet.checkTunes(false);
  ck(pet.tuneUnlocked(TUNE_TITLE), "the title screen is free from the start");
  ck(!pet.tuneUnlocked(TUNE_PALLET), "Pallet Town waits for a hatch");
  pet.dbgHatchAs(4, false);
  pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_PALLET), "hatching unlocks Pallet Town");
  ck(pet.tuneToast() && pet.tuneToastId == TUNE_PALLET, "...and announces it");
  ck(!pet.tuneUnlocked(TUNE_GYM) && !pet.tuneUnlocked(TUNE_LAVENDER) &&
     !pet.tuneUnlocked(TUNE_CENTER) && !pet.tuneUnlocked(TUNE_ROUTE1),
     "nothing else is unlocked yet");

  // ---- the screen: reached from the menu ----
  menuOpen = true;
  onTap(233, MENU_JUKE_Y);
  ck(jukeOpen && on("jukebox"), "the JUKEBOX menu row opens the jukebox");
  // page 0 is TITLE, PALLET, ROUTE1, CENTER, LAVENDER
  onTap(233, rowY(2));
  ck(audioCurrentTrack() == 0xFF, "a locked row plays nothing");
  onTap(233, rowY(1));
  ck(audioCurrentTrack() == TUNE_PALLET, "an unlocked row plays its song");
  onTap(233, rowY(0));
  ck(audioCurrentTrack() == TUNE_TITLE, "another row switches songs");
  onTap(233, rowY(0));
  ck(audioCurrentTrack() == 0xFF, "tapping the song that is on stops it");
  onTap(233, rowY(1));
  onSwipe(-1);
  ck(jukeOpen && jukePage == 1, "a horizontal swipe pages, keeping it open");
  onSwipeV(1);
  ck(!jukeOpen && on("main"), "a vertical swipe backs out to the pet");
  ck(audioCurrentTrack() == TUNE_PALLET, "and the song keeps playing after leaving");
  menuOpen = true; onTap(233, MENU_JUKE_Y);
  onTap(233, 440);
  ck(!jukeOpen, "a tap below the rows backs out too");

  // ---- the music stops for a dark screen and for bed ----
  audioPlayTrack(TUNE_PALLET);
  screenOff = true; loop(); screenOff = false;
  ck(audioCurrentTrack() == 0xFF, "turning the screen off stops the song");
  audioPlayTrack(TUNE_PALLET);
  pet.sleeping = true; loop(); pet.sleeping = false;
  ck(audioCurrentTrack() == 0xFF, "putting the pet to bed stops the song");
  // ...and the companion check, so the two above cannot pass vacuously
  audioPlayTrack(TUNE_PALLET); loop();
  ck(audioCurrentTrack() == TUNE_PALLET, "awake with the screen on, the song plays on");

  // ---- listening joy ----
  pet.listenMin = 0; pet.listenDay = 0;
  int quiet = joyAfter(LISTEN_DAILY_MIN, false);
  pet.listenMin = 0; pet.listenDay = 0;
  int heard = joyAfter(LISTEN_DAILY_MIN, true);
  ck(pet.musicOn, "loop() marks a playing song as music for the pet");
  int full = LISTEN_DAILY_MIN / LISTEN_EVERY * LISTEN_JOY;
  printf("      joy after %d min: %d quiet, %d with music (+%d expected)\n",
         LISTEN_DAILY_MIN, quiet, heard, full);
  ck(heard - quiet == full, "a song is worth LISTEN_JOY every LISTEN_EVERY minutes");
  int more = joyAfter(LISTEN_DAILY_MIN, true);
  ck(more == quiet, "past LISTEN_DAILY_MIN a day, listening is worth nothing more");
  pet.setClock(T0 + DAY + 12 * 3600);
  int tomorrow = joyAfter(LISTEN_DAILY_MIN, true);
  ck(tomorrow - quiet == full, "the cap resets the next day");
  pet.setClock(T0 + 2 * DAY + 12 * 3600);
  audioSetEnabled(false);
  int muted = joyAfter(LISTEN_DAILY_MIN, true);
  audioSetEnabled(true);
  ck(!pet.musicOn && muted == quiet, "a song with the sound switched off is not music");
  audioSetVolume(0);
  int silent = joyAfter(LISTEN_DAILY_MIN, true);
  audioSetVolume(50);
  ck(silent == quiet, "nor is one at volume 0");
  audioMusic(MUS_NONE); loop();

  // ---- the other unlock rules, each through the real state ----
  pet.winBadge(0, 0, false); pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_GYM) && pet.tuneUnlocked(TUNE_WIN), "a first badge unlocks the gym theme and the fanfare");
  ck(!pet.tuneUnlocked(TUNE_TRN), "...but not the trainer theme");
  for (uint8_t i = 1; i < 4; i++) pet.winBadge(0, i, false);
  pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_TRN) && !pet.tuneUnlocked(TUNE_WILD), "four badges unlock the trainer theme, not yet the wild one");
  pet.bestStreak = 3; pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_CENTER), "a 3-day streak unlocks the Pokemon Center");
  pet.setStepsToday(1000); pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_ROUTE1), "1000 steps in a day unlocks Route 1");
  pet.setStepsToday(0); pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_ROUTE1), "and it stays unlocked when the steps reset");
  pet.lastEnd = CER_FAREWELL; pet.dbgTick();
  ck(pet.tuneUnlocked(TUNE_LAVENDER), "a goodbye unlocks Lavender Town");

  // ---- player-wide: survives a new creature and a reload ----
  uint16_t before = pet.tunes;
  pet.newEgg();
  ck(pet.tunes == before, "a new egg keeps every song");
  pet.saveNow();
  Pet again; again.begin();
  ck(again.tunes == before, "the songs survive a reload");

  printf(bad ? "%d FAILED\n" : "all passed\n", bad);
  return bad ? 1 : 0;
}
