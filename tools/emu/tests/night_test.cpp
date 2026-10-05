// "Night" on the main screen follows the CLOCK, never the creature: a pet put to
// sleep at noon sleeps in daylight. render() used to OR pet.sleeping into gNight,
// which turned the whole sky, ground and UI to night the moment the light button
// was pressed in the middle of the day.
//
// Each case is paired with its opposite so the check cannot pass vacuously: if the
// harness never reached the scene, "asleep is not night" would be true for the
// wrong reason, so the night cases prove gNight really does change with the hour.
#include "Arduino.h"
#include "Arduino_GFX_Library.h"
#include "Preferences.h"
#include "pet.h"
#include <cstdio>
uint32_t g_seed=7; FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX=0,g_touchY=0; volatile bool g_touchDown=false;
void FakeESP::restart(){exit(0);}
int FakeSerial::available(){return 0;}
String FakeSerial::readStringUntil(char){return String("");}
void setup(); void render();
extern Pet pet;
extern bool gNight;

static int bad = 0;
static void expect(const char *what, bool asleep, int hour, bool wantNight) {
  pet.sleeping = asleep;
  pet.lastSeenEpoch = 1767225600UL + hour * 3600UL;   // 2026-01-01, `hour`:00 UTC
  render();
  if (gNight != wantNight) { printf("FAIL  %s: gNight=%d, wanted %d\n", what, gNight, wantNight); bad++; }
  else printf("PASS  %s\n", what);
}

int main() {
  setup();
  for (int i = 0; i < 4; i++) render();
  if (pet.awaitingStarter()) pet.chooseStarter(4);
  expect("awake at noon is day", false, 12, false);
  expect("ASLEEP at noon is still day", true, 12, false);
  expect("awake at 22:00 is night", false, 22, true);
  expect("asleep at 22:00 is night", true, 22, true);
  expect("awake at 03:00 is night", false, 3, true);
  expect("asleep at 07:00 is day", true, 7, false);
  expect("awake again at noon is day (nothing latched)", false, 12, false);
  printf(bad ? "FAILED\n" : "all passed\n");
  return bad ? 1 : 0;
}
