// Checks the positional STRINGS table: a language row with too few entries is
// zero-padded by the compiler with no diagnostic, which silently shifts every
// string after the gap. Also enforces the ASCII-only rule (the bitmap font has
// no glyphs for accents) for every language except ZH and KO, whose non-ASCII
// text must be well-formed UTF-8 AND have a glyph in cjkfont.h -- a codepoint
// missing from the table draws as a hollow box on the panel.
#include "Arduino.h"
#include "Preferences.h"
// linked against the same core as every other suite, so it needs the same
// hardware stubs even though it only exercises the string table
uint32_t g_seed = 1;
FakeSerial Serial; FakeESP ESP; FakeWire Wire;
volatile int g_touchX = 0, g_touchY = 0;
volatile bool g_touchDown = false;
bool wasPressed = false;
uint32_t millis() { return 0; }
void FakeESP::restart() { exit(0); }
int FakeSerial::available() { return 0; }
String FakeSerial::readStringUntil(char) { return String(""); }
void sfxPlay(uint8_t) {}
#include "i18n.h"
#include "cjk.h"
#include <cstdio>
#include <cstring>
#include <string>

// STRINGS is file-static, so go through the same accessor the firmware uses.
static const char *LANG[] = { "ES", "EN", "FR", "DE", "IT", "PT", "ZH", "KO" };

int main() {
  int bad = 0;
  for (int l = 0; l < LANG_COUNT; l++)
    for (int s = 0; s < STR_COUNT; s++) {
      setLang((Lang)l);
      const char *v = T((StrId)s);
      if (!v) { printf("NULL  %s index %d\n", LANG[l], s); bad++; continue; }
      bool cjkLang = (l == LANG_ZH || l == LANG_KO);
      CjkState st;
      for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p < 0x80) { st.need = 0; continue; }
        if (!cjkLang) { printf("NON-ASCII  %s index %d: \"%s\"\n", LANG[l], s, v); bad++; break; }
        if (cjkFeed(st, *p) == 2 && !cjkGlyph(st.cp)) {
          printf("NO GLYPH  %s index %d U+%04X: \"%s\"  (run tools/gen_cjk.py)\n", LANG[l], s, (unsigned)st.cp, v);
          bad++; break;
        }
      }
    }
  // a translation must take the same printf arguments as the English original,
  // or snprintf reads the wrong type off the stack
  auto spec = [](const char *v) {
    std::string r;
    for (; *v; v++)
      if (*v == '%') {
        v++;
        while (*v && !strchr("sudclx%", *v)) v++;
        if (!*v) break;
        if (*v != '%') r += *v;
      }
    return r;
  };
  for (int s = 0; s < STR_COUNT; s++) {
    setLang(LANG_EN);
    std::string want = spec(T((StrId)s));
    for (int l = 0; l < LANG_COUNT; l++) {
      setLang((Lang)l);
      if (spec(T((StrId)s)) != want) {
        printf("FORMAT  %s index %d: \"%s\" takes different arguments than EN\n", LANG[l], s, T((StrId)s));
        bad++;
      }
    }
  }
  printf("%s: %d languages x %d strings\n", bad ? "FAIL" : "PASS", LANG_COUNT, STR_COUNT);
  return bad ? 1 : 0;
}
