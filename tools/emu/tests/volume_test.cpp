// The volume page's slider. Drives the REAL touch path (setup()/loop()/
// handleTouch()), because what is pinned here is a gesture rule: a drag that
// begins on the bar sets the level and must NOT be resolved as a horizontal
// swipe, or sliding the knob would page the settings screen away.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include "party.h"
#include "audio.h"
#include <cstdio>
#include <chrono>
#include <thread>
uint32_t g_seed=1234; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void loop(); void render();
void clockTap(int16_t,int16_t);
extern Pet pet;
extern bool clockOpen;
extern uint8_t settingsPage, dimStage;

static int bad=0;
static void ck(bool ok,const char*w){printf("%s  %s\n",ok?"PASS":"FAIL",w); if(!ok)bad++;}

// geometry of the volume page, as in the sketch
#define SET_VOLUME 1
#define ROW_Y 214
#define BAR_X 110
#define BAR_W 246
#define BAR_Y (ROW_Y + 28 + 12)
#define MINUS_X 36
#define PLUS_X 370
#define TEST_X 242
#define SW_Y 130

static void pump(int n){ for(int i=0;i<n;i++){ std::this_thread::sleep_for(std::chrono::milliseconds(30)); loop(); } }
static void down(int x,int y){ g_touchX=x; g_touchY=y; g_touchDown=true; emuFireInterrupt(); pump(3); }
static void move(int x,int y){ g_touchX=x; g_touchY=y; emuFireInterrupt(); pump(3); }
static void up(){ g_touchDown=false; emuFireInterrupt(); pump(3); }

int main(){
  setup();
  for(int i=0;i<4;i++) render();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  dimStage = 0;
  clockOpen = true; settingsPage = SET_VOLUME;

  // touching the bar sets the level from where the finger is
  audioSetVolume(10);
  down(BAR_X + BAR_W/2, BAR_Y); up();
  ck(audioVolume() >= 48 && audioVolume() <= 52, "touching the middle of the bar sets about 50");

  // a long horizontal drag follows the finger, to both ends, without paging
  audioSetVolume(30);
  down(BAR_X + 20, BAR_Y);
  move(BAR_X + 120, BAR_Y);
  ck(audioVolume() >= 40 && audioVolume() <= 56, "mid-drag the level tracks the finger");
  move(BAR_X + BAR_W + 12, BAR_Y);
  up();
  ck(audioVolume() == 100, "dragging past the right end gives 100");
  ck(clockOpen && settingsPage == SET_VOLUME, "and the drag did not page or close the settings screen");

  down(BAR_X + 200, BAR_Y);
  move(BAR_X - 14, BAR_Y);
  up();
  ck(audioVolume() == 0, "dragging past the left end gives 0");
  ck(clockOpen && settingsPage == SET_VOLUME, "and still did not page");

  // the same swipe started OFF the bar still pages
  down(BAR_X + 200, 160); move(BAR_X + 20, 160); up();
  ck(settingsPage != SET_VOLUME, "a swipe that starts off the bar still pages");
  settingsPage = SET_VOLUME;

  // - and + step by 5 and clamp
  audioSetVolume(50);
  clockTap(PLUS_X + 20, ROW_Y + 20);
  ck(audioVolume() == 55, "+ adds 5");
  clockTap(MINUS_X + 20, ROW_Y + 20);
  clockTap(MINUS_X + 20, ROW_Y + 20);
  ck(audioVolume() == 45, "- subtracts 5");
  audioSetVolume(98); clockTap(PLUS_X + 20, ROW_Y + 20);
  ck(audioVolume() == 100, "+ clamps at 100");
  audioSetVolume(3); clockTap(MINUS_X + 20, ROW_Y + 20);
  ck(audioVolume() == 0, "- clamps at 0");

  // TEST leaves the level alone and the screen where it is
  audioSetVolume(40);
  clockTap(TEST_X + 20, SW_Y + 20);
  ck(audioVolume() == 40 && clockOpen && settingsPage == SET_VOLUME, "TEST changes nothing but the sound");

  printf("%s\n", bad ? "FAILED" : "all passed");
  return bad ? 1 : 0;
}
