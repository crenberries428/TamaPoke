// TamaPoke - pixel art tamagotchi inspired by gen 1
// for Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Libraries (Library Manager or the Waveshare repo):
//   - "GFX Library for Arduino" (moononournation), with CO5300 QSPI support
//   - "SensorLib" (Lewis He), CST9217 touch driver
//
// Board: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Sprites and the species table are generated with tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "types.h"
#include "moves.h"
#include "battle.h"
#include "trainers.h"
#include "link.h"
#include "linknow.h"
#include "backs.h"
#include "badges.h"
#include "avatars.h"
#include "cjk.h"
#include <stdarg.h>
#include "party.h"
#include "save.h"
#include "pet.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "imu.h"
#include "steps.h"
#include "i18n.h"
#include "logo.h"
#include "uifont.h"
#include "audio.h"

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION "3.14"
#define PET_CRY_GAP_MS 6000  // minimum time between cries from petting

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
// Canvas that also draws UTF-8 text (zh/ko): ASCII still goes through the stock
// 5x7 font, everything else from the generated 12x12 table in cjkfont.h.
#ifndef TP_EMU_CANVAS   // the emulator's stub canvas does this in print() itself
class TpCanvas : public Arduino_Canvas {
public:
  using Arduino_Canvas::Arduino_Canvas;
  size_t write(uint8_t c) override {
    int r = cjkFeed(cjk, c);
    if (r == 1) return Arduino_Canvas::write(c);
    if (r == 2) {
      // the hanzi is 12 rows against the stock font's 7, so lift it to stay level
      cjkDraw(*this, cursor_x, cursor_y - 2 * textsize_x, textsize_x, textcolor, cjk.cp);
      cursor_x += 12 * textsize_x;
    }
    return 1;
  }
private:
  CjkState cjk;
};
#endif
TpCanvas *gfx = new TpCanvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;
Pet pet;

// animated sprite from the SD for the current species (if the file exists)
SdMon mon;          // B/W sprite (fallback and minigame when there is no PMD)
PmdMon pmd;         // multi-action PMD sprite (main screen)
PmdMon evoPmd;      // previous form, only during the evolution blink
int16_t monFor = -2;
bool monShinyFor = false;

// on-screen behaviour of the creature
struct {
  uint8_t mode = 0;     // 0 idle, 1 walk, 2 one-shot gesture
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // start of the animation in progress
  uint32_t until = 0;   // end of the current state
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // the pet's ground line
#define PET_ZOOM 120    // main-screen creature zoom, percent (on top of the integer base scale)
#define PET_MAX_PX 205  // ...capped to this drawn height so it stays below the name
PmdMon galleryPmd;  // large sprite for the gallery detail view (PMD/TPK2, legal)

// pokedex gallery
bool galleryOpen = false;
bool galleryDirty = false;
// 16 to a page, and the Pokedex is browsed ONE REGION AT A TIME. Three
// generations flat is 25 pages of swiping to reach Hoenn, which is not a
// Pokedex, it is a scroll. A vertical swipe changes region and a horizontal one
// pages within it, so nothing is ever more than ten pages from the front.
// ALL is deliberately not offered here -- it is the thing being replaced.
#define GAL_PER_PAGE 16
#define GAL_REGIONS (REGION_COUNT - 1)          // the real regions, not ALL
#define GAL_LO (REGIONS[galleryRegion % GAL_REGIONS].lo)
#define GAL_HI (REGIONS[galleryRegion % GAL_REGIONS].hi)
#define GAL_SPAN (GAL_HI - GAL_LO + 1)
#define GAL_PAGES ((GAL_SPAN + GAL_PER_PAGE - 1) / GAL_PER_PAGE)
uint8_t galleryRegion = 0;
// Both the Pokedex and the gym ladder now open on a REGION CHOOSER rather than
// dropping you into whichever region was last viewed. The vertical swipe that
// changes region still works, but it is invisible, so on its own it meant the
// Johto and Hoenn content looked absent.
bool galleryPick = false;
bool gymPick = false;
uint8_t rpickPage = 0;      // the region chooser is paged; shared by all 3 modes
int galleryPage = 0;        // GAL_PAGES pages of GAL_PER_PAGE
int16_t galleryDetail = 0;  // dex in detail view, 0 = grid

bool screenOff = false;       // short press of the PWR button
bool cardOpen = false;        // creature card (vertical swipe)
bool kbOpen = false;
enum : uint8_t { KB_PET = 0, KB_TRAINER };
uint8_t kbTarget = KB_PET;          // keyboard for renaming the creature
char nameBuf[12] = "";
uint8_t nameLen = 0;
#define CARD_PAGES 4   // profile, stats, moves, progress -- medals moved to
                       // the player card, where the totals already live
uint8_t cardPage = 0;         // 0 profile, 1 stats+medals
// Menu overlay: opened by tapping the pet's name on the main screen. The
// horizontal swipe is already taken by the Pokedex (it pages through 10 pages
// internally), so a hub on that axis would be ambiguous; the header was inert
// and is the only free surface left.
bool menuOpen = false;
#define MENU_X 73
// Four rows: PARTY and GYMS came out, since a swipe right and a swipe left now
// reach them directly. Sized to the bezel -- the panel is 320 wide, so 160 from
// the centre, and sqrt(233^2 - 160^2) = 169 means it can only span y 64..402.
#define MENU_Y 75
#define MENU_W 320
#define MENU_H 316
#define MENU_ROW_H 52
#define MENU_ROW_GAP 6
// 5 rows: STATS / POKEDEX / SETTINGS / RETIRE / CLOSE. At MENU_Y 75 the panel
// spans 75..391 and the round display gives a half-width of 171 there against
// the 160 a row needs, so the corners stay on glass.
#define MENU_ROWS 5
#define MENU_ROW_Y(i) (MENU_Y + 16 + (i) * (MENU_ROW_H + MENU_ROW_GAP))

// Party screen. partyPick != 0 means the newcomer needs a slot: the player
// either taps someone to replace or lets it go.
bool partyOpen = false;
bool partyPick = false;
uint8_t partyDetail = 0;   // 0 = the grid, else slot + 1
// The box, reached from the party screen. `boxSwapFrom` is the party slot
// waiting for something to trade with, 0 when nothing is pending.
bool boxOpen = false;
uint8_t boxPage = 0;
uint8_t boxSwapFrom = 0;   // party slot + 1, armed from the party side
uint8_t boxSel = 0;        // box slot + 1, armed from the box side
// The box gets the same detail sheet the party has. Tapping a box slot used to
// yank the creature into the party immediately, which is a surprising amount to
// happen from one tap -- and left nowhere to put a RELEASE button.
uint8_t boxDetail = 0;        // box slot + 1 whose sheet is open
// The RELEASE confirm, on whichever sheet is up. One flag rather than two,
// because only one sheet can be open at a time and partyDetail/boxDetail
// already say which.
bool releaseConfirm = false;
#define BOX_PER_PAGE 6
uint32_t partyBannerUntil = 0;   // "<name> joined the party!"
char partyBannerName[14] = "";
#define PARTY_CELL_W 150
#define PARTY_CELL_H 70
#define PARTY_GRID_X 78
#define PARTY_GRID_Y 88

bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
int clockH = 12, clockM = 0;  // hora en edicion
// settings pages, swiped left/right
enum { SET_TIME, SET_VOLUME, SET_BRIGHT, SET_LANG, SET_ABOUT, SET_PAGES };
uint8_t gBright = 7;           // screen brightness 1..10, saved under "bri"
uint8_t settingsPage = 0;

// bath scene: foam over the creature, cleaned up when it bursts
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // food picker open until this millis

// "taps" minigame: keep the pokeball in the air
bool gameOpen = false;
uint32_t gameOverUntil = 0;
// The ball game used to run until you missed three times or walked away, so a
// session had no length and the reward no shape. The bag is 10 s and the
// reaction test 15 s; this sits between them, and whichever comes first --
// the clock or three misses -- ends it.
#define GAME_MS 20000UL
uint32_t gameUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore, gameMisses;
float hitX, hitY;             // last hit (impact ring)
uint32_t hitTime = 0;
bool gameNewHi = false;

// punching bag (trains strength)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;

// training submenu (the 5th icon): routes to the trainer for each stat.
// DEF has no minigame -- it rises on its own from good wellbeing -- so its row
// is informational and does not respond to a tap.
bool trainOpen = false;

// move picker, opened from the MOVES card page. Most learnsets are level 0, so
// a level-up "you learned a move" prompt would almost never fire -- the moveset
// is edited on demand instead.
bool movePickOpen = false;
uint8_t movePickSlot = 0;   // which of the 4 slots is being replaced
uint8_t movePickParty = 0;  // 0 = the live pet, else the party slot + 1
uint8_t movePickPage = 0;
#define MOVE_ROW_Y(i) (96 + (i) * 58)
#define MOVE_PICK_PER_PAGE 5
#define MOVE_PICK_Y(i) (76 + (i) * 58)
// level-up learn prompt: modal, and deliberately without a timeout -- it
// decides what the creature is for the rest of its life, and once banked into
// the party, forever.
#define LEARN_ROW_Y(i) (104 + (i) * 56)
#define LEARN_SKIP_Y 334

// ---------- battle ----------
// The move menu is a 2x2 grid rather than four stacked rows: the round panel
// has to fit both creatures, both HP bars and the menu, and four full-width
// rows do not leave room for the sprites.
bool battleOpen = false;

enum : uint8_t {
  SCR_STARTER = 0, SCR_REGION, SCR_GALLERY, SCR_DEXPICK, SCR_MOVEPICK, SCR_BOX,
  SCR_PARTY, SCR_KEYBOARD, SCR_CARD, SCR_PLAYER, SCR_CLOCK, SCR_GYM, SCR_GYMPICK,
  SCR_LAN, SCR_PICK, SCR_BATTLE, SCR_WIN, SCR_LEARN, SCR_TRAIN, SCR_MENU,
  SCR_GAME, SCR_MAIN, SCR_COUNT
};
extern const char *const SCREEN_NAME[SCR_COUNT];   // const is internal linkage in C++

// Declared HERE, above every use. tools/emu/build.sh generates a proto.h with
// every prototype at the top, so the emulator compiled these happily while
// arduino-cli -- which relies on the IDE's auto-prototyping and does not always
// produce one -- did not. A green emulator build is not proof the firmware
// builds; only arduino-cli is.
void bootReport();
uint8_t uiCurrentScreen();
// Declared here because renderMonSheet() and the sheet's tap handlers use both
// ~4000 lines above where they are defined. The emulator generates a proto.h and
// would compile it either way; arduino-cli would not, and that has shipped once.
void uiConfirmRects(int *b1Top, int *b1Bot, int *b2Top, int *b2Bot);
void renderParty();
void renderBox();
void drawConfirmPanel(const char *q, const char *sub1, const char *sub2,
                      uint16_t subCol, const char *o1, uint16_t c1, uint16_t t1,
                      const char *o2, uint16_t c2, uint16_t t2);
const char *const SCREEN_NAME[SCR_COUNT] = {
  "starter", "region", "gallery", "dexpick", "movepick", "box",
  "party", "keyboard", "card", "player", "clock", "gym", "gympick",
  "lan", "pick", "battle", "win", "learn", "train", "menu",
  "minigame", "main"
};

// Badge art for a region, or nullptr when that region has none yet.
//
// NEVER `BADGES_ART[region % BADGE_REGIONS]`. The moment GYM_REGIONS outgrew
// BADGE_REGIONS -- which happened when Kalos landed and the upstream badge set
// stops at Unova -- 5 % 5 = 0 silently dressed Kalos in KANTO's badges, on both
// the win screen and the player card. A wrong badge is worse than an honest
// blank: it claims you won something you did not. The callers draw a
// placeholder instead, and static_assert below stops the array being indexed
// out of range whatever the two counts are.
static_assert(BADGE_REGIONS <= GYM_REGIONS,
              "more badge sheets than ladders -- BADGES_ART would be indexed by a region that does not exist");
static const BadgeArt *badgeArtFor(uint8_t region, uint8_t i) {
  if (region >= BADGE_REGIONS || i >= TRAINER_GYMS) return nullptr;
  return &BADGES_ART[region][i];
}

// Does this region have its own badge art? Split out with a primitive return
// type so the tests can ask: the emulator's genproto.py emits every prototype
// ABOVE the includes, where the BadgeArt struct does not exist yet.
bool badgeArtExists(uint8_t region, uint8_t i) { return badgeArtFor(region, i) != nullptr; }

Combatant btlYou, btlFoe;
bool btlOver = false;
bool btlWon = false;
bool btlNewBadge = false;
uint32_t btlWinUntil = 0;   // the win screen is up
// A trainer fight is a run of 1v1s: both sides queue their squad and the next
// one steps up when the current one faints. This is the whole difficulty curve
// -- no gating, just attrition, so one strong creature sweeps Brock and dies
// four deep into Lance.
// The ladder is now sequential: a leader opens once the previous one is beaten,
// tracked per difficulty so hard mode is its own run. This replaces the earlier
// "no gating, attrition is the gate" rule -- with both ladders level-capped,
// nothing stopped you opening with Lance and simply losing, which read as a
// dead end rather than a challenge.
// reaction test (trains SPEED)
bool spdOpen = false;
uint32_t spdUntil = 0, spdOverUntil = 0, spdBorn = 0;
int16_t spdX = 0, spdY = 0;
uint16_t spdHits = 0, spdMisses = 0;
uint8_t spdGain = 0;
bool spdNewHi = false;

bool gymOpen = false;
bool gymHard = false;   // which ladder the list is showing

// LAN battle. `lanOpen` is the pairing screen; once both squads are known the
// normal battle screen takes over with btlLink set.
bool lanOpen = false;
Link lan;
// What the shared region chooser is being used FOR. It only changes the
// subtitle and whether there is a way back: at first boot every count would
// read zero, which tells the player nothing, and there is nowhere to go back to.
#define RPICK_FOR_GYMS  0
#define RPICK_FOR_DEX   1
#define RPICK_FOR_START 2
// Three rows is what the round panel fits above the BACK label. The dex now
// lists four regions and will list more, so the chooser PAGES rather than
// growing -- and every paged screen in this sketch has to be driven by
// swipe_test, which is why that test exists at all.
#define RPICK_PER_PAGE  3
extern uint8_t rpickPage;
static void renderRegionPick(uint8_t mode);   // the region chooser, defined below
// Not static: swipe_test drives these so it asks the FIRMWARE for the page
// count instead of recomputing it from its own copy of RPICK_PER_PAGE, which
// would prove the transcription rather than the screen.
uint8_t rpickRegions(uint8_t mode);           // rows this mode lists (gyms: 3)
uint8_t rpickPageCount(uint8_t mode);
uint8_t rpickModeNow();                       // which chooser is up, or 0xFF
static bool rpickSwipe(int dir);              // true if it handled the gesture
static int regionPickTap(int16_t x, int16_t y, uint8_t mode);
static void drawEggRegion();          // defined with the egg screen helpers
static int eggRegionTap(int16_t x, int16_t y);
static void drawBtlBack();
static void btlLinkPoll();   // defined with the battle code, called from render()
static void btlSwitchTo(uint8_t i);
static void btlResolve(uint8_t yourMove);
// The peer's whole team, kept live. A trainer's replacements are built fresh
// from TRAINERS[] because they only ever arrive once; a linked opponent can
// switch OUT and back IN, so its creatures have to remember how battered they
// are. Host side only -- the guest takes absolute health off the wire.
Combatant btlFoeSquad[TRAINER_TEAM_MAX];
uint8_t btlFoeSquadN = 0;
uint8_t btlMyAct = 0;        // host: our own action, latched until theirs lands
// Which ladder the gym screen and the current fight belong to. The battle keeps
// its own copy so that leaving the gym list mid-fight cannot retarget the badge.
// The BOX button on the party screen. It was 150x32 with a pixel-exact hit
// test, which is a small target on a round panel -- reported as hard to press,
// same complaint as the battle grid. Now bigger AND padded: the drawn size grew
// too, so the button looks like the size it actually is rather than hiding a
// generous hit area behind a small graphic.
// The smallest a button may be. Three separate "hard to hit" reports -- the
// battle grid's bottom row, the party screen's BOX, and the LAN button -- were
// all the same mistake: a control sized to fit its label rather than a finger.
// 44 px is the usual guidance and roughly a fingertip on this 466 px panel.
#define UI_TAP_MIN 44

// The LAN battle button on the gym region chooser.
#define LANBTN_W 190
#define LANBTN_H UI_TAP_MIN
#define LANBTN_X (233 - LANBTN_W / 2)
#define LANBTN_Y 336

#define BOXBTN_X 146
#define BOXBTN_Y 320
#define BOXBTN_W 174
#define BOXBTN_H UI_TAP_MIN
#define BOXBTN_PAD 8
// CLOSE sits below BOX with a real gap between them. They used to touch at
// y=372, and BOX's padding then reached to 380 -- so the top of CLOSE was
// inside BOX's hit area and taps there opened the box instead of closing the
// screen. Padding one button into its neighbour just moves the problem along.
#define PARTYCLOSE_Y 376
#define PARTYCLOSE_H UI_TAP_MIN
#define PARTYCLOSE_X 133
#define PARTYCLOSE_W 200

// The confirm panel, in ONE place. Three callers draw it -- the evolve/farewell/
// retire dialog on the main screen, and letting a banked creature go from the
// party or box sheet -- and hit_test asserts against the same numbers through
// uiConfirmRects(). Copies of this geometry in the tap handler are exactly how
// a YES button ends up somewhere the drawing is not.
#define CONFIRM_X 73
#define CONFIRM_Y 126   // grown upward so two TINY cost lines fit above the buttons
#define CONFIRM_W 320
#define CONFIRM_H 218
#define CONFIRM_BTN_X 93
#define CONFIRM_BTN_W 280
#define CONFIRM_BTN_H 52
#define CONFIRM_B1_Y 206
#define CONFIRM_B2_Y 268

// The two buttons on the party/box detail sheet. A third full-width row would
// not fit above BACK, and BRING BACK was h=38 -- under UI_TAP_MIN, the shape
// section 4 of CLAUDE.md keeps warning about.
//
// THEY ARE NOT EQUAL HALVES, and that is the whole point. The first version
// centred the pair on 233 with a dead gap between them -- which put the gap at
// the CENTRE OF THE PANEL, the one place a thumb naturally lands, so BRING BACK
// "did not work" and was reported as broken. Worse, every build up to v3.5 drew
// BRING BACK as a FULL-WIDTH button centred exactly there, so muscle memory
// aimed straight at the dead zone.
//
// The primary action now owns the centre. RELEASE is narrower, offset right,
// and still over UI_TAP_MIN -- it is irreversible, so it should be reachable
// but never the thing you hit by aiming at the middle. The gap between them is
// wider than before, not narrower.
#define PDET_BTN_Y 336
#define PDET_BTN_H 48
#define PDET_L_X 70
#define PDET_L_W 180
#define PDET_R_X 266
#define PDET_R_W 120
static_assert(PDET_L_X < 233 && 233 < PDET_L_X + PDET_L_W,
              "the panel centre must land on the sheet's PRIMARY button, not between the two");
static_assert(PDET_R_X > PDET_L_X + PDET_L_W + 8,
              "the destructive button needs a real dead gap before it");

uint8_t gymRegion = 0;
uint8_t btlRegion = 0;
#define TRAINERS (TRAINER_SETS[gymRegion % GYM_REGIONS].list)
#define BTL_TRAINERS (TRAINER_SETS[btlRegion % GYM_REGIONS].list)
bool gShowAllAvatars = false;  // emulator screenshot aid, never set on hardware
bool btlPetIn = false;       // was the live pet in the squad?
uint8_t btlTrainGain = 0;    // what the win trained, for the win screen
uint8_t btlTrainWhich = 0;
bool btlLink = false;      // this fight is against another device
bool btlLinkHost = false;
static bool gymUnlocked(uint8_t idx, bool hard) {
  return idx == 0 || pet.hasBadge(gymRegion, idx - 1, hard);
}

// Team select. Candidate 0 is the live pet, 1..PARTY_SLOTS are the banked
// members, so one bitmask covers the whole pool.
bool pickOpen = false;
// The team picker serves the gym ladder and the LAN screen both. PICK_LAN is
// not a trainer index: squadCap() already returns an uncapped six for anything
// past the roster, which is what a LAN battle wants -- two players who know
// each other can bring what they like.
#define PICK_LAN 0xFF
static void lanOffer(bool host);
uint8_t pickTrainer = 0;
bool pickHard = false;
bool lanWantHost = true;   // which button opened the picker
uint16_t squadMask = 0xFFFF;   // everything, until the player says otherwise
uint8_t pickPage = 0;
#define PICK_PER_PAGE 6
#define PICK_CELL_W 150
#define PICK_CELL_H 74
#define PICK_X(i) (78 + ((i) % 2) * (PICK_CELL_W + 10))
#define PICK_Y(i) (86 + ((i) / 2) * (PICK_CELL_H + 6))
#define PICK_GO_Y 350
// BACK beside FIGHT on the team-select screen. There was no way out of it but a
// swipe, which is invisible -- the same complaint as everywhere else.
#define PICK_BTN_W 155
#define PICK_BTN_H UI_TAP_MIN
#define PICK_BACK_X (233 - PICK_BTN_W - 7)
#define PICK_GO_X (233 + 7)
bool playerOpen = false;
// One badge page per gym region, then the medals. Three ladders will not fit on
// one page, and the page you are on IS the region -- no extra control needed,
// and horizontal paging already works everywhere else.
uint8_t playerPage = 0;
#define PLAYER_PAGES (GYM_REGIONS + 1)
#define playerBadgeRegion (playerPage % GYM_REGIONS)
uint8_t gymPage = 0;
#define GYM_ROWS 5
#define GYM_ROW_Y(i) (110 + (i) * 50)
int8_t btlTrainer = -1;      // index into TRAINERS, -1 = a one-off fight
bool btlHard = false;
Combatant btlSquad[TRAINER_TEAM_MAX + 1];
uint8_t btlSquadN = 0, btlSquadAt = 0;
uint8_t btlFoeAt = 0;

// Animation. Deliberately built on the thumbnails the screen already draws
// rather than on PmdMon: three PmdMon blobs are live already, and the battle
// has to stay graceful on a board with no SD at all (S_NO_SPRITES). Index 0 is
// you, 1 is the foe.
uint32_t btlLungeUntil[2] = { 0, 0 };   // acted: leans toward the opponent
uint32_t btlHitUntil[2] = { 0, 0 };     // was hit: jitters and flashes
uint16_t btlHpShown[2] = { 0, 0 };      // bars ease toward the real value
// Two streamed sprites, so the creatures can actually swing and flinch. They
// cost ~135 KB of PSRAM each on average and are freed when the fight ends. The
// player's side is NOT the global `pmd`: the active creature may be a banked
// party member rather than the live pet.
PmdMon btlPmd[2];
int16_t btlPmdDex[2] = { 0, 0 };
// A faint used to swap the next creature in instantly, inside the same call
// that resolved the turn -- which is why it felt like a jump cut. The swap is
// now deferred: the fainted one drops out of frame, and the replacement slides
// in only once the player dismisses that message.
uint32_t btlFaintUntil[2] = { 0, 0 };
uint32_t btlEnterUntil[2] = { 0, 0 };
int8_t btlSwapWho = -1;        // 0 = your side, 1 = the foe's, -1 = nothing due
#define BTL_FAINT_MS 700
#define BTL_ENTER_MS 420
// battle menu: 0 = FIGHT/POKEMON, 1 = the moves, 2 = the switch list
uint8_t btlMenu = 0;
#define BTL_LUNGE_MS 260
#define BTL_HIT_MS 420
char btlMsg[6][40];
uint8_t btlMsgCount = 0;   // queued lines; a tap shows the next
#define BTL_CELL_W 160
#define BTL_CELL_H 44
#define BTL_GRID_X 69
#define BTL_GRID_Y 274
#define BTL_CELL_X(i) (BTL_GRID_X + ((i) % 2) * (BTL_CELL_W + 8))
#define BTL_CELL_Y(i) (BTL_GRID_Y + ((i) / 2) * (BTL_CELL_H + 8))

// A cell's HIT area is bigger than the cell that is drawn. On the board the two
// bottom buttons were much harder to hit than the top two: the drawn cells are
// only 44 px tall, there was an 8 px dead gap between the rows, and everything
// below the bottom row was dead too -- so a finger landing low, or a touch panel
// reading a few pixels high, missed entirely. Nothing else is tappable in this
// area while the grid is open, so the slop costs nothing.
//
// The gap between the two rows and the two columns is split down the middle, and
// the bottom row additionally claims the empty space beneath it.
#define BTL_HIT_PAD 4
// The bottom row keeps extra room downward, but not so much that it reaches the
// BACK bar below it -- the mistake made once already with BOX and CLOSE.
#define BTL_HIT_BOTTOM 6
// BACK, under the grid: the move and switch screens had no way out except
// choosing something.
// The gym list's EASY/HARD pill. It was 24 px tall, which is half a fingertip.
// Sits between the title and the first leader row. At 72 with a 44 px height it
// ran to 116 and overlapped the first row, which begins at 110 -- introduced
// when the pill was enlarged to a real tap target.
#define GYMDIF_Y 60
#define GYMDIF_H UI_TAP_MIN
#define BTL_BACK_W 190
#define BTL_BACK_H UI_TAP_MIN
#define BTL_BACK_X (233 - BTL_BACK_W / 2)
#define BTL_BACK_Y 384
#define BTL_HIT_X0(i) (BTL_CELL_X(i) - BTL_HIT_PAD)
// The far edges stop one pixel short so the four boxes TILE: the gap between
// two cells is split down the middle with no pixel left over and none shared.
#define BTL_HIT_X1(i) (BTL_CELL_X(i) + BTL_CELL_W + BTL_HIT_PAD - 1)
#define BTL_HIT_Y0(i) (BTL_CELL_Y(i) - BTL_HIT_PAD)
#define BTL_HIT_Y1(i) (BTL_CELL_Y(i) + BTL_CELL_H - 1 + \
                       ((i) / 2 ? BTL_HIT_BOTTOM : BTL_HIT_PAD))

// Which cell a point falls in, or -1. Exposed (not static) so a test can sweep
// the panel and prove there are no dead pixels between the cells -- the bug that
// made the bottom row hard to press was a gap, not a wrong rectangle.
int btlCellIndexAt(int16_t x, int16_t y);

// Used by BOTH the move grid and the switch grid. They had a copy each of the
// same rectangle test, which is exactly how two halves of one control drift.
static inline bool btlCellHit(int i, int16_t x, int16_t y) {
  return x >= BTL_HIT_X0(i) && x <= BTL_HIT_X1(i) &&
         y >= BTL_HIT_Y0(i) && y <= BTL_HIT_Y1(i);
}
#define TRAIN_X 73
#define TRAIN_Y 96
#define TRAIN_W 320
#define TRAIN_H 274
#define TRAIN_ROW_H 56
#define TRAIN_ROW_GAP 8
#define TRAIN_ROW_Y(i) (TRAIN_Y + 54 + (i) * (TRAIN_ROW_H + TRAIN_ROW_GAP))

// the 9 species with their own sprite in flash (fallback without SD): dex -> index
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centre of the round screen
#define CY 233
#define PET_CY 202  // vertical centre of the sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

// icon buttons following the lower arc of the round screen
// (the outer ones sit higher so they stay inside the circle)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
// Five across the arc: spacing tightened 62 -> 54 so the outer pair stays far
// enough in to keep its old y. Lifting them instead would have run the row into
// the ENE/HYG bars, which end at y=361 -- the buttons are 52 tall, so any centre
// above 387 overlaps them.
// Four, not five. The ball had its own icon here until it became DEFENCE's
// trainer and moved into the training menu -- at which point tapping it just
// opened the same menu the dumbbell does, two icons for one destination.
// They sit on the panel's curve: y = 406 - dx^2/729.
// What the creature is eating, remembered from the food picker so the animation
// can draw it (0 apple, 1 blueberry, 2 pear, 3 candy). Not saved.
static uint8_t eatItem = 0;

#define BTN_COUNT 4
// Referred to by NAME, never by literal index. Removing the ball icon shifted
// every index by one and drawButtons() still had `i != 2` meaning LIGHT -- which
// silently made the BATH button the one that wakes the pet.
#define BTN_FOOD  0
#define BTN_LIGHT 1
#define BTN_BATH  2
#define BTN_TRAIN 3
Btn buttons[BTN_COUNT] = {
  { 134, 393, SPR_ICON_FOOD },   // eat
  { 200, 405, SPR_ICON_LIGHT },  // light
  { 266, 405, SPR_ICON_CLEAN },  // bath
  { 332, 393, SPR_ICON_TRAIN },  // train
};
#define BTN_HALF 30  // 60x60 button -- bigger and more widely spaced than before
#define BTN_HIT 40   // touch radius (a little more generous)

// egg cracks ('k' pixels over the sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// night mode stars: positions are rolled once per boot (see drawStars)
#define STAR_COUNT 40

bool wasPressed = false;
// starter choice (first game): Bulbasaur / Charmander / Squirtle, 3 rows
// The first-boot starter list is the FRONT of each region's starter array in
// dex.h -- not a copy of it. That array is also the pool a region's first egg
// is drawn from (pet.cpp rollInRegion), where Kanto's five deliberately include
// Pikachu and Eevee; the choice screen shows the canonical three and leaves the
// rest to the egg. starter_test pins the first three of every region, so
// reordering that array cannot silently change the first screen anyone sees.
#define STARTER_SHOWN 3
int16_t starterOf(uint8_t region, uint8_t i) {
  const RegionInfo &rg = REGIONS[region % REGION_COUNT];
  if (i >= rg.starterCount) i = 0;
  return rg.starters[i];
}
uint8_t starterCountShown(uint8_t region) {
  uint8_t n = REGIONS[region % REGION_COUNT].starterCount;
  return n < STARTER_SHOWN ? n : STARTER_SHOWN;
}

// First boot runs region -> starter. This is NOT persisted: a reset between the
// two lands back on the region, which is the harmless direction to fail in --
// nothing has been chosen yet, and pet.setRegion() is idempotent.
static bool starterRegionDone = false;
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// evolution CTA button (centred, half the screen)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// farewell CTA button (wider: carries the name + phrase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// the CST9217 signals touch data on the INT pin; we use it to avoid
// reading the I2C bus while the chip is asleep (that read hung for ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
// AMOLED protection: dimmed on inactivity
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 despierto, 1 atenuado (90s), 2 casi apagado (5min)
bool swallowGesture = false; // el toque que despierta no acciona nada
bool sliderDrag = false;     // this touch began on the volume slider
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

// Boot splash: the title in the ABOUT page's logo colours, shown while the rest
// of setup() runs. Skipped after a crash restart -- the player is already inside
// a game and wants it back, not a title card.
#define SPLASH_MS 2000   // closes itself after this long
// The wordmark bitmap, pre-rendered by tools/gen_logo.py (2 bpp index image),
// centred on cy. Shared by the ABOUT page and the boot splash so they match.
void drawLogo(int cy) {
  const uint16_t pal[4] = {0, 0xFE40, 0x3A79, 0x10A8};   // clear, yellow, blue, navy
  const int x0 = CX - LOGO_W / 2, y0 = cy - LOGO_H / 2;
  for (int y = 0; y < LOGO_H; y++)
    for (int x = 0; x < LOGO_W; x++) {
      int i = y * LOGO_W + x;
      uint8_t v = (LOGO_BITS[i >> 2] >> ((i & 3) * 2)) & 3;
      if (v) gfx->fillRect(x0 + x, y0 + y, 1, 1, pal[v]);
    }
}

void drawSplash() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  drawLogo(CY);
  uiText(UIF_TINY, CX, CY + LOGO_H / 2 + 24, "by C.R.", UI_TRACK_TEXT, 1);   // quiet credit
  gfx->flush();
}

void setup() {
  Serial.setRxBufferSize(8192);  // the SD transfer arrives in 2 KB blocks
  Serial.begin(115200);
  // CRITICAL: without this, Serial.print BLOCKS the game when no serial
  // monitor is open on the host (the USB CDC TX buffer fills up
  // and nobody drains it) -> with timeout 0 the messages are dropped
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  bootReport();   // why the last run ended, and what it was doing
  loadLang();  // saved language (ES by default)
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (touch), AXP2101 (PMU) and PCF85063 (RTC) share this I2C bus.
  // Safety net for PMU/RTC (SensorLib does NOT honour this timeout on the
  // touch chip; the hang of the sleeping touch chip is solved by gating on INT, see
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICAL: power up the panel supply (BLDO1=OLED VDD 3.3V) BEFORE
  // initialising the display. If the PMU was reset (total drain), this rail
  // stays OFF and the screen looks black even though the rest of the board works.
  pmuEnablePanel();

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  {
    Preferences bp;
    bp.begin("tamapoke", true);
    gBright = bp.getUChar("bri", 7);
    bp.end();
    if (gBright < 1 || gBright > 10) gBright = 7;
  }
  panel->setBrightness(gBright * 255 / 10);

  const int bootReason = (int)esp_reset_reason();
  const bool crashed = (bootReason == ESP_RST_PANIC || bootReason == ESP_RST_INT_WDT ||
                        bootReason == ESP_RST_TASK_WDT || bootReason == ESP_RST_WDT ||
                        bootReason == ESP_RST_BROWNOUT);
  const uint32_t splashAt = millis();
  if (!crashed) drawSplash();

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // sometimes fails on the first attempt
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 not detected");
  // begin() leaves the chip in command mode (reads the identity and does not leave it);
  // a hardware reset is needed for it to report touches again
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // the panel is mounted rotated 180 degrees
  // active-low INT: fires when there is data. Gates the I2C reads (see loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  party.begin();
  pet.begin();
  sdBegin();
  thumbs.load();

  // real clock: apply the time it was switched off
  rtcBegin();
  batBegin();
  pwrSetup();
  imuBegin();
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // blank RTC: seed (the absolute time does not matter,
    e = rtcEpoch();             // only the differences do)
    Serial.println("RTC has no time: seeded, no offline progression this time");
  }
  pet.syncClock(e);

  audioBegin();  // ES8311 + I2S + amplifier (plays a boot jingle)

  lastInteract = millis();

  // hold the splash for its full time even if setup() finished sooner
  if (!crashed) {
    uint32_t used = millis() - splashAt;
    if (used < SPLASH_MS) delay(SPLASH_MS - used);
  }
}

// loads/unloads the SD sprite when the species changes
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // primary: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // fallback: B/W
  }
}

void loop() {
  uint32_t now = millis();
  pet.update(now);

  // Steps are sampled on every pass, screen on or off: a board in a pocket is
  // the case this exists for. A render frame is ~100 ms, so samples arrive
  // unevenly; StepDetector takes the timestamp and does not care.
  static StepDetector stepper;
  float ax, ay, az;
  if (imuReadAccel(ax, ay, az)) pet.addSteps(stepper.feed(ax, ay, az, now));

  // The link is pumped here rather than from the LAN screen, because it has to
  // keep running through the battle too: linkNowPoll() drains what the radio
  // parked on the WiFi task, and tick() is what resends a lost packet and gives
  // up on a peer that has gone quiet.
  if (lan.live()) {
    linkNowPoll();
    lan.tick(now);
  }

  // plays a sound when the creature becomes ready to evolve
  // (includes the case of reaching it on waking). canEvolveNow is false while asleep.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // gloomy warning when the creature is about to run away from neglect
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  handleTouch();
  handleSerial();
  ensureMon();

  // A sprite pack can arrive long after the card was mounted: the web installer
  // streams it over PUT into the FIRMWARE THAT IS ALREADY RUNNING. gRegionArt was
  // computed once in sdBegin(), so without this the region a player just spent ten
  // minutes downloading stayed greyed out reading NEEDS PACK until they rebooted --
  // indistinguishable, from the player's side, from the download having failed.
  // Quietly, because the host is still parsing this serial stream.
  if (sdArtDirty) {
    sdArtDirty = false;
    sdScanRegionArt(false);
  }

  // A farewell or release just finished: the creature is waiting for a slot.
  // With room it simply joins; with a full party the player is taken straight
  // to the party screen to choose who it replaces, or to let it go.
  if (pet.endedKind != CER_NONE && !partyPick) {
    // party first, then the box; only a full box makes it your choice
    if (party.add(pet.endedMon) || party.boxAdd(pet.endedMon)) {
      snprintf(partyBannerName, sizeof(partyBannerName), "%s",
               pet.endedMon.nick[0] ? pet.endedMon.nick : DEX_TBL[pet.endedMon.dex].name);
      partyBannerUntil = now + 3500;
      pet.endedKind = CER_NONE;
      sfxPlay(SFX_MEDAL);
    } else {
      partyPick = true;
      partyOpen = true;
      menuOpen = false;
    }
  }

  // short press of PWR: screen on/off
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      screenOff = !screenOff;
      pet.setScreenOff(screenOff);   // asleep only if it is also night
      if (!screenOff) lastInteract = now;
    }
  }

  updateBrightness(now);

  // flush the periodic autosave ONLY with the screen dimmed/off or
  // asleep: the NVS write freezes both cores for ~1s (flash cache off),
  // and there is no animation to cut or finger waiting for a response here. With 90s
  // of inactivity the screen already dims, so it flushes right away; active
  // use persists anyway through the saves of each action (eat/play/...).
  if (pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }

  // records the real time every 30 s (persisted on every game save)
  static uint32_t lastClock = 0;
  if (now - lastClock > 30000) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  // health heartbeat every 5 min (for the soak test; discarded when no monitor)
  static uint32_t lastHealth = 0;
  if (now - lastHealth > 300000) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u\n", (unsigned long)(now / 1000),
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
  }

  // 85 ms in game/bag: safe margin so the redraw does not step on the DMA
  // send of the previous frame (at 40-65 ms it overlapped and caused black flashes; with
  // large sprites drawing takes longer, so a cushion is left)
  if (now - lastRender >= (uint32_t)((gameOpen || sackOpen || spdOpen) ? 85 : 100)) {
    lastRender = now;
    render();
  }
}

// brightness according to sleep + inactivity (AMOLED protection)
void updateBrightness(uint32_t now) {
  // visible events wake the screen by themselves
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) {
    lastInteract = now;
  }
  uint32_t idle = now - lastInteract;
  dimStage = (idle > 300000) ? 2 : (idle > 90000) ? 1 : 0;
  // the player's level (1..10) sets the awake brightness; the sleep and idle
  // dim levels below it only ever go DOWN from it, never above
  uint8_t base = (uint8_t)(gBright * 255 / 10);
  uint8_t target = pet.sleeping ? 25 : base;
  if (dimStage == 1) target = pet.sleeping ? 10 : (base < 60 ? base : 60);
  else if (dimStage == 2) target = 8;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

// ---------- serial console (SD provisioning + debugging) ----------

// The values are what dex.h stores in `DexEntry::biome` (see TYPE_BIOME in
// tools/gen_dex.py): append new ones, never reorder.
enum Biome : uint8_t {
  BIOME_MEADOW, BIOME_BEACH, BIOME_FOREST, BIOME_VOLCANO, BIOME_MOUNTAIN, BIOME_SNOW, BIOME_GRAVEYARD,
  BIOME_COUNT
};

// Debug override set by the BIOME console command; -1 = follow the species.
// Not saved, so a reboot clears it.
static int8_t gBiomeOverride = -1;

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("species #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("BIOME ")) {
    int n = line.substring(6).toInt();
    if (n >= 0 && n < BIOME_COUNT) {
      gBiomeOverride = (int8_t)n;
      Serial.printf("biome %d\n", n);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    // level() is 1 + age/rate, so the age for level N is (N-1) rates -- this
    // used to set N and hand back N+1, which is a poor thing for a command
    // named LVL to do when it is what every balance check is anchored on.
    long want = line.substring(4).toInt();
    if (want < 1) want = 1;
    if (want > MAX_LEVEL) want = MAX_LEVEL;
    pet.ageMinutes = (uint32_t)(want - 1) * MINUTES_PER_LEVEL;
    pet.saveNow();
    Serial.printf("lvl=%u\n", pet.level());
  } else if (line.startsWith("STEPS ")) {
    // STEPS <n>: sets today's step count, to see the plate without a walk
    long want = line.substring(6).toInt();
    if (want < 0) want = 0;
    if (want > STEP_MAX) want = STEP_MAX;
    pet.setStepsToday((uint32_t)want);
    pet.saveNow();
    Serial.printf("steps=%lu\n", (unsigned long)pet.stepsToday());
  } else if (line.startsWith("ENE ")) {
    // ENE <0-100>: sets energy, e.g. to clear the >= 40 evolution gate
    long want = line.substring(4).toInt();
    if (want < 0) want = 0;
    if (want > 100) want = 100;
    pet.energy = (uint8_t)want;
    pet.saveNow();
    Serial.printf("ene=%u\n", pet.energy);
  } else if (line == "POOP" || line.startsWith("POOP ")) {
    // POOP [n]: leaves n piles on the screen (default 1, max 3), for checking
    // how they draw. Same family as MISS / IV / TR.
    long n = line.length() > 5 ? line.substring(5).toInt() : 1;
    if (n < 0) n = 0;
    if (n > 3) n = 3;
    pet.poops = (uint8_t)n;
    pet.saveNow();
    Serial.printf("poops=%u\n", pet.poops);
  } else if (line.startsWith("MISS ")) {
    // MISS <n>: sets the care mistakes -- the miss= on STATS.
    // Each one pushes every evolution threshold up a level, so a creature that
    // was neglected early evolves late; MISS 0 forgives that. Its sibling
    // commands are IV, TR and LVL.
    long m = line.substring(5).toInt();
    if (m < 0) m = 0;
    if (m > 255) m = 255;
    pet.careMistakes = (uint8_t)m;
    pet.saveNow();
    Serial.printf("miss=%u\n", pet.careMistakes);
  } else if (line.startsWith("TR ")) {
    // TR <atk> <def> <spe>: sets the TRAINING (this game's EVs), for testing a
    // fully-raised creature without playing the minigames for an hour. Each is
    // clamped to trMaxFor(iv), the same IV-bound ceiling the games enforce, so
    // this cannot produce a creature the player could not have raised.
    int v[3] = { 0, 0, 0 };
    int n = sscanf(line.c_str() + 3, "%d %d %d", &v[0], &v[1], &v[2]);
    if (n >= 1) {
      int a = v[0], d = (n >= 2) ? v[1] : v[0], e = (n >= 3) ? v[2] : v[0];
      pet.trAtk = (uint8_t)(a < 0 ? 0 : (a > pet.trMaxAtk() ? pet.trMaxAtk() : a));
      pet.trDef = (uint8_t)(d < 0 ? 0 : (d > pet.trMaxDef() ? pet.trMaxDef() : d));
      pet.trSpe = (uint8_t)(e < 0 ? 0 : (e > pet.trMaxSpe() ? pet.trMaxSpe() : e));
      pet.saveNow();
    }
    Serial.printf("tr=%u/%u/%u max=%u/%u/%u\n", pet.trAtk, pet.trDef, pet.trSpe,
                  pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
  } else if (line.startsWith("IV ")) {
    // IV <atk> <def> <spe> <hp>: sets the individual values (testing).
    // "IV 31 31 31 31" shows the ceiling; "IV 8 8 8 8" the floor.
    int v[4] = { 16, 16, 16, 16 };
    int n = sscanf(line.c_str() + 3, "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]);
    if (n >= 1) {
      for (int i = 0; i < 4; i++) v[i] = v[i] < 0 ? 0 : (v[i] > 31 ? 31 : v[i]);
      pet.ivAtk = v[0];
      pet.ivDef = (n >= 2) ? v[1] : v[0];
      pet.ivSpe = (n >= 3) ? v[2] : v[0];
      pet.ivHp = (n >= 4) ? v[3] : v[0];
      if (pet.trAtk > pet.trMaxAtk()) pet.trAtk = pet.trMaxAtk();
      if (pet.trDef > pet.trMaxDef()) pet.trDef = pet.trMaxDef();
      if (pet.trSpe > pet.trMaxSpe()) pet.trSpe = pet.trMaxSpe();
    }
    pet.saveNow();   // IV used to change RAM only and never write
    Serial.printf("iv=%u/%u/%u/%u max=%u/%u/%u\n", pet.ivAtk, pet.ivDef,
                  pet.ivSpe, pet.ivHp, pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // RTC only (simulate power-offs in tests)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simulates 20 egg rolls (does not change the game state)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line.startsWith("EGG ")) {
    // EGG <dex> [shiny]: hatch a chosen species right now. The legendary and
    // shiny IV guarantees only apply at hatch time, so this is the only way to
    // exercise them on hardware (SHINY below just toggles the flag afterwards).
    int dex = 0, sh = 0;
    int n = sscanf(line.c_str() + 4, "%d %d", &dex, &sh);
    if (n >= 1 && dex >= 1 && dex <= DEX_COUNT) {
      pet.dbgHatchAs(dex, sh != 0);
      Serial.printf("%s%s iv=%u/%u/%u/%u\n", DEX_TBL[dex].name,
                    pet.shiny ? " *SHINY*" : "", pet.ivAtk, pet.ivDef,
                    pet.ivSpe, pet.ivHp);
    }
    Serial.println("DONE");
  } else if (line.startsWith("BATTLE")) {
    // BATTLE <dex> [level] -- the only way in until the trainer roster exists
    int dex = 0, lvl = 0;
    int n = sscanf(line.c_str() + 6, "%d %d", &dex, &lvl);
    if (n >= 1 && dex >= 1 && dex <= DEX_COUNT) {
      startBattle(dex, lvl > 0 ? (uint8_t)lvl : pet.level());
      Serial.printf("battle vs %s Lv.%u\n", DEX_TBL[dex].name, btlFoe.level);
    } else {
      Serial.println("usage: BATTLE <dex> [level]");
    }
    Serial.println("DONE");
  } else if (line == "SHINY") {  // toggles shiny on the current one (testing)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simulates a new cared-for day (testing)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // audio test
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // forces the "ready to run away" state (button test)
    Serial.println("DONE");
  } else if (line == "EXPORT") {
    // Prints the whole save as a block of IMPORT commands. Pasting that block
    // back is the restore -- there is no separate format to get wrong, and no
    // single 2000-character line for a terminal to mangle.
    static uint8_t buf[2048];
    size_t n = saveExport(buf, sizeof(buf));
    if (!n) { Serial.println("EXPORT FAIL"); return; }
    Serial.printf("# TamaPoke save, %u bytes. Paste this whole block back.\n",
                  (unsigned)n);
    for (size_t i = 0; i < n; i += 48) {
      Serial.print("IMPORT ");
      for (size_t j = i; j < i + 48 && j < n; j++) Serial.printf("%02X", buf[j]);
      Serial.println();
    }
    Serial.println("IMPORT");        // the empty one commits
  } else if (line.startsWith("IMPORT")) {
    // IMPORT <hex>   append a chunk
    // IMPORT         commit what has been appended
    static uint8_t in[2048];
    static size_t inN = 0;
    String hex = line.substring(6);
    hex.trim();
    if (hex.length()) {
      if (hex.length() & 1) { Serial.println("IMPORT ODD"); inN = 0; return; }
      for (size_t i = 0; i + 1 < (size_t)hex.length(); i += 2) {
        if (inN >= sizeof(in)) { Serial.println("IMPORT FULL"); inN = 0; return; }
        auto nyb = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          return -1;
        };
        const char *hs = hex.c_str();
        int hi = nyb(hs[i]), lo = nyb(hs[i + 1]);
        if (hi < 0 || lo < 0) { Serial.println("IMPORT BAD"); inN = 0; return; }
        in[inN++] = (uint8_t)((hi << 4) | lo);
      }
      return;                        // silent while collecting
    }
    if (!inN) { Serial.println("IMPORT EMPTY"); return; }
    bool ok = saveImport(in, inN);
    Serial.println(ok ? "IMPORT OK" : "IMPORT REJECTED");
    inN = 0;
    if (ok) { Serial.println("DONE"); delay(100); ESP.restart(); }
  } else if (line == "NAME" || line.startsWith("NAME ")) {
    // NAME <owner>   owner shown above TamaPoke on ABOUT; bare NAME clears it
    String arg = line.substring(4);
    arg.trim();
    pet.setOwnerName(arg.c_str());
    Serial.print("OWNER ");
    Serial.println(pet.ownerName);
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // wipes NVS and reboots -> new game (starter choice)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line.startsWith("PARTY")) {
    // PARTY          list the party
    // PARTY <dex>    bank a level-50 specimen (fills slots up for testing)
    // PARTY CLEAR    empty it
    String arg = line.substring(5);
    arg.trim();
    if (arg == "CLEAR") {
      for (int i = 0; i < PARTY_SLOTS; i++) party.releaseAt(i);
    } else if (arg.length()) {
      int d = arg.toInt();
      if (d >= 1 && d <= DEX_COUNT) {
        PartyMon m;
        m.dex = d;
        m.level = 50;
        m.ivAtk = m.ivDef = m.ivSpe = m.ivHp = 20;
        m.trAtk = m.trDef = m.trSpe = 50;
        Serial.println(party.add(m) ? "added" : "party full");
      }
    }
    Serial.printf("party %u/%u:", party.count(), PARTY_SLOTS);
    for (int i = 0; i < PARTY_SLOTS; i++) {
      const PartyMon &m = party.slots[i];
      if (m.empty()) Serial.print(" -");
      else Serial.printf(" %s%s(lv%u)", DEX_TBL[m.dex].name, m.shiny ? "*" : "", m.level);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "REG") {
    Serial.printf("pokedex %u/%u:", pet.registeredCount(), DEX_COUNT);
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    // PSRAM is where the sprites and the framebuffer live, so a memory problem
    // shows up here long before it shows up in the heap figure above.
    Serial.printf("psram=%u screen=%s btlspr=%d/%d\n", (unsigned)ESP.getFreePsram(),
                  SCREEN_NAME[uiCurrentScreen() % SCR_COUNT],
                  btlPmd[0].loaded ? 1 : 0, btlPmd[1].loaded ? 1 : 0);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d lv=%u food=%u joy=%u ene=%u hyg=%u miss=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("weight=%u atk=%u def=%u spe=%u vit=%u berry=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.vitStat(), pet.berryKnown);
    Serial.printf("iv=%u/%u/%u/%u tr=%u/%u/%u max=%u/%u/%u\n",
                  pet.ivAtk, pet.ivDef, pet.ivSpe, pet.ivHp,
                  pet.trAtk, pet.trDef, pet.trSpe,
                  pet.trMaxAtk(), pet.trMaxDef(), pet.trMaxSpe());
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.println("DONE");
  }
}

// ---------- touch input ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

// the touch resolves on finger LIFT to tell a tap from a swipe
void handleTouch() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 20) return;  // 50 Hz is plenty for a finger
  lastPoll = millis();
  // we only touch the bus if the chip signalled via INT or if the finger is still down (we
  // have to detect the lift). Reading the sleeping CST9217 hung for ~1s and
  // froze the whole loop; SensorLib does not honour the Wire timeout.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;

  // punching bag: every touch counts instantly (rapid mashing)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      if (y < 72) leaveSack();       // tap the top = quit, keeping what was earned
      else sackTap();
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // gesture starts
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0) || screenOff;  // si estaba a oscuras, solo despierta
    sliderDrag = !swallowGesture && clockSliderHit(x, y);
    if (sliderDrag) { swallowGesture = true; clockSliderSet(x); }
    if (screenOff) pet.setScreenOff(false);        // waking the screen wakes it
    screenOff = false;
    lastInteract = millis();
  } else if (pressed) {  // still pressed
    tXl = x;
    tYl = y;
    if (sliderDrag) clockSliderSet(x);
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    //
    // Gated on the MAIN screen, not on a hand-maintained list of screens to
    // exclude. That list had gallery/card/keyboard/clock on it and nothing
    // else, so the hold still fired on the party, box, gym, battle, player and
    // menu screens -- every one of which draws something inside inPetZone.
    // On the party screen it was genuinely dangerous: the grid overlaps the
    // zone, so holding a party slot opened "release the live pet?", and that
    // dialog's YES box sits on top of party slot 4. Hold a slot, tap where you
    // think a creature is, lose the creature you are actually raising.
    // One question with one answer, so a new screen cannot be forgotten.
    if (!holdFired && !swallowGesture && uiCurrentScreen() == SCR_MAIN && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // finger lifts: resolve the gesture
    lastInteract = millis();
    if (sliderDrag) {                    // save once, not on every pixel of the drag
      sliderDrag = false;
      clockSliderEnd();
      sfxPlay(SFX_TAP);                  // hear the level you landed on
    }
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      if (abs(dx) > 80 && abs(dy) < 70 && dt < 800) onSwipe(dx > 0 ? 1 : -1);
      else if (abs(dy) > 80 && abs(dx) < 70 && dt < 800) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1500 && abs(dx) < 40 && abs(dy) < 40) onTap(tX0, tY0);
    }
  }
  wasPressed = pressed;
}

// vertical swipe: opens/closes the creature card

void openClock();  // prototype

void onSwipeV(int dir) {
  if (pet.awaitingStarter()) return;  // blocked during the starter choice
  if (uiCurrentScreen() == SCR_DEXPICK || uiCurrentScreen() == SCR_GYMPICK)
    return;                 // on the chooser, vertical does nothing: pick a row
  if (menuOpen) { menuOpen = false; return; }   // any swipe closes the menu
  if (battleOpen) return;   // no swiping out of a fight
  if (pickOpen) { pickOpen = false; return; }
  if (lanOpen) { lanLeave(); lanOpen = false; return; }
  if (gymOpen) {
    // Same gesture as the Pokedex: vertical changes region, horizontal pages.
    gymRegion = (uint8_t)((gymRegion + (dir > 0 ? 1 : GYM_REGIONS - 1)) % GYM_REGIONS);
    gymPage = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  if (playerOpen) { playerOpen = false; return; }
  if (trainOpen) { trainOpen = false; return; }
  if (movePickOpen) { movePickOpen = false; return; }
  if (boxOpen) { boxOpen = false; boxSel = 0; return; }   // vertical backs out
  if (partyOpen) {
    if (partyDetail) { partyDetail = 0; return; }
    if (partyPick) { partyPick = false; pet.endedKind = CER_NONE; }
    partyOpen = false;
    return;
  }
  // Either minigame exits on a swipe. A swipe cannot be confused with a ball
  // hit -- the gesture resolver separates them -- which the header tap no
  // longer can now that the ball is hittable up there.
  if (gameOpen) { leaveGame(); return; }
  if (sackOpen) { leaveSack(); return; }
  if (spdOpen) { leaveSpeed(); return; }
  if (galleryOpen) {
    if (galleryDetail) { galleryDetail = 0; galleryPmd.unload(); galleryDirty = true; return; }
    galleryRegion = (uint8_t)((galleryRegion + (dir > 0 ? 1 : GAL_REGIONS - 1)) % GAL_REGIONS);
    galleryPage = 0;
    galleryDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (kbOpen || pet.ceremony) return;
  if (clockOpen) { clockOpen = false; return; }
  if (cardOpen) {
    if (dir < 0) cardOpen = false;  // up closes the card
    return;
  }
  // Swipe down is the PLAYER card, up is the creature's. The clock lost this
  // gesture on purpose -- the menu's SETTINGS row already opens it, and the
  // player card is the thing you reach for far more often.
  if (dir > 0) {
    if (!confirmUntil && !feedMenuUntil) playerOpen = true;
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // swipe up: card
    cardPage = 0;
  }
}

// party screen: pick a slot (when a newcomer is waiting) or just leave
// A banked creature's sheet: its moves above all, since typing alone does not
// tell you whether that Lapras still has ICE BEAM -- and in hard mode that is
// what decides the fight.
// The detail sheet, shared by the party and the box so the two cannot drift.
// `fromBox` picks which action the LEFT button offers; the right one is always
// RELEASE, which is irreversible and therefore always asks first.
void renderMonSheet(const PartyMon &m, bool fromBox) {
  const DexEntry &d = DEX_TBL[m.dex];
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[36];
  snprintf(head, sizeof(head), "%s%s Lv.%u", m.shiny ? "*" : "",
           m.nick[0] ? m.nick : d.name, (unsigned)m.level);
  uiText(UIF_SMALL, CX, 54, head, d.accent, 1);
  char ty[24];
  if (d.type2 == T_NONE) snprintf(ty, sizeof(ty), "%s", typeName(d.type1));
  else snprintf(ty, sizeof(ty), "%s/%s", typeName(d.type1), typeName(d.type2));
  uiText(UIF_TINY, CX, 71, ty, UI_TRACK_TEXT, 1);

  for (int i = 0; i < MOVE_SLOTS; i++)
    drawMoveRow(78 + i * 52, m.moves[i], false, m.dex);

  char st[40];
  snprintf(st, sizeof(st), "ATK %u  DEF %u  SPD %u  HP %u",
           party.atkOf(m), party.defOf(m), party.speOf(m), party.vitOf(m));
  uiTextFit(UIF_SMALL, CX, 308, st, UI_INK, 1, 380);

  // Bringing one back is only offered while an egg is waiting. Otherwise it
  // would silently destroy whatever creature is currently alive, and a rule the
  // player cannot see is worse than a button they cannot press. A box creature
  // goes to the party instead, which is always allowed if there is room.
  bool leftOk = fromBox ? (party.firstFree() >= 0)
                        : (pet.isEgg() && !pet.awaitingStarter());
  const char *leftLbl = fromBox ? T(S_BOX_TAKE) : T(S_REVIVE);
  gfx->fillRoundRect(PDET_L_X, PDET_BTN_Y, PDET_L_W, PDET_BTN_H, 10,
                     leftOk ? UI_BAR_OK : UI_TRACK_TEXT);
  gfx->drawRoundRect(PDET_L_X, PDET_BTN_Y, PDET_L_W, PDET_BTN_H, 10, UI_INK);
  uiTextFit(UIF_SMALL, PDET_L_X + PDET_L_W / 2, PDET_BTN_Y + PDET_BTN_H / 2 + 7,
            leftLbl, leftOk ? UI_BG_DAY : 0x8410, 1, PDET_L_W - 8);

  gfx->fillRoundRect(PDET_R_X, PDET_BTN_Y, PDET_R_W, PDET_BTN_H, 10, UI_BAR_BAD);
  gfx->drawRoundRect(PDET_R_X, PDET_BTN_Y, PDET_R_W, PDET_BTN_H, 10, UI_INK);
  uiTextFit(UIF_SMALL, PDET_R_X + PDET_R_W / 2, PDET_BTN_Y + PDET_BTN_H / 2 + 7,
            T(S_RELEASE_BTN), UI_WHITE, 1, PDET_R_W - 8);

  // WHY the button is dead, ABOVE it and in a colour that can be read.
  //
  // This used to sit BELOW the buttons in UI_TRACK_TEXT -- pale beige on a pale
  // background, at the smallest text size, eight pixels above "tap: back". It
  // was reported as "every time I hit bring back it just buzzes": the refusal
  // was correct (reviving would destroy the creature you are raising) but the
  // reason was invisible, so the button read as broken rather than disabled.
  // Above the button there is clear space between it and the stat line, and
  // that is also where the eye is already travelling.
  if (!leftOk) {
    const char *why = fromBox ? T(S_PARTY_FULL) : T(S_REVIVE_EGG);
    uiTextFit(UIF_TINY, CX, PDET_BTN_Y - 8, why, UI_BAR_WARN, 1, 340);
  }
  uiText(UIF_SMALL, CX, 418, T(S_BACK), UI_TRACK_TEXT, 1);

  // Asked before it happens, because nothing gets this creature back: it is not
  // a farewell, it does not join anything, and there is no undo.
  if (releaseConfirm) {
    char q[40];
    snprintf(q, sizeof(q), T(S_RELEASE_FMT), m.nick[0] ? m.nick : d.name);
    drawConfirmPanel(q, T(S_RELEASE_GONE), nullptr, UI_BAR_BAD,
                     T(S_YES), UI_BAR_BAD, UI_WHITE, T(S_NO), UI_TRACK_TEXT, UI_INK);
  }
  gfx->flush();
}

void renderPartyDetail() {
  // An empty slot means the creature was just let go. Fall through to the grid
  // rather than returning: a bare return draws nothing AND never flushes, which
  // leaves the previous frame frozen on the panel -- the exact failure
  // flush_test exists to catch, and invisible to any screenshot.
  if (party.slots[partyDetail - 1].empty()) {
    partyDetail = 0;
    releaseConfirm = false;
    renderParty();
    return;
  }
  renderMonSheet(party.slots[partyDetail - 1], false);
}

void renderBoxDetail() {
  if (party.box[boxDetail - 1].empty()) {
    boxDetail = 0;
    releaseConfirm = false;
    renderBox();
    return;
  }
  renderMonSheet(party.box[boxDetail - 1], true);
}

// The two buttons' hit areas, so a test can prove they do not overlap without
// copying the geometry -- a test that restates the numbers drifts from them.
// Every primary button's height, so a test can hold them all to UI_TAP_MIN
// instead of waiting for somebody to report the next one by hand.
void uiButtonHeights(int *out, int max, int *n) {
  const int h[] = { BOXBTN_H, PARTYCLOSE_H, LANBTN_H, BTL_CELL_H + BTL_HIT_PAD * 2 };
  int c = (int)(sizeof(h) / sizeof(h[0]));
  if (c > max) c = max;
  for (int i = 0; i < c; i++) out[i] = h[i];
  if (n) *n = c;
}

// The gym list's difficulty pill against its first leader row. Enlarging the
// pill to a real tap target once pushed it straight over that row -- the third
// overlap of this kind, after BOX/CLOSE and the battle grid against BACK.
// Which home icon is the only one live while the pet sleeps, and where it is.
// Exposed so a test can prove it is the LIGHT: removing an icon once shifted
// every index and quietly made the BATH button the wake-up button.
// Asleep, the LIGHT is the only live icon -- it is what wakes the pet. Both the
// draw path and the tap path ask THIS, so a greyed button can never still be
// tappable and the greying can never point at the wrong icon.
bool uiButtonDisabled(int i) { return pet.sleeping && i != BTN_LIGHT; }

int uiSleepButton(int *cx, int *cy) {
  if (cx) *cx = buttons[BTN_LIGHT].cx;
  if (cy) *cy = buttons[BTN_LIGHT].cy;
  return BTN_LIGHT;
}

void uiButtonAt(int i, int *cx, int *cy, int *half) {
  if (i < 0 || i >= BTN_COUNT) return;
  if (cx) *cx = buttons[i].cx;
  if (cy) *cy = buttons[i].cy;
  if (half) *half = BTN_HALF;
}

void gymHeaderRects(int *pillTop, int *pillBot, int *rowTop) {
  if (pillTop) *pillTop = GYMDIF_Y;
  if (pillBot) *pillBot = GYMDIF_Y + GYMDIF_H;
  if (rowTop) *rowTop = GYM_ROW_Y(0);
}

void partyButtonRects(int *boxTop, int *boxBot, int *closeTop, int *closeBot) {
  if (boxTop) *boxTop = BOXBTN_Y - BOXBTN_PAD;
  if (boxBot) *boxBot = BOXBTN_Y + BOXBTN_H + BOXBTN_PAD;
  if (closeTop) *closeTop = PARTYCLOSE_Y;
  if (closeBot) *closeBot = PARTYCLOSE_Y + PARTYCLOSE_H;
}

// Which of the detail sheet's two buttons a tap landed on. One answer for the
// party and the box, off the same PDET_* constants the sheet is DRAWN with, so
// the graphic and the hit area cannot drift apart.
bool monSheetBtn(int16_t x, int16_t y, bool left) {
  if (y < PDET_BTN_Y || y > PDET_BTN_Y + PDET_BTN_H) return false;
  int x0 = left ? PDET_L_X : PDET_R_X;
  int w = left ? PDET_L_W : PDET_R_W;
  return x >= x0 && x <= x0 + w;
}

// YES / NO on the release confirm. Returns true when the tap was consumed. The
// caller swallows everything else while it is up: a modal that leaks a miss
// through to what is drawn underneath is precisely how an irreversible action
// gets triggered by a fumbled tap, which is the shape CLAUDE.md section 4 keeps
// warning about.
bool monSheetConfirmTap(int16_t x, int16_t y, bool fromBox) {
  int c1t, c1b, c2t, c2b;
  uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
  if (x < CONFIRM_BTN_X || x > CONFIRM_BTN_X + CONFIRM_BTN_W) return false;
  if (y >= c1t && y <= c1b) {            // YES -- and it does not come back
    if (fromBox) { party.boxReleaseAt(boxDetail - 1); boxDetail = 0; }
    else { party.releaseAt(partyDetail - 1); partyDetail = 0; }
    // A half-finished swap named a slot that may now be empty, so disarm both
    // sides rather than leaving one pointing at a creature that is gone.
    boxSwapFrom = 0;
    boxSel = 0;
    releaseConfirm = false;
    sfxPlay(SFX_BYE);
    return true;
  }
  if (y >= c2t && y <= c2b) {            // NO
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return true;
  }
  return false;
}

void partyTap(int16_t x, int16_t y) {
  if (boxOpen) { boxTap(x, y); return; }
  // The SHEET IS CHECKED FIRST, and that is not cosmetic ordering. It covers the
  // whole screen, and its RELEASE button (x240..370, y336..384) lands inside the
  // BOX button's hit area (x138..328, y312..372) below -- so with BOX tested
  // first, tapping RELEASE opened the box instead. Nothing behind a full-screen
  // sheet may answer a tap.
  if (partyDetail) {
    // The confirm is modal: while it is up nothing else on the sheet responds,
    // or a miss on YES would fall through to the move rows underneath it.
    if (releaseConfirm) { monSheetConfirmTap(x, y, false); return; }
    if (monSheetBtn(x, y, true)) {           // BRING BACK
      if (!pet.isEgg() || pet.awaitingStarter()) { sfxPlay(SFX_DENY); return; }
      pet.reviveFrom(party.slots[partyDetail - 1]);
      party.releaseAt(partyDetail - 1);      // it is alive now, not banked
      partyDetail = 0;
      boxSwapFrom = 0;
      partyOpen = false;
      sfxPlay(SFX_HATCH);
      return;
    }
    if (monSheetBtn(x, y, false)) {          // RELEASE -- ask first, always
      releaseConfirm = true;
      sfxPlay(SFX_TAP);
      return;
    }
    for (int i = 0; i < MOVE_SLOTS; i++) {   // tap a move to change it
      int ry = 78 + i * 52;
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      movePickParty = partyDetail;
      movePickSlot = i;
      movePickPage = 0;
      movePickOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    partyDetail = 0;
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  if (!partyPick && y >= BOXBTN_Y - BOXBTN_PAD && y <= BOXBTN_Y + BOXBTN_H + BOXBTN_PAD &&
      x >= BOXBTN_X - BOXBTN_PAD && x <= BOXBTN_X + BOXBTN_W + BOXBTN_PAD) {
    boxOpen = true;                  // open the box, nothing picked yet
    boxPage = 0;
    boxSwapFrom = 0;
    sfxPlay(SFX_TAP);
    return;
  }
  // exit button, and the top band, both always work
  if ((y >= 372 && y <= 416 && x >= 133 && x <= 333) || y < 34) {
    if (partyPick) {                 // declined the swap: the pet is let go
      partyPick = false;
      pet.endedKind = CER_NONE;
    }
    partyOpen = false;
    sfxPlay(SFX_TAP);
    return;
  }
  for (int i = 0; i < PARTY_SLOTS; i++) {
    int cx0 = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int cy0 = PARTY_GRID_Y + (i / 2) * (PARTY_CELL_H + 8);
    if (x < cx0 || x > cx0 + PARTY_CELL_W || y < cy0 || y > cy0 + PARTY_CELL_H) continue;
    if (boxSel) {                    // a box creature is waiting for a slot
      party.swapPartyBox(i, boxSel - 1);
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (!partyPick) {
      if (boxSwapFrom == i + 1) {    // tapped again: take it to the box
        boxOpen = true;
        boxPage = 0;
        sfxPlay(SFX_TAP);
        return;
      }
      if (party.slots[i].empty()) { boxSwapFrom = i + 1; sfxPlay(SFX_TAP); return; }
      partyDetail = i + 1;
      boxSwapFrom = i + 1;           // armed, in case the box is opened next
      sfxPlay(SFX_TAP);
      return;
    }
    party.replaceAt(i, pet.endedMon);
    snprintf(partyBannerName, sizeof(partyBannerName), "%s",
             pet.endedMon.nick[0] ? pet.endedMon.nick : DEX_TBL[pet.endedMon.dex].name);
    partyBannerUntil = millis() + 3500;
    pet.endedKind = CER_NONE;
    partyPick = false;
    partyOpen = false;
    sfxPlay(SFX_MEDAL);
    return;
  }
}

// swipe: dir +1 = to the right
void onSwipe(int dir) {
  // The region chooser pages, and it is checked before everything else because
  // it sits on TOP of the starter/gallery/gym screens -- each of which has its
  // own horizontal handler that would otherwise swallow the gesture. Paging a
  // screen by closing it is the bug this project shipped four times.
  if (rpickSwipe(dir)) return;
  if (pet.awaitingStarter()) return;  // blocked during the starter choice
  if (menuOpen) { menuOpen = false; return; }   // any swipe closes the menu
  if (battleOpen) return;   // no swiping out of a fight
  if (pickOpen) {   // horizontal pages the candidates, as everywhere else
    uint8_t pages = (pickCandidates() + PICK_PER_PAGE - 1) / PICK_PER_PAGE;
    if (!pages) pages = 1;
    int p = (int)pickPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) pickOpen = false;
    else pickPage = (uint8_t)p;
    return;
  }
  if (gymOpen) {   // horizontal pages the ladder; vertical backs out
    uint8_t pages = (TRAINER_COUNT + GYM_ROWS - 1) / GYM_ROWS;
    int p = (int)gymPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) { gymPick = true; rpickPage = 0; }  // back to the chooser
    else gymPage = (uint8_t)p;
    return;
  }
  if (playerOpen) {   // horizontal pages it, like the card and the gallery
    // circular, like the creature's card: the screen only closes by tapping
    int p = (int)playerPage + (dir > 0 ? -1 : 1);
    playerPage = (uint8_t)((p + PLAYER_PAGES) % PLAYER_PAGES);
    return;
  }
  if (trainOpen) { trainOpen = false; return; }
  if (movePickOpen) {   // the picker is paged; without this its later pages
    uint8_t all[64];    // were simply unreachable
    uint8_t n = learnableList(all, sizeof(all));
    uint8_t pages = n ? (n + MOVE_PICK_PER_PAGE - 1) / MOVE_PICK_PER_PAGE : 1;
    int p = (int)movePickPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) movePickOpen = false;
    else movePickPage = (uint8_t)p;
    return;
  }
  if (boxOpen) {   // horizontal pages the box, as every other paged screen
    uint8_t pages = BOX_SLOTS / BOX_PER_PAGE;
    int p = (int)boxPage + (dir > 0 ? -1 : 1);
    if (p < 0 || p >= pages) { boxOpen = false; boxSel = 0; }
    else boxPage = (uint8_t)p;
    return;
  }
  if (partyOpen) {
    if (partyDetail) { partyDetail = 0; return; }
    if (partyPick) { partyPick = false; pet.endedKind = CER_NONE; }
    partyOpen = false;
    return;
  }
  if (gameOpen) { leaveGame(); return; }   // swipe out, keeping what you earned
  if (spdOpen) { leaveSpeed(); return; }
  if (kbOpen) return;
  if (clockOpen) {   // settings: horizontal pages, circular; OK/cancel close it
    int p = (int)settingsPage + (dir > 0 ? -1 : 1);
    settingsPage = (uint8_t)((p + SET_PAGES) % SET_PAGES);
    return;
  }
  if (cardOpen) {  // dentro de la ficha: cambiar entre las 4 paginas
    // circular: a left swipe past the last page lands on the first, a right
    // swipe before the first lands on the last
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // left advances
    cardPage = (uint8_t)((p + CARD_PAGES) % CARD_PAGES);
    return;
  }
  if (!galleryOpen) {
    // Swipe LEFT is the gym ladder, RIGHT is the party. The Pokedex lost this
    // gesture: it has a menu row, and gestures are worth more spent on screens
    // without one.
    if (!pet.ceremony && !confirmUntil) {
      if (dir < 0) { gymOpen = true; gymPick = true; gymPage = 0; rpickPage = 0; }
      else partyOpen = true;
    }
    return;
  }
  if (galleryDetail) {  // in detail view: back to the grid
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  int np = galleryPage - dir;  // swiping left advances a page
  if (np < 0) {                // back past the first page = the region chooser
    galleryPick = true;
    rpickPage = 0;
    galleryPmd.unload();
    return;
  }
  if (np > GAL_PAGES - 1) np = GAL_PAGES - 1;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
  }
}

void onTap(int16_t x, int16_t y) {
  if (pet.awaitingStarter()) {  // first game: region, then starter
    if (!starterRegionDone) {
      int r = regionPickTap(x, y, RPICK_FOR_START);
      if (r >= 0) {
        pet.setRegion((uint8_t)r);   // the region picked here is where eggs come from too
        starterRegionDone = true;
        sfxPlay(SFX_TAP);
      }
      return;
    }
    for (int i = 0; i < starterCountShown(pet.region); i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(starterOf(pet.region, (uint8_t)i));
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  if (playerOpen) {
    if (playerPage == 0 && y >= 32 && y < 68) {   // the name: rename yourself
      openKeyboardFor(KB_TRAINER);
      sfxPlay(SFX_TAP);
      return;
    }
    // the avatar is the only other live target; everything else backs out
    if (playerPage == 0 && x > CX - 40 && x < CX + 40 && y > 70 && y < 146) {
      pet.avatar = (uint8_t)((pet.avatar + 1) % AVATAR_COUNT);
      pet.flushSave();
      sfxPlay(SFX_TAP);
      return;
    }
    playerOpen = false;
    return;
  }
  if (pickOpen) {
    pickTap(x, y);
    return;
  }
  if (lanOpen) {
    lanTap(x, y);
    return;
  }
  if (gymOpen && gymPick) {
    int r = regionPickTap(x, y, RPICK_FOR_GYMS);
    if (r >= 0) {
      gymRegion = (uint8_t)r;
      gymPage = 0;
      gymPick = false;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y >= LANBTN_Y && y <= LANBTN_Y + LANBTN_H &&
        x >= LANBTN_X && x <= LANBTN_X + LANBTN_W) {   // LAN battle
      gymOpen = false; gymPick = false;
      lan.state = LINK_OFF;
      lanOpen = true;
      sfxPlay(SFX_TAP);
      return;
    }
    if (y > 380) { gymOpen = false; gymPick = false; }
    return;
  }
  if (gymOpen) {
    if (y >= GYMDIF_Y && y <= GYMDIF_Y + GYMDIF_H) {   // the difficulty pill
      gymHard = !gymHard;
      sfxPlay(SFX_TAP);
      return;
    }
    for (int i = 0; i < GYM_ROWS; i++) {
      uint8_t idx = gymPage * GYM_ROWS + i;
      if (idx >= TRAINER_COUNT) break;
      int ry = GYM_ROW_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 44) continue;
      if (!gymUnlocked(idx, gymHard)) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      gymOpen = false;
      pickTrainer = idx;
      pickHard = gymHard;
      pickPage = 0;
      pickDefault(squadCap(idx, gymHard));
      pickOpen = true;
      return;
    }
    gymOpen = false;
    return;
  }
  if (pet.hasLearnOffer()) {
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int ry = LEARN_ROW_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      sfxPlay(SFX_TAP);
      pet.acceptLearn(i);
      return;
    }
    if (x >= 70 && x <= 396 && y >= LEARN_SKIP_Y && y <= LEARN_SKIP_Y + 44) {
      sfxPlay(SFX_TAP);
      pet.declineLearn();
    }
    return;   // modal: nothing else on screen responds until it is answered
  }
  if (trainOpen) {
    bool inPanel = (x >= TRAIN_X && x <= TRAIN_X + TRAIN_W &&
                    y >= TRAIN_Y && y <= TRAIN_Y + TRAIN_H);
    if (!inPanel) { trainOpen = false; return; }   // tap outside = back to the pet
    for (int i = 0; i < 3; i++) {   // all three train something now
      int ry = TRAIN_ROW_Y(i);
      if (x < TRAIN_X + 18 || x > TRAIN_X + TRAIN_W - 18) continue;
      if (y < ry || y > ry + TRAIN_ROW_H) continue;
      sfxPlay(SFX_TAP);
      trainOpen = false;
      if (i == 0) startSack();
      else if (i == 1) startSpeedGame();
      else startGame();          // the ball game trains DEF
      return;
    }
    return;
  }
  // The menu is modal and has three independent ways out: the CLOSE row, a tap
  // anywhere on the dimmed area outside the panel, and any swipe (see onSwipe).
  // Deliberately no timeout: a menu that vanishes while you read it is worse
  // than one that lingers.
  if (menuOpen) {
    bool inPanel = (x >= MENU_X && x <= MENU_X + MENU_W &&
                    y >= MENU_Y && y <= MENU_Y + MENU_H);
    if (!inPanel) { menuOpen = false; return; }   // tap outside = back to the pet
    for (int i = 0; i < MENU_ROWS; i++) {
      int ry = MENU_ROW_Y(i);
      if (x < MENU_X + 18 || x > MENU_X + MENU_W - 18) continue;
      if (y < ry || y > ry + MENU_ROW_H) continue;
      sfxPlay(SFX_TAP);
      menuOpen = false;
      if (i == 0) { cardOpen = true; cardPage = 1; }   // straight to the stats page
      else if (i == 1) { galleryOpen = true; galleryPick = true; galleryPage = 0; rpickPage = 0; galleryDetail = 0; galleryDirty = true; }
      else if (i == 2) { openClock(); }
      else if (i == 3) {
        if (!pet.canRetireNow()) { sfxPlay(SFX_DENY); return; }
        choiceKind = 3; choiceUntil = millis() + 12000;
      }
      return;                                     // i == 4 is CLOSE: just shut
    }
    return;
  }
  if (movePickOpen) {
    uint8_t all[64];
    uint8_t n = learnableList(all, sizeof(all));
    for (uint8_t i = 0; i < MOVE_PICK_PER_PAGE; i++) {
      uint8_t idx = movePickPage * MOVE_PICK_PER_PAGE + i;
      if (idx >= n) break;
      int ry = MOVE_PICK_Y(i);
      if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
      sfxPlay(SFX_TAP);
      // Swapping for a move already in another slot would silently duplicate
      // it, so trade the two slots instead of overwriting.
      uint8_t *tgt = pickTargetMoves();
      for (int s = 0; s < MOVE_SLOTS; s++)
        if (tgt[s] == all[idx] && s != movePickSlot) tgt[s] = tgt[movePickSlot];
      tgt[movePickSlot] = all[idx];
      if (movePickParty) party.save(); else pet.flushSave();
      movePickOpen = false;
      return;
    }
    movePickOpen = false;   // tap anywhere else = back to the moves page
    return;
  }
  if (partyOpen) {
    partyTap(x, y);
    return;
  }
  if (galleryOpen) {
    if (galleryPick) {
      int r = regionPickTap(x, y, RPICK_FOR_DEX);
      if (r >= 0) {
        galleryRegion = (uint8_t)r;
        galleryPage = 0;
        galleryDetail = 0;
        galleryDirty = true;
        galleryPick = false;
        sfxPlay(SFX_TAP);
      } else if (y > 380) {
        galleryOpen = false;
      }
      return;
    }
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // no buttons during the farewell
  if (cardOpen) {
    if (cardPage == 0 && y < 84) openKeyboard();  // tap the name = rename
    else if (cardPage == 2) {
      for (int i = 0; i < MOVE_SLOTS; i++) {   // tap a slot to change it
        int ry = MOVE_ROW_Y(i);
        if (x < 70 || x > 396 || y < ry || y > ry + 50) continue;
        sfxPlay(SFX_TAP);
        movePickParty = 0;      // the live pet
        movePickSlot = i;
        movePickPage = 0;
        movePickOpen = true;
        return;
      }
      cardOpen = false;            // anywhere else on the page still exits
    } else {
      cardOpen = false;
    }
    return;
  }
  if (spdOpen) {
    spdTap(x, y);
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  if (choiceKind) {          // decision dialog: action button (top) / keep (bottom)
    int c1t, c1b, c2t, c2b;
    uiConfirmRects(&c1t, &c1b, &c2t, &c2b);
    const bool inX = (x >= CONFIRM_BTN_X && x <= CONFIRM_BTN_X + CONFIRM_BTN_W);
    bool b1 = inX && y >= c1t && y <= c1b;   // action
    bool b2 = inX && y >= c2t && y <= c2b;   // keep / stay together
    if (choiceKind == 1) {                 // evolution
      if (b1) {
        int16_t old = pet.speciesId; pet.evolve(); evoPmd.load(old, pet.shiny);
        audioCry(pet.speciesId);
      }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 3) {          // retirement on request
      if (b1) pet.startRetire();
      // b2 is simply "no": nothing to decline, the row is always there
    } else if (choiceKind == 2) {          // farewell
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // "release?" dialog: YES / NO
    if (millis() < confirmUntil && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // food picker
    if (millis() < feedMenuUntil && y >= 288 && y <= 352 && x >= 101 && x <= 365) {
      int item = (x - 101) / 66;
      eatItem = (uint8_t)item;
      if (item == 3) pet.feedCandy();
      else pet.feedBerry(item);
      sfxPlay(SFX_EAT);
    }
    feedMenuUntil = 0;
    return;
  }
  if (pet.isEgg()) {
    // the region pill first, or choosing a region would also crack the egg --
    // and a near miss is swallowed rather than counted, since three taps hatch
    if (eggRegionTap(x, y)) return;
    pet.eggTap();
    sfxPlay(SFX_TAP);
    if (!pet.isEgg()) audioCry(pet.speciesId);   // it just hatched
    return;
  }
  // evolution button: opens the evolve/keep dialog
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    choiceKind = 1; choiceUntil = millis() + 12000;
    return;
  }
  // ending buttons (same box): direct runaway; farewell opens a dialog.
  //
  // The runaway does NOT ask, deliberately. A pet you have to authorise to
  // leave is not really at stake, and neglect having teeth is the whole premise.
  // What was actually wrong is that a night's sleep could reach this state at
  // all -- fixed where it belongs, in the drain, by having the creature put
  // itself to bed (Pet::tick, autoSleep).
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < BTN_COUNT; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      if (uiButtonDisabled(i)) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      if (i == BTN_FOOD) feedMenuUntil = millis() + 6000;
      else if (i == BTN_LIGHT) pet.toggleLight();
      else if (i == BTN_BATH) startBath();
      else trainOpen = true;
      return;
    }
  }
  // tapping the name/status band opens the menu. This band was inert before,
  // and it sits clear of inPetZone (which starts at y 95). It starts at y 12 so
  // the menu icon beside the clock (drawMenuHint) is inside it.
  if (y >= 12 && y < 94) {
    menuOpen = true;
    sfxPlay(SFX_TAP);
    return;
  }
  // tapping the creature = petting
  if (inPetZone(x, y)) {
    pet.caress();
    if (!pet.sleeping) {
      // the creature answers a pet now and then; every tap would be a racket
      static uint32_t lastCryAt = 0;
      if (!lastCryAt || millis() - lastCryAt > PET_CRY_GAP_MS) {
        lastCryAt = millis() ? millis() : 1;
        audioCry(pet.speciesId);
      } else sfxPlay(SFX_HEART);
    }
  }
}

// ---------- render ----------

bool gNight = false;  // real night, by the RTC hour: set by render(). Sleeping does NOT make it night.

// The species accent is picked to read on the dark night sky; on the pale
// morning / afternoon / sunset skies a yellow or light-blue name all but vanishes.
// Darkening it toward black keeps the hue and gives the contrast back.
static uint16_t nameOnSky(uint16_t accent) {
  return lerp565(accent, RGB565_BLACK, 11, 20);
}
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- background scene: type biome + real RTC time ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // line where the sky meets the ground

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// biome of the live creature (eggs are always meadow) unless overridden
static uint8_t sceneBiome() {
  if (gBiomeOverride >= 0) return (uint8_t)gBiomeOverride;
  return pet.isEgg() ? BIOME_MEADOW : DEX_TBL[pet.speciesId].biome;
}

// hour of day 0-23 (from the real time cached every 30s; 13 if there is no clock)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

// ground of each biome by day (at night it blends toward the night blue)
static const uint16_t BIOME_SOIL[BIOME_COUNT] = {
  C565(0x7e, 0xc0, 0x7f),  // BIOME_MEADOW
  C565(0xdc, 0xca, 0x94),  // BIOME_BEACH (sand)
  C565(0x3f, 0x78, 0x4c),  // BIOME_FOREST
  C565(0x5e, 0x40, 0x3c),  // BIOME_VOLCANO (dark basalt)
  C565(0xa8, 0x90, 0x6a),  // BIOME_MOUNTAIN
  C565(0xe6, 0xee, 0xf5),  // BIOME_SNOW
  C565(0x5e, 0x5a, 0x6e),  // BIOME_GRAVEYARD (cold violet-grey earth)
};

// One puffy cloud: a flat-bottomed body of overlapping circles, a soft shadow
// band under it and a highlight on the top puffs.
static void drawCloud(int cx, int cy, int s, uint16_t col, uint16_t shade) {
  // s = scale in percent
  auto px = [&](int v) { return v * s / 100; };
  int r1 = px(17), r2 = px(13), r3 = px(11), r4 = px(9);
  // shadow body, one step lower
  gfx->fillRoundRect(cx - px(30), cy + px(2), px(60), px(14), px(7), shade);
  gfx->fillCircle(cx + px(20), cy + px(5), r3, shade);
  gfx->fillCircle(cx - px(20), cy + px(6), r4, shade);
  // main body
  gfx->fillRoundRect(cx - px(30), cy - px(2), px(60), px(14), px(7), col);
  gfx->fillCircle(cx - px(2), cy - px(6), r1, col);
  gfx->fillCircle(cx + px(18), cy + px(1), r2, col);
  gfx->fillCircle(cx - px(19), cy + px(3), r3, col);
  gfx->fillCircle(cx + px(30), cy + px(6), r4, col);
  gfx->fillCircle(cx - px(30), cy + px(7), r4, col);
}

void drawClouds(uint32_t now, uint16_t col, uint16_t shade) {
  static const int Y[3] = { 62, 108, 170 };
  static const int S[3] = { 120, 80, 100 };
  static const int OFS[3] = { 0, 190, 360 };
  static const int DIV[3] = { 70, 110, 90 };  // ms per px: far clouds drift slower
  for (int k = 0; k < 3; k++) {
    int cx = (int)((now / DIV[k] + OFS[k]) % 600) - 70;
    drawCloud(cx, Y[k], S[k], col, shade);
  }
}

// Sun: a stepped halo blended into the sky, slowly turning rays, a disc and a
// small highlight. 'sky' is the sky colour at the sun's height.
static void drawSun(int cx, int cy, int r, uint16_t core, uint16_t sky, uint32_t now) {
  for (int i = 0; i < 4; i++)
    gfx->fillCircle(cx, cy, r + 30 - i * 8, lerp565(sky, core, i + 1, 9));
  float a0 = (now % 24000) * (2.0f * PI / 24000.0f);
  for (int k = 0; k < 8; k++) {
    float a = a0 + k * (PI / 4.0f);
    float ca = cosf(a), sa = sinf(a), cb = cosf(a + 0.16f), sb = sinf(a + 0.16f);
    int r0 = r + 4, r1 = r + (k & 1 ? 20 : 28);
    gfx->fillTriangle(cx + (int)(ca * r0), cy + (int)(sa * r0),
                      cx + (int)(cb * r0), cy + (int)(sb * r0),
                      cx + (int)(ca * r1 + cb * r1) / 2, cy + (int)(sa * r1 + sb * r1) / 2,
                      lerp565(sky, core, 6, 9));
  }
  gfx->fillCircle(cx, cy, r, core);
  gfx->fillCircle(cx, cy, r - 5, lerp565(core, C565(0xff, 0xff, 0xff), 1, 3));
  gfx->fillCircle(cx - r / 3, cy - r / 3, r / 4, C565(0xff, 0xff, 0xff));
}

// Stars: random spots inside the round panel, rolled on first use. Each one
// twinkles on its own period and phase; the bright ones get a small cross.
static void drawStars(uint32_t now, uint16_t top, uint16_t bot, int yMax) {
  static struct { int16_t x, y; uint16_t period, phase; uint8_t size; } st[STAR_COUNT];
  static bool rolled = false;
  if (!rolled) {
    for (int i = 0; i < STAR_COUNT; i++) {
      int x, y;
      do {  // inside the circle (centre 233,233 radius 215), clear of the bottom row
        x = 20 + random(426); y = 24 + random(yMax - 24);
      } while ((x - 233) * (x - 233) + (y - 233) * (y - 233) > 215 * 215);
      st[i] = { (int16_t)x, (int16_t)y, (uint16_t)(1400 + random(2600)),
                (uint16_t)random(4000), (uint8_t)(random(4) == 0 ? 2 : 1) };
    }
    rolled = true;
  }
  for (int i = 0; i < STAR_COUNT; i++) {
    // triangle wave 0..8 over the period
    int t = (int)((now + st[i].phase) % st[i].period) * 16 / st[i].period;
    int b = t < 8 ? t : 16 - t;
    int sy = st[i].y;
    uint16_t sky = lerp565(top, bot, sy, HORIZON);
    uint16_t c = lerp565(sky, C565(0xff, 0xff, 0xf0), 3 + b, 11);
    int sz = st[i].size * 2;
    gfx->fillRect(st[i].x, sy, sz, sz, c);
    if (st[i].size == 2 && b >= 5) {  // sparkle cross
      uint16_t d = lerp565(sky, c, 1, 2);
      gfx->fillRect(st[i].x - 3, sy + 1, 10, 2, d);
      gfx->fillRect(st[i].x + 1, sy - 3, 2, 10, d);
    }
  }
}

// Moon: soft halo, then a crescent painted row by row (moon disc minus a
// shifted cut-out disc), so the dark side stays the halo and never pokes
// outside the moon's outline.
static void drawMoon(int cx, int cy, uint16_t sky) {
  const int R = 26, CR = 22, cutX = cx + 12;
  for (int i = 0; i < 4; i++)
    gfx->fillCircle(cx, cy, 50 - i * 6, lerp565(sky, C565(0xb8, 0xc4, 0xe0), i + 1, 12));
  uint16_t lit = C565(0xf2, 0xf0, 0xdc);
  for (int dy = -R; dy <= R; dy++) {
    int mw = (int)sqrtf((float)(R * R - dy * dy));
    int x0 = cx - mw, x1 = cx + mw;                  // moon span on this row
    if (abs(dy) >= CR) { gfx->fillRect(x0, cy + dy, x1 - x0 + 1, 1, lit); continue; }
    int cw = (int)sqrtf((float)(CR * CR - dy * dy));
    int c0 = cutX - cw;                                // cut-out span starts here
    if (c0 > x0) gfx->fillRect(x0, cy + dy, min(c0, x1 + 1) - x0, 1, lit);
    if (cutX + cw < x1) gfx->fillRect(cutX + cw + 1, cy + dy, x1 - cutX - cw, 1, lit);
  }
  uint16_t cr = C565(0xd6, 0xd6, 0xc2);              // craters, all inside the lit part
  gfx->fillCircle(cx - 18, cy + 8, 3, cr);
  gfx->fillCircle(cx - 20, cy - 8, 2, cr);
  gfx->fillCircle(cx - 12, cy + 18, 2, cr);
}

// Volcano biome, in the forest's style: layered tones lit from the sun side, and
// hand-built shapes rather than flat triangles. Back to front: a jagged distant
// range with a small second cone, a smoke plume lit from below, the main cone
// (craggy scanline-built edges, four rock tones, eroded gullies, a rim warmed by
// the crater), lava streams, an occasional eruption burst and lava bombs, glowing
// ground cracks, a small SLUGMA crawling far back, faceted boulders and a charred
// dead tree. The cone sits left of centre so its plume stays clear of the sun and
// moon (upper right). Keep the ground's lower left clear: poops land there (see
// drawPoops). The step counter covers the far left, the HUD everything below ~310.

// Cheap deterministic noise 0..255: fixed per (a, seed), so edges never shimmer.
static int volNoise(int a, int seed) {
  uint32_t v = (uint32_t)a * 374761393u + (uint32_t)seed * 668265263u;
  v = (v ^ (v >> 13)) * 1274126177u;
  return (int)((v >> 16) & 0xff);
}

// A boulder: a fan of facets around a noisy outline, tone chosen by the facet's
// facing (lit toward the upper right), with a lava rim-light on the lower left.
static void volBoulder(int cx, int cy, int r, int seed, bool night, uint16_t lava) {
  uint16_t t[4];
  if (night) { t[0] = C565(0x0c, 0x08, 0x0e); t[1] = C565(0x18, 0x12, 0x18); t[2] = C565(0x24, 0x1c, 0x24); t[3] = C565(0x34, 0x28, 0x30); }
  else       { t[0] = C565(0x2a, 0x1e, 0x22); t[1] = C565(0x42, 0x30, 0x34); t[2] = C565(0x5e, 0x46, 0x46); t[3] = C565(0x80, 0x60, 0x58); }
  const int N = 8;
  int vx[N + 1], vy[N + 1];
  for (int i = 0; i < N; i++) {
    float an = i * 6.2832f / N;
    int rr = r * (78 + volNoise(i, seed) % 30) / 100;
    vx[i] = cx + (int)(cosf(an) * rr * 1.25f);                // wider than tall: it sits on the ground
    vy[i] = cy + (int)(sinf(an) * rr * 0.8f);
  }
  vx[N] = vx[0]; vy[N] = vy[0];
  for (int i = 0; i < N; i++) {
    float an = (i + 0.5f) * 6.2832f / N;                       // facet facing; y is down, so up-right is negative sin
    float lit = cosf(an) * 0.6f - sinf(an) * 0.8f;             // dot with the sun direction
    uint16_t c = lit > 0.75f ? t[3] : (lit > 0.2f ? t[2] : (lit > -0.4f ? t[1] : t[0]));
    gfx->fillTriangle(cx, cy - r / 6, vx[i], vy[i], vx[i + 1], vy[i + 1], c);
  }
  uint16_t rim = lerp565(t[0], lava, 1, 2);                    // lava glow along the lower-left edge
  for (int i = 3; i <= 5; i++) gfx->drawLine(vx[i], vy[i], vx[i + 1], vy[i + 1], rim);
}

// Charred dead tree: forked bare limbs, a lit edge, and ember cracks that pulse.
// Thickness steps down limb by limb; the tips sway.
static void volDeadTree(int cx, int footY, uint32_t now, float sway, bool night, uint16_t lava) {
  uint16_t ch  = night ? C565(0x14, 0x0e, 0x10) : C565(0x2e, 0x22, 0x22);
  uint16_t chL = night ? C565(0x24, 0x1a, 0x1c) : C565(0x58, 0x40, 0x3a);
  static const int16_t LB[12][5] = {                          // x0 y0 x1 y1 width
    { 0, 0, -3, -50, 4 }, { -3, -50, 2, -96, 3 },
    { -3, -50, -26, -74, 2 }, { -26, -74, -38, -98, 2 },
    { -3, -56, 22, -72, 2 }, { 22, -72, 34, -100, 2 },
    { 2, -96, -12, -122, 2 }, { 2, -96, 16, -118, 2 },
    { -38, -98, -47, -110, 1 }, { 34, -100, 45, -111, 1 },
    { -12, -122, -18, -136, 1 }, { 16, -118, 25, -132, 1 } };
  for (int i = 0; i < 4; i++)                                // root flare
    gfx->fillTriangle(cx + (i < 2 ? -14 : 14), footY + 2, cx + (i < 2 ? -4 : 4), footY - 14, cx + (i < 2 ? -4 : 4), footY + 2, ch);
  for (int i = 0; i < 12; i++) {
    int x0 = cx + LB[i][0] + (int)(sway * -LB[i][1] / 100.0f), y0 = footY + LB[i][1];
    int x1 = cx + LB[i][2] + (int)(sway * -LB[i][3] / 100.0f), y1 = footY + LB[i][3];
    for (int w = 0; w < LB[i][4]; w++) gfx->drawLine(x0 + w - LB[i][4] / 2, y0, x1 + w - LB[i][4] / 2, y1, ch);
    gfx->drawLine(x0 + LB[i][4] / 2, y0, x1 + LB[i][4] / 2, y1, chL);            // lit right edge
  }
  float pulse = (sinf(now / 600.0f) + 1.0f) * 0.5f;
  uint16_t ember = lerp565(lava, C565(0xff, 0xd0, 0x58), (int)(pulse * 4), 8);
  static const int8_t EM[5][3] = { { -1, -14, 8 }, { 0, -30, 6 }, { -3, -52, 6 }, { 1, -74, 7 }, { -27, -80, 5 } };
  for (int i = 0; i < 5; i++)                                // glowing cracks in the char
    gfx->fillRect(cx + EM[i][0] + (int)(sway * -EM[i][1] / 100.0f), footY + EM[i][1], 1, EM[i][2], ember);
}

// A small SLUGMA creeping along the ground far behind the pet. Loaded once on
// entering the volcano (~140 KB of PSRAM) and freed when the biome changes.
static PmdMon gVolcanoMon;
static bool gVolcanoTried = false;

static void drawVolcano(uint32_t now, bool night, uint16_t top, uint16_t bot) {
  const int VX = 170, VT = HORIZON - 104, B = HORIZON + 2;   // crater centre, cone top, base
  float pulse = (sinf(now / 420.0f) + 1.0f) * 0.5f;          // 0..1
  uint16_t rockD = night ? C565(0x0e, 0x0a, 0x10) : C565(0x34, 0x24, 0x28);
  uint16_t rock  = night ? C565(0x1c, 0x16, 0x1c) : C565(0x56, 0x3e, 0x40);
  uint16_t rockM = night ? C565(0x2a, 0x20, 0x28) : C565(0x74, 0x52, 0x4c);
  uint16_t rockL = night ? C565(0x3a, 0x2e, 0x36) : C565(0x98, 0x6c, 0x60);
  uint16_t lava  = lerp565(C565(0xe8, 0x4a, 0x14), C565(0xff, 0x9a, 0x2a), (int)(pulse * 8), 8);
  uint16_t hot   = C565(0xff, 0xd0, 0x58);
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  auto hw = [&](int y) { return 26 + (y - VT) * 100 / (B - VT); };   // cone half-width at y
  auto seg = [&](int x0, int x1, int y, uint16_t c) { if (x1 > x0) gfx->fillRect(x0, y, x1 - x0, 2, c); };

  // heat glow along the horizon, then a jagged distant range and a small far cone
  gfx->fillRect(0, HORIZON - 12, 466, 14, lerp565(skyAt(HORIZON), lava, 1, 7));
  uint16_t far = lerp565(rock, skyAt(HORIZON - 40), 1, 2);
  for (int x = -10; x < 480; x += 14) {                      // ridge profile interpolated between noisy peaks
    int h1 = 14 + volNoise(x / 14, 11) % 30, h2 = 14 + volNoise(x / 14 + 1, 11) % 30;
    gfx->fillTriangle(x, B, x + 14, B, x, B - h1, far);
    gfx->fillTriangle(x + 14, B, x + 14, B - h2, x, B - h1, far);
  }
  gfx->fillTriangle(330, B, 480, B, 400, HORIZON - 54, far);                              // far second cone
  gfx->fillRect(393, HORIZON - 56, 14, 3, lerp565(far, lava, 1, 3 + (int)(pulse * 1)));  // its faint crater glow

  // smoke: puffs rise, grow and fade; each is a dark body with a lighter disc
  // low on it, so the young ones read as lit orange from the crater below
  uint16_t smoke = night ? C565(0x34, 0x2c, 0x3c) : C565(0x5c, 0x4a, 0x50);
  for (int k = 0; k < 7; k++) {
    int t = (int)((now / 36 + k * 31) % 217);               // 0..216 life of one puff
    int y = VT - 6 - t;
    int x = VX + 6 + t / 3 + (int)(sinf((t + k * 20) / 22.0f) * 8);
    int r = 10 + t / 8;
    uint16_t c = lerp565(smoke, skyAt(y), t, 217);
    uint16_t glowC = t < 90 ? lerp565(c, lava, 90 - t, 260) : lerp565(c, C565(0xff, 0xff, 0xff), 1, night ? 14 : 8);
    gfx->fillCircle(x, y, r, c);
    gfx->fillCircle(x + 1, y + r / 4, r * 65 / 100, glowC);
  }

  // lightning flickering inside the plume every few seconds
  if (now % 5200 < 140) {
    int lx = VX + 30, ly = VT - 120;
    uint16_t bolt = C565(0xff, 0xf4, 0xc8);
    static const int8_t Z[5][2] = { { 0, 0 }, { -9, 14 }, { 6, 28 }, { -8, 44 }, { 2, 62 } };
    for (int i = 0; i < 4; i++) {
      gfx->drawLine(lx + Z[i][0], ly + Z[i][1], lx + Z[i + 1][0], ly + Z[i + 1][1], bolt);
      gfx->drawLine(lx + Z[i][0] + 1, ly + Z[i][1], lx + Z[i + 1][0] + 1, ly + Z[i + 1][1], bolt);
    }
  }

  // main cone, one 2 px scanline at a time: noisy edges, four rock bands whose
  // borders wobble, lava warmth near the rim, and rock/light specks
  for (int y = VT; y <= B; y += 2) {
    int w = hw(y);
    int xl = VX - w + volNoise(y / 4, 1) % 5 - 2, xr = VX + w + volNoise(y / 4, 2) % 5 - 2, W = xr - xl;   // noise per 8 px: craggy, not stair-stepped
    int wob = (int)(sinf(y / 9.0f) * 4.0f);
    int b1 = xl + W * 30 / 100 + wob + volNoise(y, 3) % 7 - 3;
    int b2 = xl + W * 55 / 100 - wob + volNoise(y, 4) % 7 - 3;
    int b3 = xl + W * 76 / 100 + wob + volNoise(y, 5) % 7 - 3;
    int k = y < VT + 30 ? (30 - (y - VT)) * (50 + (int)(pulse * 14)) / 30 : 0;   // lava warmth, 0..~64
    seg(xl, b1, y, k ? lerp565(rockD, lava, k, 160) : rockD);
    seg(b1, b2, y, k ? lerp565(rock, lava, k, 160) : rock);
    seg(b2, b3, y, k ? lerp565(rockM, lava, k, 160) : rockM);
    seg(b3, xr, y, k ? lerp565(rockL, lava, k, 160) : rockL);
    if (volNoise(y, 6) < 70 && W > 4) gfx->fillRect(xl + volNoise(y, 7) % W, y, 3, 2, rockD);
    if (volNoise(y, 8) < 60 && xr > b3 + 3) gfx->fillRect(b3 + volNoise(y, 9) % (xr - b3 - 2), y, 2, 2, lerp565(rockL, C565(0xff, 0xff, 0xff), 1, 6));
  }
  for (int g = 0; g < 6; g++)                                // eroded gullies fanning from the rim
    for (int y = VT + 12; y < B - 4; y += 3) {
      int x = VX + (g * 46 - 115) * (y - VT) / (B - VT) + volNoise(y + g * 31, 12) % 5 - 2;
      gfx->fillRect(x, y, 2, 3, rockD);
    }
  volBoulder(VX - 98, B + 6, 8, 5, night, lava);              // a few boulders at the foot
  volBoulder(VX + 58, B + 8, 9, 6, night, lava);
  volBoulder(VX + 108, B + 4, 6, 7, night, lava);

  // crater: a rocky lip, a dark throat, a lava lake with a hot centre and bubbles
  gfx->fillRoundRect(VX - 34, VT - 9, 68, 17, 8, rockM);
  gfx->fillRoundRect(VX - 34, VT - 9, 68, 5, 3, rockL);                       // lit lip
  gfx->fillRoundRect(VX - 28, VT - 6, 56, 11, 5, rockD);
  gfx->fillRoundRect(VX - 25, VT - 5, 50, 9, 4, lava);
  gfx->fillRoundRect(VX - 13, VT - 3, 26, 4, 2, hot);
  for (int i = 0; i < 3; i++)                                                  // bubbles popping
    if ((now / 260 + i * 2) % 5 < 2) gfx->fillCircle(VX - 16 + i * 16 + (int)(sinf(now / 300.0f + i) * 3), VT - 3, 2, hot);

  // lava streams, wiggling, white-hot at the top, with a bright core and a glow at the foot
  const int SX[3] = { VX - 10, VX + 8, VX + 18 };
  const int SD[3] = { -34, -4, 40 };                         // total drift of each stream
  const int SL[3] = { 96, 70, 100 };                         // length
  for (int i = 0; i < 3; i++) {
    for (int y = 0; y < SL[i]; y += 2) {
      int x = SX[i] + SD[i] * y / SL[i] + (int)(sinf(y / 9.0f + i * 2) * 3);
      gfx->fillRect(x - 3, VT + 4 + y, 7, 3, lerp565(lava, rockD, 1, 5));   // dark crust edge
      gfx->fillRect(x - 2, VT + 4 + y, 5, 3, y < 10 ? hot : lava);
      gfx->fillRect(x - 1, VT + 4 + y, 2, 3, lerp565(lava, hot, 1, 2));     // core
      if (y > 20 && volNoise(y + i * 17, 16) < 50) gfx->fillRect(x - 2, VT + 4 + y, 5, 3, lerp565(lava, rockD, 2, 3));   // cooled crust patches
    }
    int fx = SX[i] + SD[i];
    if (VT + 4 + SL[i] > B - 14) gfx->fillCircle(fx, B - 2, 6 + (int)(pulse * 2), lerp565(rock, lava, 1, 2));
  }

  // every ~9 s the crater erupts for a second: a fountain of sparks and a flare
  uint32_t ep = now % 9000;
  if (ep < 1100) {
    int et = (int)(ep * 100 / 1100);                         // 0..99
    gfx->fillCircle(VX, VT - 2, 16 * (100 - et) / 100 + 4, lerp565(lava, hot, 1, 2));
    for (int i = 0; i < 16; i++) {
      int dx = (volNoise(i, 17) % 121) - 60, vy = 70 + volNoise(i, 18) % 70;
      int x = VX + dx * et / 100, y = VT - vy * et / 100 + 90 * et * et / 10000;
      if (y > 14 && y < B - 4) gfx->fillRect(x, y, 3, 3, i & 1 ? hot : lava);
    }
  }

  // lava bombs: thrown from the crater on arcs, trailing sparks
  static const int8_t BVX[4] = { -92, -40, 56, 104 };         // horizontal reach
  static const uint8_t BH[4] = { 130, 104, 120, 90 };         // arc height
  for (int k = 0; k < 4; k++) {
    for (int tr = 0; tr < 3; tr++) {                         // head, then two fading trail dots
      int ph = (int)((now / 12 + k * 50) % 210) - tr * 5;     // 0..209, tr steps back in time
      if (ph < 0 || ph >= 150) continue;
      int x = VX + BVX[k] * ph / 150;
      int y = VT - 4 * BH[k] * ph * (150 - ph) / (150 * 150);
      uint16_t c = tr == 0 ? hot : (tr == 1 ? lava : lerp565(skyAt(y), lava, 1, 2));
      int sz = tr == 0 ? 5 : (tr == 1 ? 4 : 3);
      if (y > 14 && y < B - 4) gfx->fillRect(x - sz / 2, y - sz / 2, sz, sz, c);
    }
  }

  // cracked ground: thin jagged fissures with dim lava showing through, kept to
  // the edges and well under the stream brightness so the pet stays the focus
  static const int16_t CK[5][8][2] = {                        // x,y points, x < 0 ends the line
    { { 332, 236 }, { 348, 248 }, { 338, 262 }, { 362, 274 }, { 354, 290 }, { 378, 304 }, { -1, 0 } },
    { { 348, 248 }, { 372, 254 }, { 390, 246 }, { -1, 0 } },
    { { 414, 240 }, { 426, 256 }, { 416, 270 }, { 442, 284 }, { -1, 0 } },
    { { 112, 238 }, { 126, 250 }, { 116, 262 }, { 138, 272 }, { -1, 0 } },
    { { 262, 246 }, { 274, 260 }, { 266, 274 }, { 286, 286 }, { -1, 0 } } };
  uint16_t crack = lerp565(lerp565(rockD, lava, 5, 8), lava, (int)(pulse * 3), 8);
  for (int c = 0; c < 5; c++)
    for (int i = 0; CK[c][i + 1][0] >= 0; i++) {
      int x0 = CK[c][i][0], y0 = CK[c][i][1], x1 = CK[c][i + 1][0], y1 = CK[c][i + 1][1];
      gfx->drawLine(x0 - 1, y0, x1 - 1, y1, rockD);           // dark lip either side
      gfx->drawLine(x0 + 1, y0, x1 + 1, y1, rockD);
      gfx->drawLine(x0, y0, x1, y1, crack);
    }

  // a SLUGMA creeping along behind the pet, slowly
  if (!gVolcanoTried) { gVolcanoTried = true; gVolcanoMon.load(218, false); }
  if (gVolcanoMon.loaded) {
    const uint32_t P = 34000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 250 + (int)(100ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gVolcanoMon.has(act)) act = PMD_IDLE;
    drawPmdActZ(gVolcanoMon, act, x, 262, now, true, false, 2, 70, 220);
  }

  // faceted boulders in front, on the right and the middle ground
  volBoulder(412, 296, 17, 1, night, lava);
  volBoulder(380, 302, 10, 2, night, lava);
  volBoulder(306, 292, 11, 3, night, lava);
  volBoulder(452, 300, 12, 4, night, lava);
  // the charred tree last, so it stands in front
  volDeadTree(436, 274, now, sinf(now / 1700.0f) * 2.0f, night, lava);

  // ash drifting down across the sky, embers rising from the crater
  for (int a = 0; a < 14; a++) {
    int x = (a * 67 + (int)(now / 70) + a * a * 5) % 466;
    int y = (a * 41 + (int)(now / 34)) % (HORIZON - 30) + 24;
    gfx->fillRect(x, y, 2, 2, lerp565(skyAt(y), rockD, 1, 2));
  }
  for (int e = 0; e < 8; e++) {
    int life = (int)((now / (14 + e * 2) + e * 53) % 190);
    int x0 = VX - 20 + e * 6;
    int y0 = VT;
    int x = x0 + (int)(sinf((life + e * 17) / 14.0f) * 10);
    int y = y0 - life;
    if (y < 20) continue;
    gfx->fillRect(x, y, life < 120 ? 3 : 2, life < 120 ? 3 : 2, life < 120 ? hot : lava);
  }
}

// Beach biome: a hazy island and a sailboat on a banded sea whose crests drift
// at depth-scaled speeds, a glitter path under the sun or moon, gulls by day, a
// tide that washes foam in and out over wet sand, an EXEGGUTOR pacing the
// shore, a WINGULL overhead and shells.
// The sea horizon is higher than HORIZON so there is room for water above the
// pet; the lower left stays bare (poops land there, see drawPoops).
#define SEA_TOP 176   // sea horizon y

// A small EXEGGUTOR strolls the shore far behind the pet and a WINGULL flies
// over. Each PMD sprite (~140 KB of PSRAM) is loaded once on entering the beach
// and freed when the biome changes.
static PmdMon gBeachMon;
static PmdMon gBeachBird;          // WINGULL, wheeling across the sky by day
static bool gBeachBirdTried = false;
static bool gBeachTried = false;   // one load attempt per visit: no SD must not retry every frame

static void drawBeach(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  bool sunset = !night && h >= 18, sunrise = !night && h < 8;
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };

  uint16_t farC, nearC;
  if (night)  { farC = C565(0x1e, 0x38, 0x58); nearC = C565(0x0c, 0x1c, 0x34); }
  else        { farC = C565(0x86, 0xcc, 0xdc); nearC = C565(0x2a, 0x82, 0xb0); }
  if (sunset || sunrise) {                                  // tint the water by the sky
    farC = lerp565(farC, bot, 1, 3);
    nearC = lerp565(nearC, bot, 1, 5);
  }
  uint16_t crest = night ? lerp565(nearC, C565(0x8a, 0xa8, 0xd0), 1, 3) : lerp565(farC, C565(0xff, 0xff, 0xff), 2, 3);

  // distant island and sailboat sit on the horizon, hazed toward the sky
  uint16_t isl = lerp565(night ? C565(0x10, 0x20, 0x1c) : C565(0x3e, 0x72, 0x56), skyAt(SEA_TOP), 1, 3);
  gfx->fillCircle(408, SEA_TOP + 2, 24, isl);
  gfx->fillCircle(380, SEA_TOP + 4, 15, isl);
  if (!night) {
    int bob = (int)(sinf(now / 700.0f) * 1.5f);
    int bx = 172;
    gfx->fillTriangle(bx - 1, SEA_TOP - 22 + bob, bx - 1, SEA_TOP - 4 + bob, bx + 11, SEA_TOP - 4 + bob, C565(0xff, 0xff, 0xff));
    gfx->fillTriangle(bx - 3, SEA_TOP - 17 + bob, bx - 3, SEA_TOP - 4 + bob, bx - 13, SEA_TOP - 4 + bob, lerp565(C565(0xff, 0xff, 0xff), skyAt(SEA_TOP), 1, 4));
    gfx->fillRect(bx - 12, SEA_TOP - 3 + bob, 26, 3, C565(0x7a, 0x4a, 0x30));
  }

  // sea: depth bands, haze at the horizon
  const int ROWS = 6, RH = 11;
  for (int r = 0; r < ROWS; r++)
    gfx->fillRect(0, SEA_TOP + r * RH, 466, RH, lerp565(farC, nearC, r, ROWS - 1));
  gfx->fillRect(0, SEA_TOP, 466, 3, lerp565(skyAt(SEA_TOP), farC, 1, 2));

  // drifting crests: farther rows are slower and shorter (parallax)
  for (int r = 0; r < ROWS; r++) {
    int y0 = SEA_TOP + r * RH;
    for (int k = 0; k < 7; k++) {
      int x = ((k * 83 + r * 37) + (int)(now / (52 - r * 6))) % 540 - 40;
      int yy = y0 + 5 + (int)(sinf(now / 520.0f + k + r) * 2);
      int w = 10 + r * 5;
      gfx->fillRect(x, yy, w, 2, crest);
      gfx->fillRect(x + w / 3, yy + 2, w / 3, 1, lerp565(lerp565(farC, nearC, r, ROWS - 1), crest, 1, 2));
    }
  }

  // glitter path under the sun (or moon): flickering dashes that widen toward us
  int gx = sunset ? 233 : 360;
  uint16_t gl = night ? C565(0xd8, 0xe0, 0xf0) : C565(0xff, 0xf4, 0xd0);
  for (int r = 0; r < ROWS; r++)
    for (int k = 0; k < 4; k++) {
      if ((now / 140 + k * 2 + r * 3) % 4 == 0) continue;     // flicker
      int dx = (int)(sinf(now / 420.0f + k * 1.9f + r) * (5 + r * 5));
      int w = 4 + r * 2;
      gfx->fillRect(gx + dx - w / 2, SEA_TOP + r * RH + 2 + k * 2, w, 1, gl);
    }

  // sand, then the tide: per column the water reaches ye(x), wet sand just beyond
  gfx->fillRect(0, SEA_TOP + ROWS * RH, 466, 466 - (SEA_TOP + ROWS * RH), soil);
  uint16_t wet = lerp565(soil, night ? C565(0x08, 0x0c, 0x18) : C565(0x70, 0x5a, 0x3c), 1, 4);
  for (int x = 0; x < 466; x += 4) {
    int ye = 252 + (int)(sinf(now / 1100.0f + x / 46.0f) * 6);
    gfx->fillRect(x, SEA_TOP + ROWS * RH, 4, ye - (SEA_TOP + ROWS * RH), nearC);
    gfx->fillRect(x, ye, 4, 14, wet);
    gfx->fillRect(x, ye - 1, 4, 3, night ? C565(0xa8, 0xc0, 0xe0) : C565(0xff, 0xff, 0xff));      // foam line
    if (((x / 4) + (int)(now / 300)) % 3) gfx->fillRect(x, ye + 5, 4, 1, lerp565(wet, crest, 1, 2));  // thin trail
  }

  // a WINGULL crossing the sky by day, turning round at each edge: the same
  // triangle-wave beat as the walkers, with a slow bob. Its walk cycle is the
  // wingbeat, so the walk action doubles as flying.
  if (!night && !gBeachBirdTried) { gBeachBirdTried = true; gBeachBird.load(278, false); }
  if (!night && gBeachBird.loaded) {
    const uint32_t P = 17000;
    uint32_t u = (now + 5000) % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = -40 + (int)(540ULL * tri / P);
    int y = 156 + (int)(sinf(now / 900.0f) * 9.0f);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gBeachBird.has(act)) act = PMD_IDLE;
    drawPmdActZ(gBeachBird, act, x, y, now, true, false, 1, 140, 220);
  }

  // an EXEGGUTOR pacing the waterline, small and far, turning at each end of its
  // beat. Behind the pet, in front of the sea.
  if (!gBeachTried) { gBeachTried = true; gBeachMon.load(103, false); }
  if (gBeachMon.loaded) {
    static const struct { int16_t y; uint16_t zoom; uint32_t period; uint32_t phase; int16_t lo, hi; } WK[1] = {
      { 262, 125, 30000, 0, 50, 420 } };
    for (int i = 0; i < 1; i++) {
      uint32_t u = (now + WK[i].phase) % (2 * WK[i].period);
      bool right = u < WK[i].period;
      uint32_t tri = right ? u : 2 * WK[i].period - u;
      int x = WK[i].lo + (int)((WK[i].hi - WK[i].lo) * (uint64_t)tri / WK[i].period);
      uint8_t act = right ? PMD_WALKR : PMD_WALKL;
      if (!gBeachMon.has(act)) act = PMD_IDLE;
      drawPmdActZ(gBeachMon, act, x, WK[i].y, now, true, false, 1, WK[i].zoom, 220);
    }
  }

  // shells and a starfish on the dry sand, on the right where the pet is not
  gfx->fillCircle(366, 296, 5, night ? C565(0x70, 0x58, 0x60) : C565(0xf0, 0xb8, 0xb0));
  gfx->fillCircle(366, 296, 2, lerp565(soil, C565(0xff, 0xff, 0xff), 1, 3));
  uint16_t star = night ? C565(0x80, 0x48, 0x30) : C565(0xf0, 0x7a, 0x3a);
  for (int a = 0; a < 5; a++) {
    float an = a * 1.2566f - 1.5708f;
    gfx->fillTriangle(424, 292, 424 + (int)(cosf(an) * 11), 292 + (int)(sinf(an) * 11),
                      424 + (int)(cosf(an + 0.6f) * 4), 292 + (int)(sinf(an + 0.6f) * 4), star);
  }

  // gulls drifting across the sky by day, clear of the header text above y 120
  if (!night)
    for (int g = 0; g < 2; g++) {
      int x = (int)(now / 30 + g * 260) % 540 - 40;
      int y = 130 + g * 18 + (int)(sinf(now / 600.0f + g) * 4);
      int up = ((now / 200 + g) & 1) ? -4 : 2;
      uint16_t gc = lerp565(C565(0x30, 0x38, 0x48), skyAt(y), 1, 4);
      gfx->drawLine(x - 7, y + up, x, y, gc);
      gfx->drawLine(x, y, x + 7, y + up, gc);
    }
}

// Forest biome, back to front: two hazy tree lines, drifting mist, a small
// BEEDRILL and CATERPIE, two big trees on the right that sway, ferns and mushrooms on the ground, leaves
// drifting down by day and fireflies by night. The big trees stay right of the
// pet and below the sun and moon; the lower left is bare (poops land there, see
// drawPoops) and the step counter covers the far left.
// Tree-line layer: a mix of spiky pines and rounded leafy crowns, the kind and
// height fixed per x so nothing flickers. Crowns are two overlapping circles on
// a filled base; pines are single triangles.
static void forestRidge(int baseY, int step, int w, int hmin, int hmax, int seed, int x0, uint16_t col) {
  for (int x = x0; x < 480; x += step) {
    int hsh = (x * 37 + seed * 91) & 0x7fff;
    int ht = hmin + hsh % (hmax - hmin + 1);
    if ((hsh / 7) % 3 == 0) {                                // pine
      gfx->fillTriangle(x, baseY, x + w, baseY, x + w / 2, baseY - ht, col);
    } else {                                                 // leafy crown
      int r = ht * 2 / 5 + 4, cy = baseY - ht + r;
      gfx->fillCircle(x + w / 2 - r / 2, cy + r / 4, r, col);
      gfx->fillCircle(x + w / 2 + r / 2, cy, r - 1, col);
      gfx->fillRect(x + w / 2 - r, cy, 2 * r, baseY - cy, col);
    }
  }
}

// Leaf canopy tones, dark to bright, by day or night.
static void forestTones(bool night, uint16_t t[4]) {
  if (night) { t[0] = C565(0x08, 0x18, 0x12); t[1] = C565(0x0e, 0x26, 0x1a); t[2] = C565(0x16, 0x36, 0x24); t[3] = C565(0x20, 0x46, 0x30); }
  else       { t[0] = C565(0x24, 0x62, 0x38); t[1] = C565(0x38, 0x80, 0x44); t[2] = C565(0x5c, 0xa6, 0x4a); t[3] = C565(0x92, 0xcc, 0x58); }
}

// A leafy clump lit from the upper right: dark body, then mid, light and
// highlight discs stepping toward the sun, with a few leaf specks.
static void forestClump(int cx, int cy, int r, int seed, bool night) {
  uint16_t t[4]; forestTones(night, t);
  gfx->fillCircle(cx, cy, r, t[0]);
  gfx->fillCircle(cx + r / 8, cy - r / 8, r * 85 / 100, t[1]);
  gfx->fillCircle(cx + r / 4, cy - r / 4, r * 58 / 100, t[2]);
  gfx->fillCircle(cx + r * 3 / 8, cy - r * 3 / 8, r * 30 / 100, t[3]);
  for (int i = 0; i < 5; i++) {                              // specks of light and shade
    int hsh = (seed * 53 + i * 29) & 0xff;
    int dx = (hsh % (r + 1)) - r / 3, dy = ((hsh * 7) % (r + 1)) - r * 2 / 3;
    if (dx * dx + dy * dy < r * r * 7 / 10) gfx->fillRect(cx + dx, cy + dy, 2, 2, i & 1 ? t[3] : t[0]);
  }
}

// Broadleaf tree: flared roots, a barked trunk that forks, and a crown of
// clumps, with Oran berries in it. The whole crown sways; each clump also
// flutters a little on its own.
static void forestOak(int cx, int footY, uint32_t now, float sway, bool night) {
  uint16_t bark  = night ? C565(0x1c, 0x14, 0x12) : C565(0x5a, 0x3c, 0x28);
  uint16_t barkD = night ? C565(0x10, 0x0c, 0x0a) : C565(0x3c, 0x26, 0x1a);
  uint16_t barkL = night ? C565(0x2a, 0x1e, 0x1a) : C565(0x84, 0x5c, 0x3a);
  const int FORK = footY - 74;
  gfx->fillTriangle(cx - 16, footY + 2, cx - 6, footY - 16, cx - 6, footY + 2, bark);   // roots
  gfx->fillTriangle(cx + 16, footY + 2, cx + 6, footY - 16, cx + 6, footY + 2, bark);
  gfx->fillRect(cx - 7, FORK, 14, footY - FORK + 2, bark);
  gfx->fillRect(cx + 3, FORK, 4, footY - FORK + 2, barkL);                              // lit edge
  gfx->fillRect(cx - 7, FORK, 3, footY - FORK + 2, barkD);                              // shaded edge
  for (int i = 0; i < 5; i++)                                                           // bark grooves
    gfx->fillRect(cx - 2 + (i % 2) * 3, FORK + 8 + i * 13, 1, 9, barkD);
  int csx = (int)sway, ctop = FORK - 38;                                                // crown centre
  for (int k = 0; k < 2; k++) {                                                         // the two limbs
    int ex = cx + (k ? 24 : -22) + csx, ey = FORK - 26;
    for (int w = -1; w <= 1; w++) gfx->drawLine(cx + w, FORK + 2, ex + w, ey, bark);
  }
  static const int8_t CL[8][3] = { { -34, 26, 20 }, { 32, 28, 21 }, { -16, 6, 26 }, { 22, 4, 24 },
                                   { 0, -18, 24 }, { -40, 4, 16 }, { 38, 2, 16 }, { 0, 30, 20 } };
  for (int i = 0; i < 8; i++)                                                           // back to front, lower first
    forestClump(cx + CL[i][0] + csx + (int)(sinf(now / 1100.0f + i) * 1.5f),
                ctop + 28 + CL[i][1], CL[i][2], i + 3, night);
  uint16_t berry = night ? C565(0x20, 0x30, 0x70) : C565(0x4a, 0x78, 0xe0);
  static const int8_t BERRY[4][2] = { { -18, 18 }, { 14, 30 }, { 30, 14 }, { -4, 2 } };
  for (int i = 0; i < 4; i++) {
    int bx = cx + BERRY[i][0] + csx, by = ctop + 28 + BERRY[i][1];
    gfx->fillCircle(bx, by, 3, berry);
    gfx->fillRect(bx - 1, by - 1, 1, 1, night ? berry : C565(0xc8, 0xe0, 0xff));
  }
}

// A ragged pine: trunk plus four tiers with a jagged hem, three tones, the top
// tiers swaying most. The sun-facing (right) side is lit by day.
static void forestPine(int cx, int footY, int h, int w, float sway, bool night) {
  uint16_t trunk = night ? C565(0x20, 0x18, 0x14) : C565(0x5a, 0x3c, 0x28);
  uint16_t dark  = night ? C565(0x08, 0x14, 0x0e) : C565(0x1a, 0x4c, 0x32);
  uint16_t mid   = night ? C565(0x0c, 0x1e, 0x16) : C565(0x2a, 0x6c, 0x40);
  uint16_t lit   = night ? C565(0x12, 0x2c, 0x20) : C565(0x44, 0x92, 0x4c);
  int top = footY - 22 - h, tierH = h * 45 / 100, step = (h - tierH) / 3;
  gfx->fillRect(cx - 4, footY - 26, 8, 28, trunk);
  gfx->fillRect(cx + 1, footY - 26, 3, 28, night ? C565(0x2a, 0x1e, 0x1a) : C565(0x84, 0x5c, 0x3a));
  for (int i = 0; i < 4; i++) {
    int ay = top + i * step, by = ay + tierH;
    int hw = w * (45 + 18 * i) / 200;
    int sx = (int)(sway * (4 - i) / 4.0f);
    gfx->fillTriangle(cx + sx, ay, cx - hw, by, cx + hw, by, dark);
    gfx->fillTriangle(cx + sx, ay, cx + hw, by, cx - hw / 4, by, mid);
    gfx->fillTriangle(cx + sx, ay, cx + hw, by, cx + hw / 2, by, lit);
    for (int x = cx - hw; x < cx + hw; x += 7) {             // jagged hem: drooping boughs
      int t = x - cx + hw;
      uint16_t c = t > hw * 3 / 2 ? lit : (t > hw * 3 / 4 ? mid : dark);
      gfx->fillTriangle(x, by - 1, x + 7, by - 1, x + 3, by + 5 + (x & 3), c);
    }
  }
}

// A low bush: three clumps.
static void forestBush(int x, int y, int r, bool night) {
  forestClump(x - r * 2 / 3, y, r * 3 / 4, 1, night);
  forestClump(x + r * 2 / 3, y + 1, r * 3 / 4, 2, night);
  forestClump(x, y - r / 3, r, 3, night);
}

// A small BEEDRILL hovers and a CATERPIE crawls far behind the pet by day; a
// ZUBAT flutters about at night, swapped in as the hour changes. Each
// PMD sprite (~140 KB of PSRAM) is loaded once on entering the forest and freed
// when the biome changes.
static PmdMon gForestBee, gForestBug, gForestBat;   // bat: ZUBAT, night only
static bool gForestTried = false, gForestBatTried = false;

static void drawForest(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  uint16_t farC = lerp565(night ? C565(0x10, 0x24, 0x2c) : C565(0x4a, 0x8c, 0x6c), skyAt(HORIZON - 40), 1, 2);
  uint16_t midC = lerp565(night ? C565(0x0c, 0x1c, 0x1c) : C565(0x2e, 0x6c, 0x4a), skyAt(HORIZON - 40), 1, 5);
  float sway = sinf(now / 1500.0f) * 3.0f;

  forestRidge(HORIZON + 2, 20, 30, 28, 50, 1, -10, farC);

  // mist: long soft streaks drifting slowly along the tree line
  uint16_t mist = lerp565(skyAt(HORIZON - 8), C565(0xff, 0xff, 0xff), 1, night ? 12 : 5);
  for (int k = 0; k < 4; k++) {
    int x = (int)((k * 170 + now / 70) % 640) - 140;
    gfx->fillRoundRect(x, HORIZON - 22 + (k & 1) * 12, 130, 9, 4, mist);
  }

  forestRidge(HORIZON + 2, 26, 38, 40, 72, 2, -4, midC);

  // the BEEDRILL drifts on a slow lissajous across the mid air, facing the way it
  // is heading; the CATERPIE crawls back and forth on the ground behind the ferns
  if (night) {                                               // day critters out, bat in
    if (gForestBee.loaded || gForestBug.loaded) { gForestBee.unload(); gForestBug.unload(); }
    gForestTried = false;
    if (!gForestBatTried) { gForestBatTried = true; gForestBat.load(41, false); }
  } else {
    if (gForestBat.loaded) gForestBat.unload();
    gForestBatTried = false;
  }
  if (night && gForestBat.loaded) {                          // erratic: two beats of different speed
    float a = now / 3100.0f, b = now / 1300.0f;
    int x = 190 + (int)(120.0f * sinf(a) + 30.0f * sinf(b * 1.7f));
    int y = 178 + (int)(22.0f * sinf(b) + 8.0f * sinf(a * 3.0f));
    uint8_t act = cosf(a) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gForestBat.has(act)) act = PMD_IDLE;
    drawPmdActZ(gForestBat, act, x, y, now, true, false, 1, 100, 220);
  }
  if (!night && !gForestTried) { gForestTried = true; gForestBee.load(15, false); gForestBug.load(10, false); }
  if (!night && gForestBug.loaded) {
    const uint32_t P = 26000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 230 + (int)(150ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gForestBug.has(act)) act = PMD_IDLE;
    drawPmdActZ(gForestBug, act, x, 258, now, true, false, 2, 70, 220);
  }
  if (!night && gForestBee.loaded) {
    float ph = now / 5200.0f;
    int x = 190 + (int)(130.0f * sinf(ph));
    int y = 176 + (int)(16.0f * sinf(now / 1700.0f));
    uint8_t act = cosf(ph) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gForestBee.has(act)) act = PMD_IDLE;
    drawPmdActZ(gForestBee, act, x, y, now, true, false, 1, 110, 220);
  }

  // ferns and mushrooms, to the right of the pet
  uint16_t fern = night ? C565(0x16, 0x3a, 0x28) : C565(0x4c, 0xa0, 0x58);
  static const int16_t FN[3][2] = { { 306, 266 }, { 322, 288 }, { 338, 262 } };
  for (int f = 0; f < 3; f++)
    for (int b = -2; b <= 2; b++) {
      int ex = FN[f][0] + b * 7, ey = FN[f][1] - 16 + abs(b) * 4 + (int)(sway * 0.3f);
      gfx->drawLine(FN[f][0], FN[f][1], ex, ey, fern);
      gfx->drawLine(FN[f][0] + 1, FN[f][1], ex + 1, ey, fern);
    }
  uint16_t cap = night ? C565(0x70, 0x20, 0x28) : C565(0xe0, 0x44, 0x40);
  uint16_t stem = night ? C565(0x80, 0x78, 0x70) : C565(0xf4, 0xec, 0xdc);
  static const int16_t MU[3][3] = { { 360, 298, 7 }, { 376, 304, 5 }, { 346, 306, 4 } };
  for (int m = 0; m < 3; m++) {
    int x = MU[m][0], y = MU[m][1], r = MU[m][2];
    gfx->fillRect(x - r / 3, y - 2, r * 2 / 3 + 1, r, stem);
    gfx->fillRoundRect(x - r, y - r, r * 2, r + 1, r / 2 + 1, cap);
    gfx->fillRect(x - r / 2, y - r + 2, 2, 2, stem);
    gfx->fillRect(x + r / 3, y - r + 3, 2, 2, stem);
  }

  // the two big pines last, so they stand in front of everything else
  forestPine(384, 262, 66, 44, sway * 0.7f, night);
  forestOak(440, 272, now, sway, night);
  forestBush(404, 296, 13, night);

  if (!night) {  // leaves drifting down on a slow slant
    static const uint16_t LC[3] = { C565(0x6c, 0xb8, 0x4c), C565(0xd8, 0xc0, 0x40), C565(0xd8, 0x80, 0x38) };
    for (int i = 0; i < 6; i++) {
      int x = (i * 83 + (int)(now / 55) + (int)(sinf(now / 600.0f + i * 2.0f) * 12)) % 520 - 30;
      int y = (i * 47 + (int)(now / 30)) % 270 + 36;
      gfx->fillRect(x, y, 4, 2, LC[i % 3]);
      gfx->fillRect(x + 1, y + 2, 2, 1, LC[i % 3]);
    }
  } else {       // fireflies: bob about, each pulsing on its own beat
    uint16_t glow = C565(0xd8, 0xff, 0x70);
    for (int i = 0; i < 10; i++) {
      float pulse = (sinf(now / 350.0f + i * 2.0f) + 1.0f) * 0.5f;
      if (pulse < 0.25f) continue;
      int x = 40 + (i * 97) % 400 + (int)(sinf(now / 700.0f + i) * 14);
      int y = 170 + (i * 53) % 120 + (int)(cosf(now / 900.0f + i * 1.3f) * 10);
      gfx->fillRect(x - 1, y, 5, 3, lerp565(soil, glow, (int)(pulse * 4), 16));   // halo
      gfx->fillRect(x, y - 1, 3, 5, lerp565(soil, glow, (int)(pulse * 4), 16));
      gfx->fillRect(x, y, 3, 3, lerp565(soil, glow, (int)(pulse * 16), 16));
    }
  }
}

// Snow biome, drawn as the POLAR ICE: a flat pale sheet under a wide cold sky,
// a few faceted icebergs standing on it, one open lead, soft pressure ridges and
// cracks, and a thin wind-blown snowfall. At night a full aurora: swaying
// curtains of rays, bright at the base and fading green to cyan to violet,
// mirrored faintly in the ice. Deliberately sparse. Critters: a PIPLUP waddling
// slowly.
// Keep the lower left bare (poops land there, see drawPoops) and everything below
// the sun and moon.

// scanline ellipse (the emulator's GFX has no fillEllipse, and 1 px rows are cheap here)
static void snowEll(int cx, int cy, int rx, int ry, uint16_t c) {
  for (int dy = -ry; dy <= ry; dy++) {
    int w = (int)(rx * sqrtf(1.0f - (float)(dy * dy) / (float)(ry * ry)));
    if (w > 0) gfx->fillRect(cx - w, cy + dy, 2 * w, 1, c);
  }
}

// An iceberg: a fan of seven facets around a centre point, dark on the left to
// bright on the lit right, a darker waterline band and a turquoise glow where it
// meets the ice. 'haze' (0..8) fades it toward the sky for the distant ones.
static void iceberg(int cx, int baseY, int w, int h, int haze, uint16_t sky, uint16_t soil, bool night) {
  static const int8_t P[8][2] = {                              // x, y as percent of w, h (y up)
    { -50, 0 }, { -34, 50 }, { -12, 68 }, { 8, 100 }, { 30, 62 }, { 50, 0 }, { 0, 0 }, { -5, 25 } };
  uint16_t tone[4];
  if (night) { tone[0] = C565(0x20, 0x30, 0x58); tone[1] = C565(0x30, 0x44, 0x74); tone[2] = C565(0x44, 0x5c, 0x90); tone[3] = C565(0x64, 0x80, 0xb4); }
  else       { tone[0] = C565(0x8c, 0xb0, 0xd4); tone[1] = C565(0xb4, 0xd0, 0xe8); tone[2] = C565(0xd8, 0xea, 0xf6); tone[3] = C565(0xff, 0xff, 0xff); }
  for (int i = 0; i < 4; i++) tone[i] = lerp565(tone[i], sky, haze, 10);
  auto X = [&](int i) { return cx + P[i][0] * w / 100; };
  auto Y = [&](int i) { return baseY - P[i][1] * h / 100; };
  static const uint8_t FACE[7][3] = { { 7, 0, 1 }, { 7, 1, 2 }, { 7, 2, 3 }, { 7, 3, 4 }, { 7, 4, 5 }, { 7, 5, 6 }, { 7, 6, 0 } };
  static const uint8_t TONE[7] = { 0, 1, 2, 3, 2, 1, 0 };
  snowEll(cx, baseY + 2, w * 55 / 100, 4, lerp565(soil, night ? C565(0x30, 0x60, 0x90) : C565(0x70, 0xc8, 0xe0), 1, 3 + haze / 2));   // glow in the ice
  for (int f = 0; f < 7; f++)
    gfx->fillTriangle(X(FACE[f][0]), Y(FACE[f][0]), X(FACE[f][1]), Y(FACE[f][1]), X(FACE[f][2]), Y(FACE[f][2]), tone[TONE[f]]);
  gfx->fillRect(cx - w / 2, baseY - 2, w, 3, lerp565(tone[0], night ? C565(0x10, 0x20, 0x50) : C565(0x40, 0x78, 0xb0), 1, 2));   // waterline band
}

// Aurora: swaying curtains of vertical rays. Each column has its own base height,
// ray length and brightness, all moving; rays are brightest at the base, fade
// upward, and shift green -> cyan -> violet. Two layers at different phases give
// depth. Kept clear of the moon by dimming near it. Returns nothing; also tints
// the ice faintly through 'glowAt' so the sky seems to light the ground.
static void drawAurora(uint32_t now, uint16_t top, uint16_t bot, int moonX, int moonY) {
  float t = now / 1000.0f;
  for (int layer = 0; layer < 2; layer++)
    for (int x = 12; x < 454; x += 4) {
      float ph = layer * 1.9f;
      int base = 118 + layer * 14 + (int)(20.0f * sinf(x / 71.0f + t * 0.35f + ph) + 9.0f * sinf(x / 23.0f - t * 0.9f + ph));
      int len = 34 + (int)(24.0f * sinf(x / 43.0f + t * 0.5f + ph) + 12.0f * sinf(x / 15.0f + t * 1.3f));
      float br = 0.55f + 0.45f * sinf(x / 19.0f + t * 1.1f + ph * 2.0f);          // flicker along the curtain
      int dx = x - moonX, dy = base - len / 2 - moonY;
      float d2 = (float)(dx * dx + dy * dy);
      if (d2 < 3600.0f) continue;                                                    // inside 60 px of the moon: leave it alone
      if (d2 < 10000.0f) br *= (d2 - 3600.0f) / 6400.0f;                             // fade in out to 100 px
      if (len < 8 || br < 0.12f) continue;
      for (int j = 0; j < len; j += 6) {
        float f = (float)j / len;                                                    // 0 at the base, 1 at the tip
        int a = (int)(br * (1.0f - f) * (1.0f - f * 0.3f) * 20.0f) + 1;              // brighter at the base
        uint16_t c = f < 0.5f ? lerp565(C565(0x40, 0xff, 0x98), C565(0x40, 0xd8, 0xff), (int)(f * 20), 10)
                              : lerp565(C565(0x40, 0xd8, 0xff), C565(0xb0, 0x60, 0xff), (int)((f - 0.5f) * 20), 10);
        int y = base - j - 6;
        if (y < 18) break;
        gfx->fillRect(x, y, 4, 6, lerp565(lerp565(top, bot, y, HORIZON), c, a, 28));
      }
    }
}

// small walker (PIPLUP, or SEEL without the Sinnoh pack). The sprite is resident
// only while the biome is on screen (~110 KB of PSRAM).
static PmdMon gSnowMon;
static int16_t gSnowMonDex = 0;

static void drawSnow(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  uint16_t sW = night ? C565(0x98, 0xaa, 0xd0) : C565(0xf6, 0xfb, 0xff);
  uint16_t haze = skyAt(HORIZON - 30);

  if (night) drawAurora(now, top, bot, 360, 122);

  // distant icebergs on the horizon, hazed toward the sky
  iceberg(92, B, 120, 62, 3, haze, soil, night);
  iceberg(300, B, 80, 40, 4, haze, soil, night);
  gfx->fillRect(0, B - 1, 466, 3, lerp565(soil, haze, 1, 3));            // soft horizon line

  // the ice sheet: faint aurora glow in it at night, soft ridges, cracks, one open lead
  if (night) {
    float t = now / 1000.0f;
    for (int x = 0; x < 466; x += 8) {
      int a = 1 + (int)((sinf(x / 40.0f + t * 0.5f) + 1.0f) * 2.5f);
      gfx->fillRect(x, B + 2, 8, 22, lerp565(soil, C565(0x40, 0xe0, 0x98), a, 40));
    }
  }
  uint16_t shade = night ? C565(0x30, 0x40, 0x70) : C565(0xc0, 0xd4, 0xea);
  uint16_t lite = night ? C565(0x78, 0x8c, 0xb8) : C565(0xff, 0xff, 0xff);
  snowEll(240, 256, 84, 4, shade);                                      // two long, low pressure ridges
  snowEll(244, 254, 80, 3, lite);
  snowEll(150, 284, 60, 3, shade);
  snowEll(153, 283, 56, 2, lite);
  uint16_t crack = night ? C565(0x28, 0x38, 0x68) : C565(0x88, 0xa8, 0xd0);
  static const int16_t CK[2][6][2] = { { { 296, 246 }, { 312, 258 }, { 306, 270 }, { 330, 282 }, { 326, 296 }, { -1, 0 } },
                                       { { 100, 248 }, { 118, 256 }, { 112, 266 }, { -1, 0 } } };
  for (int c = 0; c < 2; c++)
    for (int i = 0; CK[c][i + 1][0] >= 0; i++)
      gfx->drawLine(CK[c][i][0], CK[c][i][1], CK[c][i + 1][0], CK[c][i + 1][1], crack);
  uint16_t water = night ? C565(0x10, 0x20, 0x4c) : C565(0x3c, 0x6c, 0x9c);   // the open lead, ice-rimmed
  snowEll(398, 272, 62, 6, night ? C565(0x70, 0x84, 0xb0) : C565(0xe8, 0xf2, 0xfa));
  snowEll(398, 272, 56, 4, water);
  snowEll(404, 271, 28, 1, lerp565(water, sW, 1, 3));                    // sky reflected in it
  iceberg(418, 276, 56, 42, 1, haze, soil, night);                       // one nearer berg beside it

  // sparkles on the ice: a few fixed spots, each twinkling on its own beat
  for (int i = 0; i < 8; i++) {
    int x = 130 + volNoise(i, 23) % 320, y = 244 + volNoise(i, 24) % 54;
    if (x < 200 && y > 262) continue;                        // poops land bottom left
    if ((now / 260 + i * 3) % 8 != 0) continue;
    gfx->fillRect(x, y, 2, 2, sW);
    gfx->fillRect(x - 2, y, 6, 1, lerp565(soil, sW, 1, 2));
    gfx->fillRect(x, y - 2, 1, 6, lerp565(soil, sW, 1, 2));
  }

  // the walker: a PIPLUP waddling slowly behind the pet at any hour (SEEL
  // instead if the Sinnoh pack is not on the card, so it is not left empty)
  if (!gSnowMonDex) {
    gSnowMonDex = 393;
    if (!gSnowMon.load(393, false)) { gSnowMonDex = 86; gSnowMon.load(86, false); }
  }
  if (gSnowMon.loaded) {
    const uint32_t P = 30000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 250 + (int)(80ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gSnowMon.has(act)) act = PMD_IDLE;
    drawPmdActZ(gSnowMon, act, x, 262, now, true, false, 2, 70, 220);
  }

  // heavy snowfall in three depths, blown sideways by the wind: many small far
  // flakes, fewer big near ones, each with its own speed and a little wobble
  for (int d = 0; d < 3; d++) {
    int n = 46 - d * 12, sz = 2 + d, drift = 22 - d * 5, fall = 60 + d * 25;
    for (int i = 0; i < n; i++) {
      int x = (i * 71 + d * 37 + 466 + (int)(now / drift) + (int)(sinf(now / 700.0f + i) * 5.0f)) % 466;
      int y = (i * 53 + d * 29 + (int)(now / (38 - d * 9 - i % 3 * 3))) % (HORIZON + fall) - 10;
      if (night) { gfx->fillRect(x, y, sz, sz, d == 0 ? lerp565(skyAt(y), sW, 2, 3) : sW); continue; }
      uint16_t tint = C565(0xa8, 0xc4, 0xe4);                // by day white-on-pale is invisible: tint, plus a shadow
      if (d == 0) { gfx->fillRect(x, y, sz, sz, lerp565(skyAt(y), tint, 2, 3)); continue; }
      gfx->fillRect(x + 1, y + 1, sz, sz, tint);
      gfx->fillRect(x, y, sz, sz, C565(0xff, 0xff, 0xff));
    }
  }
}

// Meadow biome: an open flower meadow of tall grass in the wind. Layered rolling
// hills behind; in front, seven rows of long curved blades whose tips sway in a
// wave that travels across the field (brighter where it crests, stronger in
// gusts), with wildflowers standing above the grass and seed fluff drifting.
// Day: a small BUTTERFREE overhead and a RATTATA scurrying through the grass;
// night: fireflies and a VENOMOTH, everything darker. Kept calm. The lower left
// stays short and bare (poops land there, see drawPoops), the sun and moon clear.

// small residents: a flyer (BUTTERFREE by day, VENOMOTH at night) and a RATTATA
// by day, each loaded only while wanted and freed when the biome changes.
static PmdMon gMeadowFly, gMeadowWalk;
static int16_t gMeadowFlyDex = 0;
static bool gMeadowWalkTried = false;

static void drawMeadow(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  float gust = 1.0f + 0.6f * sinf(now / 3300.0f);             // 0.4..1.6: the wind comes and goes

  // layered hills: pale and hazy far away, richer green near, tiny bushes on the middle one
  uint16_t farH = lerp565(night ? C565(0x18, 0x34, 0x34) : C565(0x6c, 0xaa, 0x8c), skyAt(HORIZON - 30), 1, 3);
  uint16_t midH = night ? C565(0x14, 0x38, 0x2a) : C565(0x4c, 0x98, 0x58);
  snowEll(70, B, 150, 26, farH);
  snowEll(330, B, 170, 22, farH);
  snowEll(455, B, 100, 30, farH);
  snowEll(190, B + 2, 165, 18, midH);
  snowEll(425, B + 2, 125, 20, midH);
  forestBush(150, B - 14, 9, night);
  forestBush(322, B - 12, 8, night);
  forestBush(262, B - 8, 6, night);

  // residents: the flyer by day/night (the rattata is drawn mid-grass below)
  int16_t wantFly = night ? 49 : 12;
  if (gMeadowFlyDex != wantFly) { gMeadowFly.unload(); gMeadowFlyDex = wantFly; gMeadowFly.load(wantFly, false); }
  if (gMeadowFly.loaded) {
    float ph = now / 6200.0f;
    int x = 200 + (int)(120.0f * sinf(ph)), y = 188 + (int)(14.0f * sinf(now / 1500.0f));
    uint8_t act = cosf(ph) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gMeadowFly.has(act)) act = PMD_IDLE;
    drawPmdActZ(gMeadowFly, act, x, y, now, true, false, 1, 110, 220);
  }
  if (night) { if (gMeadowWalk.loaded) gMeadowWalk.unload(); gMeadowWalkTried = false; }
  else if (!gMeadowWalkTried) { gMeadowWalkTried = true; gMeadowWalk.load(19, false); }

  // grass tones, dark to bright, and the flower palette (day / night)
  uint16_t gT[4];
  if (night) { gT[0] = C565(0x0e, 0x24, 0x1a); gT[1] = C565(0x18, 0x38, 0x28); gT[2] = C565(0x26, 0x50, 0x36); gT[3] = C565(0x5a, 0x7c, 0x5a); }
  else       { gT[0] = C565(0x34, 0x7c, 0x3c); gT[1] = C565(0x4c, 0x9c, 0x46); gT[2] = C565(0x70, 0xbc, 0x54); gT[3] = C565(0xdc, 0xf2, 0x8c); }
  static const uint16_t FC[2][4] = {
    { C565(0xff, 0xff, 0xff), C565(0xe8, 0x40, 0x3c), C565(0x58, 0x88, 0xf0), C565(0xff, 0xd8, 0x3c) },
    { C565(0xa0, 0xa8, 0xc0), C565(0x80, 0x34, 0x40), C565(0x34, 0x4c, 0x90), C565(0xa0, 0x90, 0x40) } };
  uint16_t lav = night ? C565(0x58, 0x40, 0x80) : C565(0xa8, 0x70, 0xe0);

  // seven rows of grass, back to front, each with the flowers that stand in it
  for (int r = 0; r < 7; r++) {
    int y0 = 238 + r * 11, len = 10 + r * 4, sp = 9 + r / 2;
    float amp = 2.0f + r * 0.8f;
    gfx->fillRect(0, y0 - 2, 466, 8, lerp565(soil, gT[0], 1, 3));        // shadowed root band under the blades
    if (r == 3 && gMeadowWalk.loaded && !night) {                         // the rattata runs through the middle of the grass
      const uint32_t P = 22000;
      uint32_t u = now % (2 * P);
      bool right = u < P;
      uint32_t tri = right ? u : 2 * P - u;
      int rx = 236 + (int)(90ULL * tri / P);
      uint8_t act = right ? PMD_WALKR : PMD_WALKL;
      if (!gMeadowWalk.has(act)) act = PMD_IDLE;
      drawPmdActZ(gMeadowWalk, act, rx, 262, now, true, false, 2, 70, 220);
    }
    for (int i = -1; i * sp < 480; i++) {
      int x = i * sp + volNoise(i + r * 17, 45) % sp;
      if (x < 170 && y0 > 262) continue;                                  // keep the poop corner short and bare
      float wv = sinf(now / 560.0f + x / 48.0f + r * 0.5f);              // the travelling wave
      int off = (int)(wv * amp * gust) + volNoise(i + r * 7, 46) % 5 - 2;
      int bl = len - volNoise(i + r * 11, 47) % (len / 3 + 1);
      int mx = x + off * 2 / 5, my = y0 - bl * 55 / 100, tx = x + off, ty = y0 - bl;
      uint16_t c = gT[volNoise(i + r * 5, 48) % 3];
      uint16_t tip = (wv * gust > 0.8f) ? gT[3] : lerp565(c, gT[3], 1, 4); // crests glint
      gfx->drawLine(x, y0 + 4, mx, my, c);
      gfx->drawLine(mx, my, tx, ty, tip);
      if (r >= 3) { gfx->drawLine(x + 1, y0 + 4, mx + 1, my, c); gfx->drawLine(mx + 1, my, tx + 1, ty, tip); }
    }
    for (int f = 0; f < 44; f++) {                                       // wildflowers rooted in this row
      int fy = 240 + volNoise(f, 42) % 66;
      if (fy < y0 || fy >= y0 + 11) continue;
      int fx = 14 + volNoise(f, 43) % 440;
      if (fx < 170 && fy > 262) continue;
      float wv = sinf(now / 560.0f + fx / 48.0f + r * 0.5f);
      int sw = (int)(wv * (amp + 1.0f) * gust), fl = len + 8 + volNoise(f, 49) % 8;
      int hx = fx + sw, hy = fy - fl, kind = volNoise(f, 44) % 6;
      gfx->drawLine(fx, fy + 2, fx + sw / 2, fy - fl / 2, gT[1]);
      gfx->drawLine(fx + sw / 2, fy - fl / 2, hx, hy, gT[1]);
      if (kind == 5) {                                                    // lavender: a tall violet spike
        gfx->fillRect(hx - 1, hy - 7, 3, 9, lav);
        gfx->fillRect(hx - 2, hy - 4, 5, 2, lav);
      } else {
        uint16_t fc = FC[night][kind % 4];
        if (kind == 1) { gfx->fillRect(hx - 3, hy - 2, 7, 5, fc); gfx->fillRect(hx - 1, hy - 1, 3, 3, night ? C565(0x20, 0x10, 0x14) : C565(0x30, 0x20, 0x20)); }   // poppy
        else if (kind == 3) { gfx->fillRect(hx - 2, hy - 2, 5, 5, fc); gfx->fillRect(hx - 1, hy - 2, 2, 1, C565(0xff, 0xf4, 0xa0)); }                              // buttercup
        else { gfx->fillRect(hx - 3, hy - 1, 7, 3, fc); gfx->fillRect(hx - 1, hy - 3, 3, 7, fc); gfx->fillRect(hx - 1, hy - 1, 3, 3, night ? C565(0x80, 0x70, 0x30) : C565(0xff, 0xc0, 0x20)); }   // daisy / cornflower
      }
    }
  }

  if (!night) {  // seed fluff and pollen carried along on the wind
    for (int i = 0; i < 10; i++) {
      int x = (i * 53 + (int)(now / 14) + (int)(sinf(now / 600.0f + i) * 6.0f)) % 520 - 30;
      int y = 190 + (i * 11) % 90 + (int)(sinf(now / 450.0f + i * 1.7f) * 5.0f);
      gfx->fillRect(x, y, 2, 2, C565(0xff, 0xf6, 0xd0));
      gfx->fillRect(x + 2, y - 1, 1, 1, C565(0xff, 0xff, 0xff));
    }
  } else {       // fireflies: bob about, each pulsing on its own beat
    uint16_t glow = C565(0xd8, 0xff, 0x70);
    for (int i = 0; i < 10; i++) {
      float pulse = (sinf(now / 350.0f + i * 2.0f) + 1.0f) * 0.5f;
      if (pulse < 0.25f) continue;
      int x = 40 + (i * 97) % 400 + (int)(sinf(now / 700.0f + i) * 14);
      int y = 200 + (i * 53) % 100 + (int)(cosf(now / 900.0f + i * 1.3f) * 10);
      gfx->fillRect(x - 1, y, 5, 3, lerp565(soil, glow, (int)(pulse * 4), 16));
      gfx->fillRect(x, y - 1, 3, 5, lerp565(soil, glow, (int)(pulse * 4), 16));
      gfx->fillRect(x, y, 3, 3, lerp565(soil, glow, (int)(pulse * 16), 16));
    }
  }
}

// Mountain biome: high alpine country. Back to front: a hazy range, a mid range,
// one big near peak on the left (all faceted fans with snow caps and ragged snow
// hems, lit from the right), drifting fog banks, a rocky cliff on the right with
// a waterfall into a misty pool, one small pine, boulders and scree. Day: a
// FEAROW soaring and a GEODUDE trundling behind the pet; night: a GOLBAT and a
// CUBONE. The peaks stay below the sun and moon; the lower left is bare (poops
// land there, see drawPoops).

// A peak: a fan of eight facets around a centre point, dark on the left to bright
// on the lit right, then a snow cap (shaded left, lit right) with a ragged hem.
// 'haze' (0..10) fades it toward the sky for the far ones; snowPct is how far down
// the cap reaches.
static void mtnPeak(int cx, int baseY, int w, int H, int seed, int haze, uint16_t sky, int snowPct, bool night) {
  static const int8_t V[8][2] = { { -100, 0 }, { -52, 46 }, { -26, 70 }, { 0, 100 }, { 24, 72 }, { 50, 44 }, { 100, 0 }, { 12, 0 } };
  uint16_t tone[4], sW, sS;
  if (night) { tone[0] = C565(0x14, 0x18, 0x2c); tone[1] = C565(0x1e, 0x24, 0x40); tone[2] = C565(0x2c, 0x34, 0x54); tone[3] = C565(0x40, 0x4a, 0x70); sW = C565(0xa0, 0xb0, 0xd8); sS = C565(0x5c, 0x6c, 0x9c); }
  else       { tone[0] = C565(0x4a, 0x4e, 0x6a); tone[1] = C565(0x6a, 0x6e, 0x86); tone[2] = C565(0x8a, 0x8e, 0xa4); tone[3] = C565(0xb4, 0xb6, 0xc8); sW = C565(0xf8, 0xfc, 0xff); sS = C565(0xc0, 0xd0, 0xea); }
  for (int i = 0; i < 4; i++) tone[i] = lerp565(tone[i], sky, haze, 10);
  sW = lerp565(sW, sky, haze, 12); sS = lerp565(sS, sky, haze, 12);
  int px[8], py[8];
  for (int i = 0; i < 8; i++) {
    int jx = volNoise(i, seed) % 9 - 4, jy = (i == 0 || i == 6 || i == 7) ? 0 : volNoise(i + 9, seed) % 7 - 3;
    px[i] = cx + (V[i][0] + jx) * w / 100;
    py[i] = baseY - (V[i][1] + jy) * H / 100;
  }
  int ccx = cx, ccy = baseY - 34 * H / 100;
  static const uint8_t TN[8] = { 0, 1, 2, 3, 2, 1, 0, 0 };         // tone of the facet starting at vertex i
  for (int i = 0; i < 8; i++) {
    int j = (i + 1) & 7;
    gfx->fillTriangle(ccx, ccy, px[i], py[i], px[j], py[j], tone[TN[i]]);
  }
  // snow cap, a fraction of the way down each ridge from the summit
  auto lx = [&](int a, int b) { return px[3] + (px[a] - px[3]) * snowPct / 100; };
  auto ly = [&](int a, int b) { return py[3] + (py[a] - py[3]) * snowPct / 100; };
  int sx[5] = { lx(1, 0), lx(2, 0), px[3], lx(4, 0), lx(5, 0) };    // lower edge of the cap, left to right
  int sy[5] = { ly(1, 0), ly(2, 0), py[3] + (baseY - py[3]) * snowPct / 160, ly(4, 0), ly(5, 0) };
  gfx->fillTriangle(px[3], py[3], sx[0], sy[0], sx[1], sy[1], sS);
  gfx->fillTriangle(px[3], py[3], sx[1], sy[1], sx[2], sy[2], sS);
  gfx->fillTriangle(px[3], py[3], sx[2], sy[2], sx[3], sy[3], sW);
  gfx->fillTriangle(px[3], py[3], sx[3], sy[3], sx[4], sy[4], sW);
  for (int k = 0; k < 4; k++)                                        // ragged hem: teeth hanging off the lower edge
    for (int t = 0; t < 3; t++) {
      int ax = sx[k] + (sx[k + 1] - sx[k]) * t / 3, ay = sy[k] + (sy[k + 1] - sy[k]) * t / 3;
      int bx = sx[k] + (sx[k + 1] - sx[k]) * (t + 1) / 3, by = sy[k] + (sy[k + 1] - sy[k]) * (t + 1) / 3;
      gfx->fillTriangle(ax, ay, bx, by, (ax + bx) / 2, (ay + by) / 2 + 3 + volNoise(k * 3 + t, seed + 3) % 6, k < 2 ? sS : sW);
    }
}

// A grey boulder with a lit upper right, shaded lower left, and a tuft on top.
static void mtnBoulder(int cx, int cy, int r, int seed, bool night) {
  uint16_t t[4];
  if (night) { t[0] = C565(0x18, 0x1c, 0x2c); t[1] = C565(0x28, 0x2e, 0x44); t[2] = C565(0x3a, 0x42, 0x5c); t[3] = C565(0x50, 0x5a, 0x78); }
  else       { t[0] = C565(0x5c, 0x58, 0x5c); t[1] = C565(0x7c, 0x78, 0x78); t[2] = C565(0x9c, 0x96, 0x92); t[3] = C565(0xc0, 0xb8, 0xae); }
  const int N = 8;
  int vx[N + 1], vy[N + 1];
  for (int i = 0; i < N; i++) {
    float an = i * 6.2832f / N;
    int rr = r * (80 + volNoise(i, seed) % 26) / 100;
    vx[i] = cx + (int)(cosf(an) * rr * 1.25f);
    vy[i] = cy + (int)(sinf(an) * rr * 0.8f);
  }
  vx[N] = vx[0]; vy[N] = vy[0];
  for (int i = 0; i < N; i++) {
    float an = (i + 0.5f) * 6.2832f / N, lit = cosf(an) * 0.6f - sinf(an) * 0.8f;
    gfx->fillTriangle(cx, cy - r / 6, vx[i], vy[i], vx[i + 1], vy[i + 1], lit > 0.75f ? t[3] : (lit > 0.2f ? t[2] : (lit > -0.4f ? t[1] : t[0])));
  }
  uint16_t g = night ? C565(0x1c, 0x3a, 0x2c) : C565(0x58, 0x9c, 0x4c);                // moss on top
  gfx->fillRect(cx - r / 3, cy - r * 4 / 5, r * 2 / 3, 2, g);
  gfx->fillRect(cx - r / 4, cy - r * 4 / 5 - 2, r / 2, 2, g);
}

static PmdMon gMtnFly, gMtnWalk;
static int16_t gMtnDay = -1;   // which set is loaded: 1 day, 0 night, -1 none

static void drawMountain(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  float sway = sinf(now / 1500.0f) * 3.0f;
  uint16_t sW = night ? C565(0xa0, 0xb0, 0xd8) : C565(0xf8, 0xfc, 0xff);

  // three ranges, back to front: hazy and low, mid, then the big near peak on the left
  uint16_t hz = skyAt(HORIZON - 50);
  for (int i = 0; i < 6; i++)
    mtnPeak(10 + i * 88, B, 56 + volNoise(i, 61) % 30, 46 + volNoise(i, 62) % 34, 61 + i, 7, hz, 42, night);
  mtnPeak(60, B, 110, 78, 71, 4, hz, 40, night);
  mtnPeak(300, B, 90, 52, 72, 4, hz, 44, night);                 // below the sun and moon
  mtnPeak(430, B, 80, 48, 73, 4, hz, 44, night);
  mtnPeak(172, B, 124, 118, 81, 0, hz, 44, night);                // the big one, lit from the right

  // fog banks drifting across the ranges
  uint16_t fog = lerp565(skyAt(HORIZON - 20), C565(0xff, 0xff, 0xff), 1, night ? 9 : 4);
  for (int k = 0; k < 3; k++) {
    int x = (int)((k * 190 + now / 90) % 640) - 150;
    snowEll(x, HORIZON - 18 + k * 9, 100 + k * 14, 4, fog);
    snowEll(x + 30, HORIZON - 21 + k * 9, 60, 3, fog);
  }

  // the cliff on the right: ragged-edged strata, shaded left edge, lit right side, grass on top
  uint16_t rc[4];
  if (night) { rc[0] = C565(0x14, 0x18, 0x28); rc[1] = C565(0x20, 0x26, 0x3c); rc[2] = C565(0x2e, 0x36, 0x50); rc[3] = C565(0x42, 0x4c, 0x6c); }
  else       { rc[0] = C565(0x4c, 0x44, 0x48); rc[1] = C565(0x6c, 0x62, 0x60); rc[2] = C565(0x88, 0x7c, 0x74); rc[3] = C565(0xa8, 0x98, 0x88); }
  const int CT = 178, CB = 266;                                     // cliff top and foot
  for (int y = CT; y < CB; y += 2) {
    int xl = 376 + volNoise(y / 4, 63) % 9 - (y - CT) / 14;          // ragged, flaring out toward the foot
    int band = (y / 12 + volNoise(y / 12, 64) % 2) % 3;               // strata
    gfx->fillRect(xl, y, 12, 2, rc[0]);
    gfx->fillRect(xl + 12, y, 466 - xl - 12, 2, rc[band == 2 ? 2 : 1]);
    gfx->fillRect(430, y, 40, 2, rc[band == 0 ? 2 : 3]);             // the lit side
    if (volNoise(y, 65) < 50) gfx->fillRect(xl + 14 + volNoise(y, 66) % 40, y, 3, 2, rc[0]);   // cracks and pits
  }
  uint16_t gTop = night ? C565(0x1c, 0x3c, 0x2a) : C565(0x58, 0xa0, 0x4c);
  uint16_t gTopL = night ? C565(0x2a, 0x54, 0x38) : C565(0x86, 0xcc, 0x58);
  gfx->fillRect(372, CT - 5, 100, 6, gTop);
  gfx->fillRect(420, CT - 5, 50, 3, gTopL);
  for (int x = 374; x < 466; x += 6)                                 // grass hanging over the edge
    gfx->fillTriangle(x, CT, x + 6, CT, x + 3, CT + 4 + volNoise(x, 67) % 5, gTop);

  // the waterfall: a pale ribbon from a notch in the lip to a misty pool, with bright
  // streaks racing down it and spray rising at the foot
  const int WX = 414;
  uint16_t wt = night ? C565(0x70, 0x90, 0xc8) : C565(0xb8, 0xdc, 0xf4);
  gfx->fillRect(WX - 5, CT - 3, 10, 6, lerp565(rc[0], wt, 1, 2));    // the notch
  for (int y = CT + 2; y < CB + 6; y += 2) {
    int wob = (int)(sinf(y / 14.0f + now / 500.0f) * 1.0f);
    gfx->fillRect(WX - 4 + wob, y, 8, 2, wt);
    gfx->fillRect(WX + 2 + wob, y, 2, 2, lerp565(wt, C565(0x30, 0x58, 0x88), 1, 4));   // shaded edge
  }
  int wlen = CB - CT;
  for (int k = 0; k < 5; k++) {
    int yy = CT + 4 + (int)((now / 9 + k * 37) % wlen);
    gfx->fillRect(WX - 3 + (k * 3) % 5, yy, 2, 8, C565(0xff, 0xff, 0xff));
  }
  snowEll(WX + 4, 270, 46, 8, night ? C565(0x3a, 0x4a, 0x70) : C565(0x78, 0x9c, 0xc0));            // pool rim
  snowEll(WX + 4, 270, 42, 6, night ? C565(0x1a, 0x2c, 0x58) : C565(0x4c, 0x80, 0xb4));            // water
  for (int k = 0; k < 3; k++) {                                                                      // ripples spreading out from the splash
    int r = (int)((now / 40 + k * 14) % 42);
    gfx->fillRect(WX + 4 - r, 270, 1, 1, lerp565(C565(0x4c, 0x80, 0xb4), sW, 2, 3));
    gfx->fillRect(WX + 4 + r, 270, 1, 1, lerp565(C565(0x4c, 0x80, 0xb4), sW, 2, 3));
  }
  for (int k = 0; k < 6; k++) {                                                                      // spray: puffs rising and thinning
    int life = (int)((now / 26 + k * 40) % 120);
    int px = WX + (int)(sinf(k * 2.1f + now / 400.0f) * (8 + life / 8)), py = 266 - life / 3;
    gfx->fillCircle(px, py, 3 + life / 24, lerp565(skyAt(py), C565(0xff, 0xff, 0xff), 8 - life / 18, 14));
  }

  // an alpine pine at the cliff foot, boulders around it; tufts and scree on the ground
  forestPine(380, 270, 52, 34, sway * 0.6f, night);
  mtnBoulder(352, 292, 12, 1, night);
  mtnBoulder(300, 300, 10, 2, night);
  mtnBoulder(460, 296, 11, 3, night);
  uint16_t gD = night ? C565(0x14, 0x30, 0x24) : C565(0x4c, 0x86, 0x44);
  for (int i = 0; i < 16; i++) {
    int x = 190 + volNoise(i, 68) % 220, y = 250 + volNoise(i, 69) % 50;
    if (x > 372 && y < 270) continue;
    int sw = (int)(sinf(now / 600.0f + x / 40.0f) * 1.5f);
    gfx->drawLine(x, y, x - 3 + sw, y - 6, gD);
    gfx->drawLine(x + 1, y, x + 1 + sw, y - 8, gD);
    gfx->drawLine(x + 2, y, x + 5 + sw, y - 5, gD);
  }
  for (int i = 0; i < 26; i++) {                                    // scree: scattered pebbles
    int x = 140 + volNoise(i, 70) % 320, y = 240 + volNoise(i, 71) % 62;
    if ((x < 200 && y > 262) || (x > 366 && y < 274)) continue;
    gfx->fillRect(x, y, 2 + i % 2, 2, night ? C565(0x2a, 0x30, 0x48) : (i & 1 ? C565(0x84, 0x7c, 0x78) : C565(0xb8, 0xae, 0xa0)));
  }
  static const int16_t EW[5][2] = { { 268, 262 }, { 318, 278 }, { 232, 296 }, { 340, 258 }, { 280, 292 } };   // edelweiss
  for (int i = 0; i < 5; i++) {
    int x = EW[i][0], y = EW[i][1];
    gfx->drawLine(x, y + 2, x, y - 4, gD);
    gfx->fillRect(x - 2, y - 6, 5, 3, night ? C565(0x90, 0xa0, 0xc0) : C565(0xff, 0xff, 0xf0));
    gfx->fillRect(x, y - 5, 1, 1, C565(0xe0, 0xc0, 0x40));
  }

  // residents: FEAROW soars by day / GOLBAT at night, a GEODUDE or CUBONE trundles behind the pet
  int16_t day = night ? 0 : 1;
  if (gMtnDay != day) {
    gMtnFly.unload(); gMtnWalk.unload(); gMtnDay = day;
    gMtnFly.load(day ? 22 : 42, false);
    gMtnWalk.load(day ? 74 : 104, false);
  }
  if (gMtnFly.loaded) {                                            // a long slow glide, turning at each end, clear of the moon
    const uint32_t P = 20000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 60 + (int)(250ULL * tri / P), y = 168 + (int)(sinf(now / 1400.0f) * 8.0f);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gMtnFly.has(act)) act = PMD_IDLE;
    drawPmdActZ(gMtnFly, act, x, y, now, true, false, 1, 100, 220);
  }
  if (gMtnWalk.loaded) {
    const uint32_t P = 26000;
    uint32_t u = now % (2 * P);
    bool right = u < P;
    uint32_t tri = right ? u : 2 * P - u;
    int x = 240 + (int)(80ULL * tri / P);
    uint8_t act = right ? PMD_WALKR : PMD_WALKL;
    if (!gMtnWalk.has(act)) act = PMD_IDLE;
    drawPmdActZ(gMtnWalk, act, x, 262, now, true, false, 2, 70, 220);
  }
}

// Graveyard biome for ghost types: always dusky, never cheerful. Back to front:
// bare gnarled trees on the horizon, a haunted house with flickering windows on a
// far hill, drifting fog, an iron fence, weathered gravestones (round, cross,
// obelisk, slab) with moss and carved lines, scattered bones, more fog, and a few
// will-o'-wisps. A small GASTLY (HAUNTER at night) drifts about; at night two bats
// cross the sky. The lower left is bare (poops land there, see
// drawPoops), and the sun and moon stay clear.

// One gravestone, lit cold from the upper right: a stone body per 'kind', a lit
// right edge, a shadowed left, moss at the foot, a hairline crack, carved lines.
// kind: 0 rounded, 1 cross, 2 obelisk, 3 slab (leaning by 'lean' px).
static void graveStone(int cx, int footY, int kind, int w, int h, int lean, int seed, bool night) {
  uint16_t stL = night ? C565(0x5c, 0x64, 0x88) : C565(0xa8, 0xa2, 0xb8);
  uint16_t stM = night ? C565(0x40, 0x48, 0x68) : C565(0x84, 0x7e, 0x98);
  uint16_t stD = night ? C565(0x26, 0x2c, 0x48) : C565(0x5a, 0x56, 0x6c);
  uint16_t moss = night ? C565(0x1e, 0x3a, 0x34) : C565(0x5c, 0x8c, 0x58);
  uint16_t earth = night ? C565(0x1c, 0x1a, 0x2a) : C565(0x44, 0x40, 0x52);
  snowEll(cx, footY + 1, w * 70 / 100, 4, earth);                              // the mound it stands in
  int x0 = cx - w / 2, top = footY - h;
  if (kind == 0) {                                                            // rounded headstone
    gfx->fillRect(x0, top + w / 2, w, h - w / 2, stM);
    gfx->fillCircle(cx, top + w / 2, w / 2, stM);
    gfx->fillRect(x0 + w - 3, top + w / 2, 3, h - w / 2, stL);
    gfx->fillRect(x0, top + w / 2, 3, h - w / 2, stD);
    gfx->fillRect(cx - 2, top + 4, 4, 5, stD);                                // a small carved cross
    gfx->fillRect(cx - 1, top + 2, 2, 9, stD);
  } else if (kind == 1) {                                                     // a cross
    int bw = w / 3;
    gfx->fillRect(cx - bw / 2, top, bw, h, stM);
    gfx->fillRect(x0, top + h / 4, w, bw, stM);
    gfx->fillRect(cx + bw / 2 - 2, top, 2, h, stL);
    gfx->fillRect(x0 + w - 2, top + h / 4, 2, bw, stL);
    gfx->fillRect(cx - bw / 2, top + h / 4 + bw, 2, h - h / 4 - bw, stD);
  } else if (kind == 2) {                                                     // an obelisk on a plinth
    int pw = w, ph = h / 5;
    gfx->fillRect(cx - pw / 2, footY - ph, pw, ph, stM);
    gfx->fillRect(cx - pw / 2, footY - ph, pw, 2, stL);
    int bw = w * 60 / 100;
    gfx->fillRect(cx - bw / 2, top + bw, bw, h - ph - bw, stM);
    gfx->fillTriangle(cx - bw / 2, top + bw, cx + bw / 2, top + bw, cx, top, stL);
    gfx->fillRect(cx + bw / 2 - 3, top + bw, 3, h - ph - bw, stL);
    gfx->fillRect(cx - bw / 2, top + bw, 3, h - ph - bw, stD);
  } else {                                                                    // a slab, leaning, a corner chipped off
    for (int y = 0; y < h; y += 2) {
      int xo = x0 + lean * (h - y) / h;
      gfx->fillRect(xo, top + y, w, 2, stM);
      gfx->fillRect(xo + w - 3, top + y, 3, 2, stL);
      gfx->fillRect(xo, top + y, 3, 2, stD);
    }
    gfx->fillTriangle(x0 + w - 5 + lean, top, x0 + w + lean, top, x0 + w + lean, top + 5, earth);
  }
  gfx->fillRect(cx - w / 2, footY - 4, w, 4, moss);                           // moss creeping up the foot
  for (int i = 0; i < 4; i++) gfx->fillRect(cx - w / 2 + volNoise(i, seed) % w, footY - 4 - volNoise(i + 5, seed) % 5, 2, 2, moss);
  gfx->drawLine(cx - 2, top + h / 2, cx + 3, top + h * 3 / 4, stD);           // a hairline crack
  if (kind != 1) for (int i = 0; i < 2; i++) gfx->fillRect(cx - w / 4, top + h / 2 + i * 4 - 2, w / 2, 1, stD);   // carved lines
}

// A will-o'-wisp: a flickering teardrop flame, violet outside, white-hot inside.
static void graveWisp(int x, int y, uint32_t now, int seed, bool night) {
  float fl = sinf(now / 90.0f + seed * 2.3f);
  int r = 3 + (fl > 0.3f);
  uint16_t out = night ? C565(0x90, 0x70, 0xe8) : C565(0xa8, 0x98, 0xd0);
  gfx->fillTriangle(x - r, y, x + r, y, x + (int)(fl * 2), y - r * 3, out);
  gfx->fillCircle(x, y + 1, r, out);
  gfx->fillTriangle(x - r / 2, y, x + r / 2, y, x + (int)(fl * 1.5f), y - r * 2, C565(0xe4, 0xd8, 0xff));
  gfx->fillRect(x - 1, y, 2, 2, C565(0xff, 0xff, 0xff));
}

// Bones for the graveyard floor. Ivory, lit on the upper right, shaded below.
static void graveBoneCols(bool night, uint16_t c[3]) {
  if (night) { c[0] = C565(0xb4, 0xbc, 0xd4); c[1] = C565(0x7c, 0x86, 0xa4); c[2] = C565(0x2a, 0x30, 0x4c); }
  else       { c[0] = C565(0xf0, 0xe8, 0xd0); c[1] = C565(0xbc, 0xb0, 0x94); c[2] = C565(0x60, 0x56, 0x4a); }
}

// A long bone (femur): a shaded shaft with a lit edge and a knobbed pair at each end.
static void graveFemur(int x0, int y0, int x1, int y1, bool night) {
  uint16_t c[3]; graveBoneCols(night, c);
  gfx->drawLine(x0, y0 + 1, x1, y1 + 1, c[1]);
  gfx->drawLine(x0, y0, x1, y1, c[0]);
  int dx = x1 - x0, dy = y1 - y0, len = (int)sqrtf((float)(dx * dx + dy * dy));
  if (len < 1) return;
  int px = -dy * 2 / len, py = dx * 2 / len;                                 // perpendicular, 2 px
  for (int e = 0; e < 2; e++) {
    int ex = e ? x1 : x0, ey = e ? y1 : y0;
    gfx->fillCircle(ex + px, ey + py, 2, c[0]);
    gfx->fillCircle(ex - px, ey - py, 2, c[1]);
  }
}

// A skull, optionally sunk into the ground up to the jaw.
static void graveSkull(int x, int y, int r, bool buried, bool night, uint16_t earth) {
  uint16_t c[3]; graveBoneCols(night, c);
  gfx->fillCircle(x, y, r, c[0]);
  gfx->fillCircle(x - r / 4, y + r / 4, r * 3 / 4, c[1]);                    // shaded lower left
  gfx->fillCircle(x + r / 5, y - r / 5, r * 3 / 4, c[0]);                    // lit upper right
  gfx->fillRect(x - r * 2 / 3, y + r / 2, r * 4 / 3, r / 2 + 1, c[1]);       // jaw
  for (int t = 0; t < 4; t++) gfx->fillRect(x - r / 2 + t * (r / 3 + 1), y + r / 2 + 1, 1, r / 3, c[2]);   // teeth
  gfx->fillRect(x - r / 2 - 1, y - r / 6, r * 2 / 5 + 2, r / 2 + 1, c[2]);   // eye sockets
  gfx->fillRect(x + r / 10, y - r / 6, r * 2 / 5 + 2, r / 2 + 1, c[2]);
  gfx->fillTriangle(x - 1, y + r / 3, x + 2, y + r / 3, x, y + r / 6, c[2]);   // nose
  if (buried) snowEll(x, y + r, r + 3, r / 2 + 1, earth);
}

// Half a ribcage: a short spine with curved ribs both sides, shrinking toward the bottom.
static void graveRibs(int x, int y, bool night) {
  uint16_t c[3]; graveBoneCols(night, c);
  gfx->drawLine(x, y, x, y + 20, c[1]);
  gfx->drawLine(x + 1, y, x + 1, y + 20, c[0]);
  for (int k = 0; k < 5; k++)
    for (int side = -1; side <= 1; side += 2) {
      int ry = y + 2 + k * 4, w = 11 - k;
      gfx->drawLine(x, ry, x + side * w, ry + 3, side > 0 ? c[0] : c[1]);
      gfx->drawLine(x + side * w, ry + 3, x + side * (w - 2), ry + 8, side > 0 ? c[0] : c[1]);
    }
}

// A row of vertebrae curving across the ground.
static void graveSpine(int x, int y, bool night) {
  uint16_t c[3]; graveBoneCols(night, c);
  for (int i = 0; i < 7; i++) {
    int vx = x + i * 5, vy = y + (int)(sinf(i * 0.7f) * 3.0f);
    gfx->fillRect(vx, vy, 4, 3, c[i & 1 ? 1 : 0]);
    gfx->fillRect(vx + 1, vy - 2, 2, 2, c[1]);                              // the spinous process
  }
}

static PmdMon gGhostMon;
static int16_t gGhostDex = 0;

static void drawGraveyard(uint32_t now, bool night, int h, uint16_t top, uint16_t bot, uint16_t soil) {
  auto skyAt = [&](int y) { return lerp565(top, bot, y < 0 ? 0 : y, HORIZON); };
  const int B = HORIZON + 2;
  uint16_t hz = skyAt(HORIZON - 30);
  uint16_t dark = night ? C565(0x0c, 0x0a, 0x1c) : C565(0x30, 0x2a, 0x40);

  // bare gnarled trees along the horizon, hazed toward the sky
  uint16_t farT = lerp565(dark, hz, 1, 3);
  for (int i = 0; i < 9; i++) {
    int x = 14 + i * 56 + volNoise(i, 81) % 20, ht = 34 + volNoise(i, 82) % 30;
    gfx->drawLine(x, B, x + volNoise(i, 83) % 5 - 2, B - ht, farT);
    gfx->drawLine(x + 1, B, x + 1 + volNoise(i, 83) % 5 - 2, B - ht, farT);
    for (int b = 0; b < 4; b++) {                                              // crooked branches, alternating sides
      int by = B - ht * (30 + b * 18) / 100, dir = (b & 1) ? 1 : -1, len = 8 + volNoise(i + b, 84) % 10;
      gfx->drawLine(x, by, x + dir * len, by - len / 2 - 3, farT);
      gfx->drawLine(x + dir * len, by - len / 2 - 3, x + dir * (len + 5), by - len - 4, farT);
    }
  }

  // the haunted house on a far hill: a main block, a roof with a gable, a tower, lit windows
  snowEll(280, B, 100, 12, lerp565(dark, hz, 1, 4));
  uint16_t hs = lerp565(dark, hz, 1, 5);
  gfx->fillRect(246, B - 40, 70, 40, hs);                                      // main block
  gfx->fillTriangle(240, B - 40, 322, B - 40, 281, B - 62, hs);               // roof
  gfx->fillRect(252, B - 66, 16, 26, hs);                                      // tower
  gfx->fillTriangle(249, B - 66, 271, B - 66, 260, B - 88, hs);               // its spire
  gfx->fillRect(300, B - 58, 6, 14, hs);                                       // chimney
  uint16_t win = night ? C565(0xff, 0xd0, 0x70) : C565(0xd8, 0xc0, 0x80);
  static const int16_t WN[5][2] = { { 256, 56 }, { 262, 28 }, { 274, 24 }, { 290, 24 }, { 303, 24 } };   // x, height above the base
  for (int i = 0; i < 5; i++) {
    int flick = volNoise((int)(now / 160) + i * 7, 85);                         // some windows flicker out for a beat
    if (flick < (night ? 26 : 90)) continue;
    gfx->fillRect(WN[i][0], B - WN[i][1], 5, 7, i == 1 ? C565(0xa8, 0x80, 0xff) : win);
  }

  // low fog behind the fence
  uint16_t fog = lerp565(soil, C565(0xd0, 0xc8, 0xf0), 1, night ? 7 : 4);
  for (int k = 0; k < 3; k++) {
    int x = (int)((k * 220 + now / (60 + k * 17)) % 700) - 170;
    snowEll(x, B + 6 + k * 6, 110 + k * 10, 5, fog);
  }

  // iron fence along the back: pointed bars between two rails, with taller posts
  uint16_t fc = night ? C565(0x10, 0x10, 0x20) : C565(0x2e, 0x2a, 0x3c);
  uint16_t fl = night ? C565(0x34, 0x3c, 0x60) : C565(0x6c, 0x66, 0x80);
  gfx->fillRect(0, B + 6, 466, 2, fc);
  gfx->fillRect(0, B + 16, 466, 2, fc);
  for (int x = 4; x < 466; x += 9) {
    gfx->fillRect(x, B, 2, 22, fc);
    gfx->fillTriangle(x - 1, B, x + 3, B, x + 1, B - 5, fc);
    gfx->fillRect(x + 1, B + 1, 1, 18, fl);                                    // lit edge
  }
  for (int x = 40; x < 466; x += 117) {                                          // taller posts with ball tops
    gfx->fillRect(x, B - 8, 5, 30, fc);
    gfx->fillCircle(x + 2, B - 10, 4, fc);
    gfx->fillRect(x + 3, B - 6, 2, 26, fl);
  }

  // gravestones: three rows back to front, nothing in the poop corner
  graveStone(196, B + 20, 0, 22, 28, 0, 1, night);
  graveStone(262, B + 18, 1, 26, 34, 0, 2, night);
  graveStone(330, B + 20, 2, 22, 40, 0, 3, night);
  graveStone(404, B + 22, 3, 24, 28, 4, 4, night);
  graveStone(300, B + 40, 3, 26, 28, -5, 5, night);
  graveStone(372, B + 46, 0, 30, 36, 0, 6, night);
  graveStone(442, B + 40, 1, 26, 32, 0, 7, night);

  // bones scattered about the ground, clear of the poop corner
  {
    uint16_t earth = night ? C565(0x1c, 0x1a, 0x2a) : C565(0x44, 0x40, 0x52);
    graveSpine(178, 274, night);
    graveFemur(288, 268, 312, 261, night);
    graveSkull(356, 262, 6, true, night, earth);
    graveSkull(330, 284, 8, false, night, earth);
    graveFemur(380, 288, 406, 280, night);
    graveFemur(388, 280, 402, 292, night);                                  // the two cross in the middle
    graveRibs(422, 262, night);
  }

  // fog drifting in front of the stones
  for (int k = 0; k < 3; k++) {
    int x = (int)((k * 240 + 200 + now / (45 + k * 13)) % 720) - 180;
    snowEll(x, 286 + k * 8, 130 + k * 12, 6, lerp565(soil, C565(0xd0, 0xc8, 0xf0), 1, night ? 6 : 4));
  }

  // wisps drifting among the graves, more of them at night
  for (int i = 0; i < (night ? 5 : 2); i++) {
    int x = 190 + (int)(sinf(now / 2100.0f + i * 1.9f) * 80.0f) + i * 36;
    int y = 262 - (int)((sinf(now / 1300.0f + i * 2.4f) + 1.0f) * 12.0f) - i * 4;
    if (x < 170 && y > 250) continue;
    graveWisp(x, y, now, i, night);
  }

  // the small ghost: GASTLY by day, HAUNTER at night, drifting about the upper air
  int16_t want = night ? 93 : 92;
  if (gGhostDex != want) { gGhostMon.unload(); gGhostDex = want; gGhostMon.load(want, false); }
  if (gGhostMon.loaded) {
    float ph = now / 7400.0f;
    int x = 180 + (int)(110.0f * sinf(ph)), y = 176 + (int)(12.0f * sinf(now / 1900.0f));
    uint8_t act = cosf(ph) > 0 ? PMD_WALKR : PMD_WALKL;
    if (!gGhostMon.has(act)) act = PMD_IDLE;
    drawPmdActZ(gGhostMon, act, x, y, now, true, false, 1, night ? 80 : 100, 220);
  }

  // two bats crossing the night sky
  if (night)
    for (int g = 0; g < 2; g++) {
      int x = 520 - (int)(now / 22 + g * 300) % 640, y = 96 + g * 30 + (int)(sinf(now / 500.0f + g * 2.0f) * 8.0f);
      int up = ((now / 110 + g) & 1) ? -5 : 3;
      uint16_t bc = C565(0x0a, 0x08, 0x14);
      gfx->fillRect(x - 1, y - 1, 3, 3, bc);
      gfx->drawLine(x - 8, y + up, x - 1, y, bc);
      gfx->drawLine(x + 1, y, x + 8, y + up, bc);
      gfx->drawLine(x - 8, y + up, x - 5, y + up + 3, bc);
      gfx->drawLine(x + 8, y + up, x + 5, y + up + 3, bc);
    }
}

void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  uint16_t top, bot;
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // sunrise
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // day
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // sunset

  if (biome == BIOME_GRAVEYARD) {  // graveyard: always dusky, violet-grey, even at noon
    top = lerp565(top, night ? C565(0x1c, 0x10, 0x34) : C565(0x4c, 0x40, 0x6c), night ? 1 : 3, night ? 2 : 4);
    bot = lerp565(bot, night ? C565(0x42, 0x2c, 0x5c) : C565(0xb4, 0x9c, 0xc4), 1, 2);
  }
  if (biome == BIOME_SNOW) {  // snow: cold, overcast tint
    top = lerp565(top, night ? C565(0x0a, 0x18, 0x30) : C565(0xa8, 0xc0, 0xd8), 1, 3);
    bot = lerp565(bot, night ? C565(0x2a, 0x3c, 0x66) : C565(0xea, 0xf2, 0xfa), 1, 2);
  }
  if (biome == BIOME_FOREST && !night) bot = lerp565(bot, C565(0xcc, 0xe8, 0xd4), 1, 3);   // forest: faint green haze
  if (biome == BIOME_VOLCANO) {  // volcano: ash-red sky, hazier toward the horizon
    top = lerp565(top, night ? C565(0x2a, 0x0e, 0x14) : C565(0x6a, 0x2c, 0x2c), 1, 2);
    bot = lerp565(bot, night ? C565(0x6a, 0x24, 0x1c) : C565(0xf0, 0x7c, 0x3c), 1, 2);
  }

  // sky in bands
  for (int y = 0; y < HORIZON; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, HORIZON));

  // sol o luna
  if (night) {
    const int moonY = 122;   // clear of the header text above it
    drawStars(now, top, bot, HORIZON - 10);
    drawMoon(360, moonY, lerp565(top, bot, moonY, HORIZON));
  } else if (h < 18) {
    drawSun(360, 84, 24, h < 8 ? C565(0xff, 0xc8, 0x6a) : C565(0xff, 0xd9, 0x5c),
            lerp565(top, bot, 84, HORIZON), now);
    drawClouds(now, biome == BIOME_VOLCANO ? C565(0xc8, 0x9c, 0x90) : C565(0xff, 0xff, 0xff),
               lerp565(top, bot, 150, HORIZON));
  } else {
    drawSun(233, biome == BIOME_BEACH ? SEA_TOP : HORIZON - 6, 32, C565(0xff, 0xe6, 0xa0),
            lerp565(top, bot, biome == BIOME_BEACH ? SEA_TOP : HORIZON - 6, HORIZON), now);  // setting sun
  }

  // beach sea: a strip of water over the sand
  uint16_t soil = BIOME_SOIL[biome < BIOME_COUNT ? biome : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (biome != BIOME_BEACH && (gBeachMon.loaded || gBeachTried || gBeachBird.loaded || gBeachBirdTried)) {
    gBeachMon.unload(); gBeachTried = false;
    gBeachBird.unload(); gBeachBirdTried = false;
  }
  if (biome != BIOME_GRAVEYARD && (gGhostMon.loaded || gGhostDex)) { gGhostMon.unload(); gGhostDex = 0; }
  if (biome != BIOME_MOUNTAIN && (gMtnFly.loaded || gMtnWalk.loaded || gMtnDay != -1)) {
    gMtnFly.unload(); gMtnWalk.unload(); gMtnDay = -1;
  }
  if (biome != BIOME_MEADOW && (gMeadowFly.loaded || gMeadowWalk.loaded || gMeadowFlyDex || gMeadowWalkTried)) {
    gMeadowFly.unload(); gMeadowWalk.unload(); gMeadowFlyDex = 0; gMeadowWalkTried = false;
  }
  if (biome != BIOME_SNOW && (gSnowMon.loaded || gSnowMonDex)) { gSnowMon.unload(); gSnowMonDex = 0; }
  if (biome != BIOME_VOLCANO && (gVolcanoMon.loaded || gVolcanoTried)) { gVolcanoMon.unload(); gVolcanoTried = false; }
  if (biome != BIOME_FOREST && (gForestBee.loaded || gForestBug.loaded || gForestBat.loaded || gForestTried || gForestBatTried)) {
    gForestBee.unload(); gForestBug.unload(); gForestBat.unload();
    gForestTried = gForestBatTried = false;
  }
  if (biome == BIOME_BEACH) {  // beach: draws its own sea, sand and tide
    drawBeach(now, night, h, top, bot, soil);
    return;
  }

  // ground
  gfx->fillRect(0, HORIZON, 466, 466 - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);

  // biome details
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == BIOME_FOREST) {  // forest
    drawForest(now, night, h, top, bot, soil);
  } else if (biome == BIOME_VOLCANO) {  // volcano
    drawVolcano(now, night, top, bot);
  } else if (biome == BIOME_MOUNTAIN) {  // mountain
    drawMountain(now, night, h, top, bot, soil);
  } else if (biome == BIOME_SNOW) {  // snow
    drawSnow(now, night, h, top, bot, soil);
  } else if (biome == BIOME_GRAVEYARD) {  // graveyard
    drawGraveyard(now, night, h, top, bot, soil);
  } else if (biome == BIOME_MEADOW) {  // meadow
    drawMeadow(now, night, h, top, bot, soil);
  }
}

// first game: pick a starter among Bulbasaur / Charmander / Squirtle
const UiFont &uiFitAll(const UiFont &f, const char *const *s, const int *maxW, int n);   // defined with uiTextFit

void renderStarterSelect() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  uiTextFit(UIF_SMALL, CX, 82, t, UI_INK, 1, 300);
  // the three names share one size: the longest one decides it
  const char *nm[3]; int nw[3]; int nn = starterCountShown(pet.region);
  if (nn > 3) nn = 3;
  for (int i = 0; i < nn; i++) { nm[i] = DEX_TBL[starterOf(pet.region, i)].name; nw[i] = 208; }
  const UiFont &nameFont = uiFitAll(UIF_BIG, nm, nw, nn);
  for (int i = 0; i < nn; i++) {
    int16_t d = starterOf(pet.region, i);
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // starter thumbnail (if the SD is ready)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    uiText(nameFont, 178, ry + 45, de.name, UI_INK, 0);
  }
  gfx->flush();
}

// ---------- crash breadcrumbs ----------
//
// "It has a mini crash" is not a bug report you can act on, because the board
// never says WHY it restarted. These three live in RTC memory, which survives
// a panic, a watchdog and a software reset -- everything except pulling the
// power -- so the next boot can say what the firmware was doing when it died.
//
// Written every frame. That is one word to RTC RAM per render, which costs
// nothing next to a full-screen redraw, and it means the crumb always names
// the screen that was actually on the panel rather than the last one opened.
#define CRUMB_MAGIC 0x7AABE10C
RTC_NOINIT_ATTR uint32_t gCrumbMagic;
RTC_NOINIT_ATTR uint32_t gCrumbScreen;
RTC_NOINIT_ATTR uint32_t gCrumbHeap;


// Which screen is on the panel RIGHT NOW, in the same order render() tests.
uint8_t uiCurrentScreen() {
  if (pet.awaitingStarter()) return starterRegionDone ? SCR_STARTER : SCR_REGION;
  if (galleryOpen) return galleryPick ? SCR_DEXPICK : SCR_GALLERY;
  if (movePickOpen) return SCR_MOVEPICK;
  if (partyOpen) return boxOpen ? SCR_BOX : SCR_PARTY;
  if (kbOpen) return SCR_KEYBOARD;
  if (cardOpen) return SCR_CARD;
  if (playerOpen) return SCR_PLAYER;
  if (clockOpen) return SCR_CLOCK;
  if (btlWinUntil) return SCR_WIN;
  if (battleOpen) return SCR_BATTLE;
  if (pickOpen) return SCR_PICK;
  if (lanOpen) return SCR_LAN;
  if (gymOpen) return gymPick ? SCR_GYMPICK : SCR_GYM;
  if (pet.hasLearnOffer()) return SCR_LEARN;
  if (gameOpen || sackOpen || spdOpen) return SCR_GAME;
  if (trainOpen) return SCR_TRAIN;
  if (menuOpen) return SCR_MENU;
  return SCR_MAIN;
}

static void crumbDrop() {
  gCrumbMagic = CRUMB_MAGIC;
  gCrumbScreen = uiCurrentScreen();
  gCrumbHeap = ESP.getFreeHeap();
}

static const char *resetReasonName(int r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software (our own restart)";
    case ESP_RST_PANIC:    return "PANIC -- a crash";
    case ESP_RST_INT_WDT:  return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG -- something blocked too long";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT -- the supply sagged";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "unknown";
  }
}

// Printed once at boot. On a clean start it is one line; after a crash it says
// which screen was up and how much heap was left, which is the whole point.
void bootReport() {
  int r = (int)esp_reset_reason();
  bool bad = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
              r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
  Serial.printf("boot: reset=%s heap=%u psram=%u\n", resetReasonName(r),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  if (bad && gCrumbMagic == CRUMB_MAGIC) {
    Serial.printf("CRASH: it died on the '%s' screen with heap=%u\n",
                  gCrumbScreen < SCR_COUNT ? SCREEN_NAME[gCrumbScreen] : "?",
                  (unsigned)gCrumbHeap);
  } else if (bad) {
    Serial.println("CRASH: no breadcrumb (RTC memory was lost too)");
  }
  gCrumbMagic = 0;   // one report per crash, not on every later boot
}

void render() {
  crumbDrop();   // so a crash can name the screen it happened on
  if (pet.awaitingStarter()) {  // first game: region, then starter
    if (!starterRegionDone) renderRegionPick(RPICK_FOR_START);
    else renderStarterSelect();
    return;
  }
  if (galleryOpen) {
    if (galleryPick) { renderRegionPick(RPICK_FOR_DEX); return; }
    renderGallery();
    return;
  }
  if (movePickOpen) {
    renderMovePick();
    return;
  }
  if (partyOpen) {
    if (boxOpen) {
      if (boxDetail) renderBoxDetail();
      else renderBox();
    } else if (partyDetail) renderPartyDetail();
    else renderParty();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (spdOpen) {
    renderSpeed();
    return;
  }
  if (trainOpen) {
    renderTrain();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  if (battleOpen) {
    btlLinkPoll();
    renderBattle();
    return;
  }
  if (pickOpen) {
    renderPick();
    return;
  }
  if (lanOpen) {
    renderLan();
    return;
  }
  if (gymOpen) {
    if (gymPick) renderRegionPick(RPICK_FOR_GYMS);
    else renderGyms();
    return;
  }
  if (playerOpen) {
    renderPlayer();
    return;
  }
  if (pet.hasLearnOffer()) {
    renderLearn();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  int h = sceneHour();
  gNight = h < 6 || h >= 20;   // by the clock only: a creature asleep at noon sleeps in daylight
  // drawScene covers the full 466x466: no fillScreen(BLACK) beforehand so
  // that an overlapping DMA flush never captures half-painted black (anti-flicker)
  drawScene(sceneBiome(), millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(d.name, nameOnSky(d.accent), msg);
    drawCeremony();
    gfx->flush();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      uiText(UIF_SMALL, CX, 316 + 14, rar,
             pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98, 1);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount(), DEX_COUNT);
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    uiText(UIF_SMALL, CX, 344 + 14, reg, inkColor(), 1);

    // Which generation this egg comes from. It lives HERE rather than in the
    // settings screen because this is the only moment it does anything: the
    // species is decided when the egg appears, so choosing the region is
    // something you do to the egg in front of you.
    drawEggRegion();
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : d.name;
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : nameOnSky(d.accent), statusMsg());
    drawStreakBadge();
    drawPet();
    drawEatFood();
    drawBath();
    drawPoops();
    drawStepPlate();
    // lower panel: clean base for bars and buttons over the landscape
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawUpHint();
    drawCelebration();
    if (pet.wantEvolveButton()) drawEvolveButton();        // red CTA: evolve
    else if (pet.canRunawayNow()) drawRunawayButton();     // gloomy CTA: runaway (neglect)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // golden CTA: farewell
  }

  if (pet.sleeping) drawSnore();

  // food picker
  if (feedMenuUntil) {
    if (millis() > feedMenuUntil) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101, 288, 264, 64, 14, inkColor());
      drawMap(SPR_ICON_FOOD, 16, 110, 296, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, 176, 296, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, 242, 296, 3, false);
      drawMap(SPR_ICON_CANDY, 16, 308, 296, 3, false);
    }
  }

  // "release?" dialog (long press on the creature)
  if (confirmUntil) {
    if (millis() > confirmUntil) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[28];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), DEX_TBL[pet.speciesId].name);
      uiTextFit(UIF_SMALL, CX, 196 + 14, q, UI_INK, 1, 258);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      uiTextFit(UIF_SMALL, 118 + 50, 270 + 14, T(S_YES), UI_WHITE, 1, 92);
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      uiTextFit(UIF_SMALL, 248 + 50, 270 + 14, T(S_NO), UI_WHITE, 1, 92);
    }
  }

  // decision dialog (evolve/keep, farewell/stay together)
  if (choiceKind) {
    if (millis() > choiceUntil) choiceKind = 0;
    else drawChoiceDialog();
  }

  // "<name> joined the party!" after a farewell or release
  if (partyBannerUntil) {
    if (millis() > partyBannerUntil) {
      partyBannerUntil = 0;
    } else {
      char b[40];
      snprintf(b, sizeof(b), T(S_PARTY_JOINED), partyBannerName);
      gfx->fillRoundRect(53, 176, 360, 74, 16, UI_BAR_OK);
      gfx->drawRoundRect(53, 176, 360, 74, 16, UI_INK);
      uiTextFit(UIF_SMALL, CX, 206 + 14, b, UI_WHITE, 1, 340);
    }
  }

  if (menuOpen) drawMenu();

  gfx->flush();
}

// ---------- minigame: taps with the pokeball ----------

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameOpen = true;
  gameOverUntil = 0;
  gameUntil = millis() + GAME_MS;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  hitTime = 0;
  gamePetX = 233;
  respawnBall();
}

void respawnBall() {
  ballX = 150 + random(166);
  ballY = 96;
  float sp = 1.6f + gameScore * 0.05f;  // livelier as you progress
  if (sp > 4.0f) sp = 4.0f;
  ballVX = random(2) ? sp : -sp;
  ballVY = 0;
}

// Leaving a minigame early banks what was actually earned rather than voiding
// it. Quitting used to forfeit everything, which mattered little when the ball
// game trained a stat you could grind back -- but it is now purely about
// happiness, and a pet that just played should be happier for it. The
// gameOver/over guards stop a swipe during the results screen paying twice.
void leaveGame() {
  if (!gameOverUntil) pet.playResult(gameScore);
  gameOpen = false;
}
void leaveSack() {
  if (!sackOverUntil) pet.trainStrength(sackHits);
  sackOpen = false;
}
void leaveSpeed() {
  if (!spdOverUntil) pet.trainSpeed(spdHits);
  spdOpen = false;
}

void gameTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  // The ball is checked BEFORE the quit strip. The ball bounces inside a circle
  // of radius 205 about the centre, so it reaches y=28 -- well inside the y<72
  // header. Reaching up to hit a high ball used to abandon the game instead,
  // silently forfeiting the score, the speed training and the record.
  float dx = ballX - x, dy = ballY - y;
  bool onBall = (dx * dx + dy * dy < 74 * 74);
  if (!onBall && y < 72) {  // tap the header = quit, keeping what was earned
    leaveGame();
    return;
  }
  if (onBall) {  // ball hit!
    gameScore++;
    sfxPlay(SFX_PLAY);
    // softer hit: moderate impulse that grows slowly with the score
    float lift = 6.6f + (gameScore > 16 ? 3.5f : gameScore * 0.22f);
    ballVY = -lift;
    ballVX += dx * 0.12f;
    if (ballVX > 6.5f) ballVX = 6.5f;
    if (ballVX < -6.5f) ballVX = -6.5f;
    hitX = ballX;
    hitY = ballY;
    hitTime = millis();
  }
}

void stepGame() {
  float grav = 0.40f + gameScore * 0.013f;  // falls a little faster each time
  if (grav > 0.80f) grav = 0.80f;
  ballVY += grav;
  ballX += ballVX;
  ballY += ballVY;
  // bounce off the circular wall
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      ballVX = (ballVX - 2 * dot * nx) * 0.85f;
      ballVY = (ballVY - 2 * dot * ny) * 0.85f;
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  // the clock, or three misses, whichever lands first
  if (gameUntil && millis() >= gameUntil && !gameOverUntil) {
    gameNewHi = (gameScore > pet.gameHi);
    pet.playResult(gameScore);
    sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
    gameOverUntil = millis() + 4000;
    return;
  }
  if (ballY > 384) {  // to the ground
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // updates the record and gives happiness
      sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
    }
  }
  // the creature follows it along the bottom
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase;
}

// ---------- punching bag (trains strength) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
}

void sackTap() {
  if (millis() >= sackUntil) return;  // time is already up
  sackHits++;
  sackShake = 16;  // shakes the bag
}

void drawGameScene();  // prototype (defined further below)

void renderSack() {
  uint32_t now = millis();
  drawGameScene();  // habitat background
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // result screen
  if (sackOverUntil) {
    if (now > sackOverUntil) { sackOpen = false; return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    uiTextFit(UIF_BIG, CX, 178, b, ink, 1, 340);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    uiTextFit(UIF_BIG, CX, 231, g, UI_BAR_BAD, 1, 340);
    if (sackNewHi && sackHits > 0) {
      uiText(UIF_SMALL, CX, 270, T(S_NEW_RECORD), UI_BAR_WARN, 1);
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      uiText(UIF_SMALL, CX, 270, r, ink, 1);
    }
    gfx->flush();
    return;
  }

  // the 10 s are over: apply the training
  if (now >= sackUntil) {
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    gfx->flush();
    return;
  }

  // vigorous mashing
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // hook/rope
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // chain
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // bag
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // lid
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // seam

  // hit counter
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  uiText(UIF_HUGE, CX, 310, buf, ink, 1);

  uiTextFit(UIF_SMALL, CX, 336, T(S_HIT_FAST), ink, 1, 330);

  // time bar
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK_TEXT);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);

  gfx->flush();
}

// minigame background: the creature's habitat (sky by hour + biome ground)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));
  if (night)
    drawStars(millis(), top, bot, 225);
  uint8_t bio = sceneBiome();
  uint16_t soil = BIOME_SOIL[bio < BIOME_COUNT ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

void renderGame() {
  // no fillScreen(BLACK): drawGameScene covers the full 466x466. If the
  // DMA of the previous flush is still reading the buffer, it will see valid content (not black
  // half-painted), which was the flicker at 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameOverUntil) {
    drawGameScene();
    if (millis() > gameOverUntil) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    uiTextFit(UIF_BIG, CX, 188, buf, ink, 1, 340);
    if (gameNewHi && gameScore > 0) {
      uiText(UIF_SMALL, CX, 228, T(S_NEW_RECORD), UI_BAR_WARN, 1);
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      uiText(UIF_SMALL, CX, 228, rec, ink, 1);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    uiTextFit(UIF_SMALL, CX, 264, msg, ink, 1, 340);
    gfx->flush();
    return;
  }

  drawGameScene();
  stepGame();

  // score, record and lives
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  uiText(UIF_BIG, CX, 58, buf, ink, 1);
  char rec[12];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  uiText(UIF_SMALL, CX, 90, rec, ink, 1);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK_TEXT);
  }
  // The clock, drawn like the bag's and the reaction test's so all three games
  // read the same way. Thin and near the rim: the middle belongs to the ball.
  if (!gameOverUntil) {
    uint32_t now2 = millis();
    uint32_t left = (gameUntil > now2) ? gameUntil - now2 : 0;
    int bw = 200, fw = (int)((uint32_t)bw * left / GAME_MS);
    gfx->fillRoundRect(CX - bw / 2, 124, bw, 10, 4, UI_TRACK_TEXT);
    if (fw > 2)
      gfx->fillRoundRect(CX - bw / 2, 124, fw, 10, 4,
                         left < 5000 ? UI_BAR_WARN : UI_BAR_OK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++)
      for (int c = 0; c < mon.w; c++) {
        uint8_t idx = fr[r * mon.w + c];
        if (idx == 0xFF) continue;
        gfx->fillRect(px + c * s, py + r * s, s, s, mon.pal[idx]);
      }
  }

  // impact ring that expands and fades (soft hit feedback)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // the pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  gfx->flush();
}

// ---------- creature card (vertical swipe) ----------

// one row of the card: label, bar, value and (if iv != IV_NONE) the individual
// value that sets that stat's ceiling
// (no default argument: Arduino's prototype generator drops them
// and calls that omit it would not compile)
#define IV_NONE 0xFF
void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar,
                  uint16_t color, uint8_t iv) {
  uiTextFit(UIF_SMALL, 70, y + 14, label, UI_INK, 0, 56);
  // The bar used to start at 112, which leaves 42px for a label drawn at size 2
  // -- three characters. BOND (EN), LIEN (FR) and LACO (PT) are four, so the
  // label ran under the bar. 132 fits five, with the bar narrowed to keep the
  // number clear of it.
  int bw = 130;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(132, y + 2, bw, 11, 3, UI_TRACK_TEXT);
  if (fw > 2) gfx->fillRoundRect(132, y + 2, fw, 11, 3, color);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  uiText(UIF_SMALL, 272, y + 14, num, UI_INK, 0);
  if (iv != IV_NONE) {
    char b[10];
    snprintf(b, sizeof(b), T(S_IV_FMT), iv);
    // a perfect IV is highlighted: it is the stroke of luck the player is after
    uiText(UIF_SMALL, 344, y + 14, b, iv >= 31 ? UI_BAR_WARN : UI_TRACK_TEXT, 0);
  }
}

// ---------- on-screen time setting (swipe down) ----------
// The user sets their LOCAL time by eye; the firmware uses it as is, so
// there is no time zone to manage. Preserves the day (does not break streak/age).

void openClock() {
  uint32_t e = rtcEpoch();
  if (!e) e = pet.lastSeenEpoch;
  clockH = (e / 3600) % 24;
  clockM = (e / 60) % 60;
  settingsPage = SET_TIME;
  clockOpen = true;
}

void applyClock() {
  uint32_t base = rtcEpoch();
  if (!base) base = pet.lastSeenEpoch;
  uint32_t e = (base / 86400) * 86400 + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  clockOpen = false;
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  uiText(UIF_BIG, x + 29, y + 42, l, UI_INK, 1);
}

// ---------- settings: four pages, swiped left/right ----------
// TIME / VOLUME / LANGUAGE / ABOUT. Only the time page has anything to
// confirm (OK applies the clock); sound and language take effect as soon as
// they are tapped, so the other pages' OK/cancel simply close the screen.

#define SET_OK_X 133
#define SET_OK_Y 358
#define SET_OK_W 200
#define SET_OK_H 48
#define SET_DOTS_Y 330

// volume page geometry
#define SND_SW_X 84              // sound master switch, left of centre
#define SND_SW_Y 130
#define SND_SW_W 140
#define SND_SW_H 52
#define SND_TEST_X 242           // TEST: beep at the chosen level, right of it
#define VOL_ROW_Y 214
#define VOLS_MINUS_X 36          // volume row: buttons pushed out, bar longer
#define VOLS_PLUS_X 370
#define VOLS_BAR_X 110
#define VOLS_BAR_W 246
#define VOLS_BAR_H 24
#define VOL_STEP 1
#define VOL_MINUS_X 96
#define VOL_PLUS_X 310
#define VOL_BTN_W 60
#define VOL_BTN_H 60
#define VOL_BAR_X 176
#define VOL_BAR_W 114
#define BRI_ROW_Y 214

// language page: a 2-column grid, one pill per language
#define LANG_GRID_X 68
#define LANG_GRID_Y 92
#define LANG_CELL_W 160
#define LANG_CELL_H 48
#define LANG_CELL_GAP 8
// the language's own name, which is never translated; ASCII except zh/ko,
// whose glyphs come from cjkfont.h
static const char *const LANG_NAMES[LANG_COUNT] = {
  "Espanol", "English", "Francais", "Deutsch", "Italiano", "Portugues",
  "中文", "한국어" };
// The order the pills are SHOWN in. The Lang enum is what NVS stores, so it
// must never be reordered; this table is the only place the display order lives.
static const Lang LANG_ORDER[LANG_COUNT] = {
  LANG_EN, LANG_ZH, LANG_ES, LANG_KO, LANG_FR, LANG_DE, LANG_IT, LANG_PT };

static void langCell(int i, int &x, int &y) {
  x = LANG_GRID_X + (i % 2) * (LANG_CELL_W + LANG_CELL_GAP);
  y = LANG_GRID_Y + (i / 2) * (LANG_CELL_H + LANG_CELL_GAP);
}

static void settingsTitle(const char *s) {
  uiTextFit(UIF_BIG, CX, 65, s, UI_INK, 1, 300);
}

// One slider for volume (0..100) and brightness (1..10). The knob centre runs
// inside the track, so the whole circle stays within the bar at both ends, and
// sliderValue() is the exact inverse of sliderKnobX().
static int sliderKnobX(int v, int lo, int hi) {
  return VOLS_BAR_X + VOLS_BAR_H / 2 + (VOLS_BAR_W - VOLS_BAR_H) * (v - lo) / (hi - lo);
}
static int sliderValue(int x, int lo, int hi) {
  int span = VOLS_BAR_W - VOLS_BAR_H;
  int t = x - VOLS_BAR_X - VOLS_BAR_H / 2;
  if (t < 0) t = 0;
  if (t > span) t = span;
  return lo + (t * (hi - lo) + span / 2) / span;
}
static void drawSlider(int v, int lo, int hi) {
  int by = VOL_ROW_Y + 28;
  int kx = sliderKnobX(v, lo, hi);
  gfx->fillRoundRect(VOLS_BAR_X, by, VOLS_BAR_W, VOLS_BAR_H, 12, UI_TRACK_TEXT);
  gfx->fillRoundRect(VOLS_BAR_X, by, kx - VOLS_BAR_X, VOLS_BAR_H, 12, UI_BAR_OK);
  gfx->fillCircle(kx, by + VOLS_BAR_H / 2, VOLS_BAR_H / 2, UI_WHITE);   // the knob
  gfx->drawCircle(kx, by + VOLS_BAR_H / 2, VOLS_BAR_H / 2, UI_INK);
}

void renderClock() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  if (settingsPage == SET_TIME) {
    settingsTitle(T(S_SET_TIME));
    char t[8];
    // 12-hour display; clockH stays 0..23 underneath so the +/- buttons and
    // the stored epoch are unchanged
    snprintf(t, sizeof(t), "%02d:%02d", clockH % 12 == 0 ? 12 : clockH % 12, clockM);
    {
      // time block (digits + AM/PM) centred as a whole
      const char *ap = clockH < 12 ? "AM" : "PM";
      int tw = uiTextWidth(UIF_HUGE, t), aw = uiTextWidth(UIF_BIG, ap);
      int x0 = CX - (tw + 10 + aw) / 2;
      uiText(UIF_HUGE, x0, 160, t, UI_INK, 0);
      uiText(UIF_BIG, x0 + tw + 10, 160, ap, UI_INK, 0);
    }

    drawClockBtn(104, 190, "-");  // hora -
    drawClockBtn(170, 190, "+");  // hora +
    drawClockBtn(252, 190, "-");  // min -
    drawClockBtn(318, 190, "+");  // min +
    uiTextFit(UIF_SMALL, 166, 270, T(S_HOUR), UI_TRACK_TEXT, 1, 120);
    uiTextFit(UIF_SMALL, 314, 270, T(S_MIN), UI_TRACK_TEXT, 1, 120);
  } else if (settingsPage == SET_VOLUME) {
    settingsTitle(T(S_SET_VOLUME));
    // the switch is the master; the level below is how loud it is when on,
    // and 0 is silent without turning the system off
    bool snd = audioEnabled();
    const char *sl = snd ? T(S_SND_ON) : T(S_SND_OFF);
    gfx->fillRoundRect(SND_SW_X, SND_SW_Y, SND_SW_W, SND_SW_H, 12, snd ? UI_BAR_OK : UI_WHITE);
    gfx->drawRoundRect(SND_SW_X, SND_SW_Y, SND_SW_W, SND_SW_H, 12, UI_INK);
    uiTextFit(UIF_MID, SND_SW_X + SND_SW_W / 2, uiMidY(UIF_MID, SND_SW_Y, SND_SW_H), sl, snd ? UI_BG_DAY : UI_INK, 1, SND_SW_W - 12);

    // TEST needs the sound on; at volume 0 it is allowed and simply silent
    gfx->fillRoundRect(SND_TEST_X, SND_SW_Y, SND_SW_W, SND_SW_H, 12, snd ? UI_WHITE : UI_TRACK_TEXT);
    gfx->drawRoundRect(SND_TEST_X, SND_SW_Y, SND_SW_W, SND_SW_H, 12, UI_INK);
    uiTextFit(UIF_MID, SND_TEST_X + SND_SW_W / 2, uiMidY(UIF_MID, SND_SW_Y, SND_SW_H), T(S_TEST), snd ? UI_INK : 0x8410, 1, SND_SW_W - 12);

    uint8_t v = audioVolume();
    for (int i = 0; i < 2; i++) {
      int bx = i ? VOLS_PLUS_X : VOLS_MINUS_X;
      bool live = i ? (v < 100) : (v > 0);
      gfx->fillRoundRect(bx, VOL_ROW_Y, VOL_BTN_W, VOL_BTN_H, 12, live ? UI_WHITE : UI_TRACK_TEXT);
      gfx->drawRoundRect(bx, VOL_ROW_Y, VOL_BTN_W, VOL_BTN_H, 12, UI_INK);
      uiText(UIF_BIG, bx + VOL_BTN_W / 2, VOL_ROW_Y + 44, i ? "+" : "-", live ? UI_INK : 0x8410, 1);
    }
    char vl[16];
    snprintf(vl, sizeof(vl), T(S_VOL_FMT), v);
    uiTextFit(UIF_SMALL, VOLS_BAR_X + VOLS_BAR_W / 2, VOL_ROW_Y + 20, vl, v ? UI_INK : UI_TRACK_TEXT, 1, VOLS_BAR_W);
    drawSlider(v, 0, 100);
  } else if (settingsPage == SET_BRIGHT) {
    settingsTitle(T(S_SET_BRIGHT));
    for (int i = 0; i < 2; i++) {
      int bx = i ? VOLS_PLUS_X : VOLS_MINUS_X;
      bool live = i ? (gBright < 10) : (gBright > 1);
      gfx->fillRoundRect(bx, BRI_ROW_Y, VOL_BTN_W, VOL_BTN_H, 12, live ? UI_WHITE : UI_TRACK_TEXT);
      gfx->drawRoundRect(bx, BRI_ROW_Y, VOL_BTN_W, VOL_BTN_H, 12, UI_INK);
      uiText(UIF_BIG, bx + VOL_BTN_W / 2, BRI_ROW_Y + 44, i ? "+" : "-", live ? UI_INK : 0x8410, 1);
    }
    char bl[8];
    snprintf(bl, sizeof(bl), "%u/10", gBright);
    uiText(UIF_BIG, CX, 158, bl, UI_INK, 1);
    drawSlider(gBright, 1, 10);
  } else if (settingsPage == SET_LANG) {
    settingsTitle(T(S_SET_LANG));
    const char *ln[LANG_COUNT]; int lwid[LANG_COUNT];
    for (int i = 0; i < LANG_COUNT; i++) { ln[i] = LANG_NAMES[LANG_ORDER[i]]; lwid[i] = LANG_CELL_W - 12; }
    const UiFont &langFont = uiFitAll(UIF_SMALL, ln, lwid, LANG_COUNT);   // every language name at one size
    for (int i = 0; i < LANG_COUNT; i++) {
      int x, y;
      langCell(i, x, y);
      Lang lg = LANG_ORDER[i];
      bool on = (lg == gLang);
      gfx->fillRoundRect(x, y, LANG_CELL_W, LANG_CELL_H, 12, on ? UI_BAR_OK : UI_WHITE);
      gfx->drawRoundRect(x, y, LANG_CELL_W, LANG_CELL_H, 12, UI_INK);
      uiText(langFont, x + LANG_CELL_W / 2, y + LANG_CELL_H / 2 + 7, LANG_NAMES[lg], on ? UI_BG_DAY : UI_INK, 1);
    }
  } else {
    settingsTitle(T(S_ABOUT));
    drawLogo(217);
    if (pet.ownerName[0]) {
      char own[24];
      snprintf(own, sizeof(own), "%s's", pet.ownerName);
      // just above the logo
      uiTextFit(UIF_BIG, CX, 217 - LOGO_H / 2 - 24 - 10 + 21, own, UI_INK, 1, 300);
    }
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", FW_VERSION);
    uiText(UIF_BIG, CX, 290, ver, UI_INK, 1);
  }

  // page dots, like the player card's
  for (uint8_t i = 0; i < SET_PAGES; i++) {
    int dx = CX - (SET_PAGES - 1) * 13 + i * 26;
    if (i == settingsPage) gfx->fillCircle(dx, SET_DOTS_Y, 5, UI_INK);
    else gfx->drawCircle(dx, SET_DOTS_Y, 4, UI_INK);
  }

  gfx->fillRoundRect(SET_OK_X, SET_OK_Y, SET_OK_W, SET_OK_H, 14, UI_BAR_OK);
  uiText(UIF_BIG, CX, SET_OK_Y + 34, "OK", UI_BG_DAY, 1);

  uiTextFit(UIF_SMALL, CX, 434, T(S_CLOCK_CANCEL), UI_TRACK_TEXT, 1, 220);
  gfx->flush();
}

// The volume bar is a slider: touching it sets the level from x, and dragging
// follows the finger. handleTouch keeps a gesture that began on it away from the
// swipe/tap resolver, or a horizontal drag would page the settings screen.
bool clockSliderHit(int16_t x, int16_t y) {
  return clockOpen && (settingsPage == SET_VOLUME || settingsPage == SET_BRIGHT) &&
         y >= VOL_ROW_Y + 20 && y < VOL_ROW_Y + VOL_BTN_H &&
         x >= VOLS_BAR_X - 16 && x <= VOLS_BAR_X + VOLS_BAR_W + 16;
}

void saveBright() {
  Preferences bp;
  bp.begin("tamapoke", false);
  bp.putUChar("bri", gBright);
  bp.end();
}

void clockSliderSet(int16_t x) {
  if (settingsPage == SET_BRIGHT) gBright = (uint8_t)sliderValue(x, 1, 10);   // applied live
  else audioSetVolume((uint8_t)sliderValue(x, 0, 100));
}

void clockSliderEnd() {   // NVS is written once, on release
  if (settingsPage == SET_BRIGHT) saveBright();
  else audioSaveVolume();
}

void clockTap(int16_t x, int16_t y) {
  // OK is on every page; only the time page has something to apply
  if (y >= SET_OK_Y && y <= SET_OK_Y + SET_OK_H && x >= SET_OK_X && x <= SET_OK_X + SET_OK_W) {
    if (settingsPage == SET_TIME) applyClock();
    else clockOpen = false;
    return;
  }
  if (settingsPage == SET_TIME) {
    if (y >= 190 && y <= 248) {  // fila de botones +/-
      if (x >= 104 && x < 162) clockH = (clockH + 23) % 24;
      else if (x >= 170 && x < 228) clockH = (clockH + 1) % 24;
      else if (x >= 252 && x < 310) clockM = (clockM + 59) % 60;
      else if (x >= 318 && x < 376) clockM = (clockM + 1) % 60;
    }
  } else if (settingsPage == SET_VOLUME) {
    if (y >= SND_SW_Y && y < SND_SW_Y + SND_SW_H && x >= SND_SW_X && x < SND_SW_X + SND_SW_W) {
      audioSetEnabled(!audioEnabled());
      if (audioEnabled()) sfxPlay(SFX_TAP);    // confirma al encender
    } else if (y >= SND_SW_Y && y < SND_SW_Y + SND_SW_H && x >= SND_TEST_X && x < SND_TEST_X + SND_SW_W) {
      if (audioEnabled()) sfxPlay(SFX_HATCH);  // the same cue as the serial BEEP
    } else if (y >= VOL_ROW_Y && y < VOL_ROW_Y + VOL_BTN_H) {
      int nv = audioVolume();
      if (x >= VOLS_MINUS_X && x < VOLS_MINUS_X + VOL_BTN_W) nv -= VOL_STEP;
      else if (x >= VOLS_PLUS_X && x < VOLS_PLUS_X + VOL_BTN_W) nv += VOL_STEP;
      else return;
      if (nv < 0) nv = 0;
      if (nv > 100) nv = 100;
      audioSetVolume((uint8_t)nv);
      audioSaveVolume();
      sfxPlay(SFX_TAP);                        // so the new level is audible
    }
  } else if (settingsPage == SET_BRIGHT) {
    if (y >= BRI_ROW_Y && y < BRI_ROW_Y + VOL_BTN_H) {
      int nb = gBright;
      if (x >= VOLS_MINUS_X && x < VOLS_MINUS_X + VOL_BTN_W) nb--;
      else if (x >= VOLS_PLUS_X && x < VOLS_PLUS_X + VOL_BTN_W) nb++;
      if (nb < 1) nb = 1;
      if (nb > 10) nb = 10;
      if (nb != gBright) {
        gBright = (uint8_t)nb;
        saveBright();
        sfxPlay(SFX_TAP);
      }
    }
  } else if (settingsPage == SET_LANG) {
    for (int i = 0; i < LANG_COUNT; i++) {
      int cx, cy;
      langCell(i, cx, cy);
      if (x >= cx && x < cx + LANG_CELL_W && y >= cy && y < cy + LANG_CELL_H) {
        setLang(LANG_ORDER[i]);
        sfxPlay(SFX_TAP);
        return;
      }
    }
  }
}

// The band between radii r0 and r1, from a0 to a1 degrees above the panel's
// 9 o'clock point, as a strip of triangles. The panel is round, so a plate that
// follows its edge fits where a rectangle would clip.
static void fillArcBand(int r0, int r1, float a0, float a1, uint16_t col) {
  const float RAD = 3.14159265f / 180.0f, STEP = 2.0f;
  for (float a = a0; a < a1; a += STEP) {
    float b = (a + STEP < a1) ? a + STEP : a1;
    float ca = cosf(a * RAD), sa = sinf(a * RAD), cb = cosf(b * RAD), sb = sinf(b * RAD);
    int ox0 = CX - (int)(r1 * ca), oy0 = CY - (int)(r1 * sa);
    int ix0 = CX - (int)(r0 * ca), iy0 = CY - (int)(r0 * sa);
    int ox1 = CX - (int)(r1 * cb), oy1 = CY - (int)(r1 * sb);
    int ix1 = CX - (int)(r0 * cb), iy1 = CY - (int)(r0 * sb);
    gfx->fillTriangle(ox0, oy0, ix0, iy0, ox1, oy1, col);
    gfx->fillTriangle(ix0, iy0, ix1, iy1, ox1, oy1, col);
  }
}

// One shoe print: a rounded sole and a separate heel, x/y is the centre.
static void drawFootprint(int x, int y, uint16_t col) {
  gfx->fillRoundRect(x - 4, y - 8, 8, 10, 3, col);
  gfx->fillRoundRect(x - 3, y + 4, 6, 4, 2, col);
}

// Today's steps, on a quarter-ring plate hugging the left edge above the party
// chevron. The number is the point; the arc along the rim fills toward
// STEP_GOAL and turns gold when it is met. No label text: a footprint says
// "steps" in every language without a row in the string table.
void drawStepPlate() {
  const float A0 = 6.0f, A1 = 42.0f;     // degrees above 9 o'clock, bottom to top
  const int R_IN = 166, R_OUT = 234;
  uint32_t steps = pet.stepsToday();
  uint16_t ink = inkColor();
  fillArcBand(R_IN, R_OUT, A0, A1, gNight ? C565(0x1c, 0x26, 0x44) : UI_WHITE);

  // outline: the inner arc and the two radial edges (the outer one is the panel's own)
  const float RAD = 3.14159265f / 180.0f;
  int px = 0, py = 0;
  for (float a = A0; a <= A1 + 0.1f; a += 2.0f) {
    int x = CX - (int)(R_IN * cosf(a * RAD)), y = CY - (int)(R_IN * sinf(a * RAD));
    if (a > A0) gfx->drawLine(px, py, x, y, ink);
    px = x; py = y;
  }
  const float edges[2] = {A0, A1};
  for (int i = 0; i < 2; i++) {
    float c = cosf(edges[i] * RAD), sn = sinf(edges[i] * RAD);
    gfx->drawLine(CX - (int)(R_IN * c), CY - (int)(R_IN * sn),
                  CX - (int)(R_OUT * c), CY - (int)(R_OUT * sn), ink);
  }

  // progress along the rim
  const float P0 = A0 + 4.0f, P1 = A1 - 4.0f;
  fillArcBand(224, 232, P0, P1, UI_TRACK_TEXT);
  uint32_t capped = steps > STEP_GOAL ? STEP_GOAL : steps;
  float pe = P0 + (P1 - P0) * capped / STEP_GOAL;
  if (pe > P0 + 0.5f) fillArcBand(224, 232, P0, pe, steps >= STEP_GOAL ? UI_BAR_WARN : UI_BAR_OK);

  // footprints at the top of the band, then the digits stacked down it, most
  // significant first. Each digit sits where the text radius meets its row, so the
  // column bends with the panel's edge and costs ~40 px of width instead of the
  // 60 a flat row of five needs. They stay upright: the font cannot rotate.
  drawFootprint(66, 100, ink);
  drawFootprint(78, 106, ink);
  char s[8];
  int n = snprintf(s, sizeof(s), "%lu", (unsigned long)steps);
  const int PITCH = 17, R_TXT = 194;
  for (int i = 0; i < n; i++) {
    int cy = 162 + (int)((i - (n - 1) / 2.0f) * PITCH);
    int dy = CY - cy;
    int cx = CX - (int)sqrtf((float)(R_TXT * R_TXT - dy * dy));
    char c[2] = {s[i], 0};
    uiText(UIF_SMALL, cx, cy - 8 + 14, c, ink, 1);   // centred on the column
  }
}

// flame + streak number at the top left
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  uiText(UIF_SMALL, x + 22, y + 2 + 14, s, inkColor(), 0);
}

// temporary banner: new medal or streak milestone
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  uiTextFit(UIF_BIG, CX, 176 + 21, l1, UI_INK, 1, 296);
  if (l2) uiTextFit(UIF_SMALL, CX, 212 + 14, l2, UI_INK, 1, 296);
}

// medals on the card: badge with label, coloured if earned
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK_TEXT);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK_TEXT);
  uiTextFit(UIF_SMALL, x + 50, y + 18, medalLabel(i), got ? UI_BG_DAY : 0x9492, 1, 92);
}

// page 0: profile (big portrait, identity, streak, bond, berry)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : d.name;
  char head[26];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  // long names do not fit the narrow strip at the top of the round panel, so
  // uiTextFit steps down to the small font
  uiTextFit(UIF_BIG, CX, 56, head, d.accent, 1, 230);
  if (pet.nick[0]) {  // real species under the nickname
    char sp[32];
    snprintf(sp, sizeof(sp), "(%s)", d.name);
    uiText(UIF_SMALL, CX, 82, sp, UI_TRACK_TEXT, 1);
  }

  // large animated portrait
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // streak with flame
  int sx = 138, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[30];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  uiText(UIF_SMALL, sx + 24, sy + 18, rl, UI_INK, 0);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e), IV_NONE);

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
  uiTextFit(UIF_SMALL, CX, 312, info, UI_INK, 1, 300);

  uiTextFit(UIF_SMALL, CX, 348, T(S_RENAME_HINT), UI_TRACK_TEXT, 1, 260);
}

// Antialiased text in the rounded UI font (uifont.h, ASCII only). Each pixel is
// blended against what is already on the canvas, so it sits on any background.
// `align`: 0 = x is the left edge, 1 = centred on x, 2 = right edge. `y` is the
// BASELINE. zh/ko and anything outside ASCII must keep using the 5x7 path.
static int uiFallbackSize(const UiFont &f) {
  return (&f == &UIF_HUGE) ? 6 : (&f == &UIF_BIG || &f == &UIF_MID) ? 3 : (&f == &UIF_TINY) ? 1 : 2;
}

int uiTextWidth(const UiFont &f, const char *s) {
  for (const char *c = s; *c; c++)       // zh/ko run on the 5x7 + CJK path
    if ((uint8_t)*c >= 127) return (int)cjkCols(s) * 6 * uiFallbackSize(f);
  int w = 0;
  for (; *s; s++)
    if (*s >= 32 && *s < 127) w += f.g[*s - 32].adv;
  return w;
}

void uiText(const UiFont &f, int x, int y, const char *s, uint16_t color, int align) {
  for (const char *c = s; *c; c++)
    if ((uint8_t)*c >= 127) {          // zh/ko: stay on the 5x7 + CJK path
      int sz = uiFallbackSize(f);
      int cw = (int)cjkCols(s) * 6 * sz;
      gfx->setTextColor(color);
      gfx->setTextSize(sz);
      gfx->setCursor(align == 1 ? x - cw / 2 : align == 2 ? x - cw : x, y - 7 * sz);
      gfx->print(s);
      return;
    }
  int w = uiTextWidth(f, s);
  if (align == 1) x -= w / 2;
  else if (align == 2) x -= w;
  const uint16_t *fb = gfx->getFramebuffer();
  const int fr = color >> 11, fg = (color >> 5) & 63, fbl = color & 31;
  for (; *s; s++) {
    if (*s < 32 || *s >= 127) continue;
    const UiGlyph &g = f.g[*s - 32];
    for (int j = 0; j < g.h; j++) {
      for (int i = 0; i < g.w; i++) {
        int n = j * g.w + i;
        uint8_t a = (f.bits[g.off + (n >> 1)] >> ((n & 1) ? 0 : 4)) & 15;
        if (!a) continue;
        int px = x + g.xo + i, py = y + g.yo + j;
        if (px < 0 || py < 0 || px >= LCD_WIDTH || py >= LCD_HEIGHT) continue;
        uint16_t bg = fb[(size_t)py * LCD_WIDTH + px];
        uint16_t c = bg;
        if (a == 15) c = color;
        else {
          int r = (bg >> 11) + (fr - (bg >> 11)) * a / 15;
          int gg = ((bg >> 5) & 63) + (fg - ((bg >> 5) & 63)) * a / 15;
          int b = (bg & 31) + (fbl - (bg & 31)) * a / 15;
          c = (uint16_t)(r << 11 | gg << 5 | b);
        }
        gfx->drawPixel(px, py, c);
      }
    }
    x += g.adv;
  }
}

// Baseline that vertically centres a cap-height line of font f in a box of
// height h whose top is y (rows, buttons).
int uiMidY(const UiFont &f, int y, int h) { return y + (h + f.ascent * 7 / 10) / 2; }

// Same as uiText, but steps down to UIF_SMALL when the string would be wider
// than maxW px (long nicknames, translated labels). Returns the width drawn.
int uiTextFit(const UiFont &f, int x, int y, const char *s, uint16_t color, int align, int maxW) {
  const UiFont *use = &f;                    // step down: given font, SMALL, TINY
  if (uiTextWidth(*use, s) > maxW) use = &UIF_SMALL;
  if (uiTextWidth(*use, s) > maxW) use = &UIF_TINY;
  uiText(*use, x, y, s, color, align);
  return uiTextWidth(*use, s);
}

// The largest of f / UIF_SMALL / UIF_TINY at which EVERY one of n strings fits its
// own width. Options in one menu must share a size: uiTextFit() shrinks each string
// on its own, so a single long name used to render smaller than its neighbours.
const UiFont &uiFitAll(const UiFont &f, const char *const *s, const int *maxW, int n) {
  const UiFont *c[3] = { &f, &UIF_SMALL, &UIF_TINY };
  for (int k = 0; k < 3; k++) {
    bool ok = true;
    for (int i = 0; i < n && ok; i++)
      if (uiTextWidth(*c[k], s[i]) > maxW[i]) ok = false;
    if (ok) return *c[k];
  }
  return UIF_TINY;
}

// page 1: combat (4 bars + train button)
void renderCardStats() {
  uiText(UIF_BIG, CX, 86, T(S_BATTLE), UI_INK, 1);

  // typing, in the accent colour of the species (English in every language,
  // same as the species names themselves)
  const DexEntry &de = DEX_TBL[pet.speciesId];
  char ty[24];
  if (de.type2 == T_NONE) snprintf(ty, sizeof(ty), "%s", typeName(de.type1));
  else snprintf(ty, sizeof(ty), "%s/%s", typeName(de.type1), typeName(de.type2));
  uiText(UIF_SMALL, CX, 124, ty, de.accent, 1);

  // Hexagon radar of all six combat stats, HP at the top and clockwise from
  // there like the games' summary screen. 300 is the scale cap: a typical
  // creature sits at 100-250, and a stat above it just touches the rim.
  struct Spoke { const char *label; uint16_t val; uint8_t iv; };
  const Spoke sp[6] = {
    {T(S_STAT_VIT), pet.vitStat(), pet.ivHp},
    {T(S_STAT_ATK), pet.atkStat(), pet.ivAtk},
    {T(S_STAT_DEF), pet.defStat(), pet.ivDef},
    {T(S_STAT_SPE), pet.speStat(), pet.ivSpe},
    {"S.DEF",       pet.spdStat(), pet.ivDef},   // special reuses the physical IV
    {"S.ATK",       pet.spaStat(), pet.ivAtk},
  };
  const int hx = CX, hy = 250, hr = 66;
  float ca[6], sa[6];
  for (int i = 0; i < 6; i++) {
    float a = -1.5708f + i * 1.0472f;
    ca[i] = cosf(a); sa[i] = sinf(a);
  }
  int px[6], py[6];
  for (int i = 0; i < 6; i++) {
    float f = sp[i].val / 300.0f;
    if (f > 1.0f) f = 1.0f;
    if (f < 0.15f) f = 0.15f;                           // keep a tiny stat visible
    px[i] = hx + (int)(ca[i] * hr * f);
    py[i] = hy + (int)(sa[i] * hr * f);
  }
  // tint = the accent half-mixed into the page colour, so the shape reads as
  // see-through (RGB565 has no alpha, so blend per channel)
  const uint16_t acc = DEX_TBL[pet.speciesId].accent;
  const uint16_t pg = gNight ? UI_BG_NIGHT : UI_BG_DAY;
  const uint16_t fillc = (uint16_t)((((acc >> 11) + (pg >> 11)) / 2) << 11 |
                                    ((((acc >> 5) & 63) + ((pg >> 5) & 63)) / 2) << 5 |
                                    (((acc & 31) + (pg & 31)) / 2));
  for (int i = 0; i < 6; i++) {
    int j = (i + 1) % 6;
    gfx->fillTriangle(hx, hy, px[i], py[i], px[j], py[j], fillc);
  }
  // grid goes on TOP of the fill, so the web shows through it
  for (int ring = 1; ring <= 3; ring++) {            // grid rings at 1/3, 2/3, full
    int r = hr * ring / 3;
    for (int i = 0; i < 6; i++) {
      int j = (i + 1) % 6;
      gfx->drawLine(hx + (int)(ca[i] * r), hy + (int)(sa[i] * r),
                    hx + (int)(ca[j] * r), hy + (int)(sa[j] * r), UI_TRACK_TEXT);
    }
  }
  for (int i = 0; i < 6; i++)
    gfx->drawLine(hx, hy, hx + (int)(ca[i] * hr), hy + (int)(sa[i] * hr), UI_TRACK_TEXT);
  for (int i = 0; i < 6; i++) {
    int j = (i + 1) % 6;
    gfx->drawLine(px[i], py[i], px[j], py[j], acc);
  }
  // label over value at each vertex; a perfect IV colours the value
  for (int i = 0; i < 6; i++) {
    char num[8];
    snprintf(num, sizeof(num), "%u", sp[i].val);
    bool vert = (i == 0 || i == 3);                   // top and bottom sit closer in
    int cx = hx + (int)(ca[i] * (hr + 38)), cy = hy + (int)(sa[i] * (hr + (vert ? 26 : 18)));
    uiText(UIF_SMALL, cx, cy - 2, sp[i].label, UI_INK, 1);
    uiText(UIF_SMALL, cx, cy + 20, num, sp[i].iv >= 31 ? UI_BAR_WARN : UI_TRACK_TEXT, 1);
  }
}

// Draws one move as a row: name, its type in the type's own colour, and either
// power or a STATUS marker. Shared by the moves page and the picker so a move
// looks the same wherever you meet it.
// A filled chip in the type's own colour, label in whichever of black/white
// reads on it. Returns its width so a caller can lay out beside it.
// Larger chip (19 px high, 16 px label) for the move rows, where there is room.
int drawTypeChipBig(int x, int y, uint8_t type) {
  const char *nm = typeName(type);
  int w = uiTextWidth(UIF_TINY, nm) + 14;
  gfx->fillRoundRect(x, y, w, 19, 5, typeColor(type));
  uiText(UIF_TINY, x + 7, y + 15, nm, typeColorIsLight(type) ? UI_INK : UI_WHITE, 0);
  return w;
}

int drawTypeChip(int x, int y, uint8_t type) {
  const char *nm = typeName(type);
  // same 19 px chip with a 16 px label as the big one
  return drawTypeChipBig(x, y, type);
}

void drawMoveRow(int y, uint8_t mv, bool highlight, int16_t dex) {
  gfx->fillRoundRect(70, y, 326, 50, 12, highlight ? UI_BAR_WARN : UI_BG_DAY);
  gfx->drawRoundRect(70, y, 326, 50, 12, UI_INK);
  if (!mv) {
    uiTextFit(UIF_SMALL, CX, y + 31, T(S_MOVE_EMPTY), UI_TRACK_TEXT, 1, 290);
    return;
  }
  const MoveEntry &m = MOVE_TBL[mv];
  uiTextFit(UIF_SMALL, 82, y + 24, m.name, UI_INK, 0, 300);
  // There is no per-type palette (DexEntry.accent is per species), and inventing
  // one by hand would duplicate what gen_dex.py generates. Colouring same-type
  // moves in the species accent is more useful anyway: STAB is a 1.5x damage
  // bonus, so this marks the moves that actually hit hardest for this creature.
  // The chip carries the TYPE; STAB moved onto the power figure, where it
  // belongs -- STAB is a damage bonus, so saying it next to the damage reads
  // straight, and it leaves the type free to be its own colour.
  bool stab = hasStab(dex, m.type) && m.cat != MC_STATUS;
  int cw = drawTypeChipBig(82, y + 28, m.type);
  if (stab) {
    uiText(UIF_TINY, 82 + cw + 8, y + 43, "STAB", DEX_TBL[dex].accent, 0);
  }
  char pw[16];
  if (m.cat == MC_STATUS) snprintf(pw, sizeof(pw), "%s", T(S_MOVE_STATUS));
  else snprintf(pw, sizeof(pw), T(S_MOVE_PWR), m.power);
  uiText(UIF_TINY, 384, y + 43, pw, stab ? DEX_TBL[dex].accent : UI_INK, 2);
}

// card page 4: the four known moves. Tapping a slot opens the picker.
void renderCardMoves() {
  uiTextFit(UIF_BIG, CX, 66, T(S_MOVES), UI_INK, 1, 220);
  for (int i = 0; i < MOVE_SLOTS; i++) drawMoveRow(MOVE_ROW_Y(i), pet.moves[i], false, pet.speciesId);
  uiTextFit(UIF_SMALL, CX, 354, T(S_MOVE_TAP), UI_TRACK_TEXT, 1, 340);
}

// Every move the species can learn by this level, so a slot can be swapped for
// anything legal -- not just the handful a level-up would have offered.
uint8_t learnableFor(int16_t dex, uint8_t lvl, uint8_t *out, uint8_t max) {
  if (dex < 1 || dex > DEX_COUNT) return 0;
  uint8_t n = learnCount(dex), w = 0;
  for (uint8_t i = 0; i < n && w < max; i++) {
    // moveUnlockLevel(), NOT learnLevel(): a TM is stored as level 0 and would
    // otherwise clear this check at level 1. That is how a level 22 Charmeleon
    // came to be offered FIRE BLAST -- the same class of bug as the level 1
    // Squirtle with SURF, in the one path that fix did not reach.
    if (moveUnlockLevel(dex, i) > lvl) continue;
    uint8_t mv = learnMove(dex, i);
    if (!mv || mv >= MOVE_COUNT) continue;
    bool dup = false;
    for (uint8_t j = 0; j < w; j++)
      if (out[j] == mv) { dup = true; break; }
    if (!dup) out[w++] = mv;
  }
  return w;
}

// The picker targets either the live pet or a banked member. A banked one keeps
// its frozen level, so it can only relearn what it could have known back then.
uint8_t learnableList(uint8_t *out, uint8_t max) {
  if (movePickParty) {
    const PartyMon &m = party.slots[movePickParty - 1];
    return learnableFor(m.dex, (uint8_t)m.level, out, max);
  }
  return pet.isEgg() ? 0 : learnableFor(pet.speciesId, pet.level(), out, max);
}

uint8_t *pickTargetMoves() {
  return movePickParty ? party.slots[movePickParty - 1].moves : pet.moves;
}
int16_t pickTargetDex() {
  return movePickParty ? party.slots[movePickParty - 1].dex : pet.speciesId;
}

void renderMovePick() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uiTextFit(UIF_SMALL, CX, 56, T(S_MOVE_PICK), UI_INK, 1, 230);

  uint8_t all[64];
  uint8_t n = learnableList(all, sizeof(all));
  uint8_t pages = n ? (n + MOVE_PICK_PER_PAGE - 1) / MOVE_PICK_PER_PAGE : 1;
  if (movePickPage >= pages) movePickPage = 0;
  for (uint8_t i = 0; i < MOVE_PICK_PER_PAGE; i++) {
    uint8_t idx = movePickPage * MOVE_PICK_PER_PAGE + i;
    if (idx >= n) break;
    // the move already in this slot is highlighted, so replacing like for like
    // is obvious rather than a guess
    drawMoveRow(MOVE_PICK_Y(i), all[idx], all[idx] == pickTargetMoves()[movePickSlot], pickTargetDex());
  }
  for (uint8_t i = 0; i < pages && pages > 1; i++) {
    if (i == movePickPage) gfx->fillCircle(CX - (pages - 1) * 13 + i * 26, 380, 5, UI_INK);
    else gfx->drawCircle(CX - (pages - 1) * 13 + i * 26, 380, 4, UI_INK);
  }
  uiText(UIF_SMALL, CX, 416, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

// ---------- battle ----------

// Draws a battle backdrop scaled 2x. Emitted as runs of identical indices
// rather than a write per pixel: this is flat pixel art with long horizontal
// runs, and 240x112 at 2x would otherwise be 26,880 fillRect calls a frame.
// The round bezel crops the overhang physically, so nothing is clipped here.
static void drawBack(const BackScene &b, int y0) {
  const int SC = 2;
  int x0 = CX - (b.w * SC) / 2;
  for (int r = 0; r < b.h; r++) {
    const uint8_t *row = b.idx + (uint32_t)r * b.w;
    int c = 0;
    while (c < b.w) {
      uint8_t v = row[c];
      int run = 1;
      while (c + run < b.w && row[c + run] == v) run++;
      uint16_t col = b.pal[v];
      gfx->fillRect(x0 + c * SC, y0 + r * SC, run * SC, SC, col);
      c += run;
    }
  }
}

// Which scene: the FOE's biome, since a battle happens where it lives, and the
// same day/night split the main screen already uses.
static void drawBattleBack() {
  int16_t dex = btlFoe.dex;
  if (dex < 1 || dex > DEX_COUNT) { gfx->fillCircle(CX, CY, 231, UI_BG_DAY); return; }
  uint8_t bi = DEX_TBL[dex].biome;
  if (bi >= BACK_BIOMES) bi = (bi == BIOME_GRAVEYARD) ? BIOME_FOREST : BIOME_MEADOW;   // no graveyard battle art yet: ghosts fight in the (dark) forest
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  drawBack(BACKS[bi][night ? 1 : 0], 30);
}

// Streams a side's sprite if it is not already the one loaded. Called whenever
// a creature steps in, never per frame. That is also when it cries, so the cry
// follows the sprite swap: a switch to the SAME species stays silent.
static void btlSyncSprite(uint8_t who, const Combatant &c) {
  int16_t key = c.dex * (c.shiny ? -1 : 1);
  if (btlPmdDex[who] == key && btlPmd[who].loaded) return;
  btlPmd[who].unload();
  btlPmdDex[who] = 0;
  if (c.dex < 1 || c.dex > DEX_COUNT) return;
  audioCry(c.dex);   // a creature just came out: it calls (needs no sprite, so before the load)
  if (btlPmd[who].load(c.dex, c.shiny)) btlPmdDex[who] = key;   // NOT (uint8_t): Hoenn runs past 255
}

static void btlFreeSprites() {
  for (int i = 0; i < 2; i++) { btlPmd[i].unload(); btlPmdDex[i] = 0; }
}

static void btlSay(const char *fmt, ...) {
  if (btlMsgCount >= 6) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(btlMsg[btlMsgCount], sizeof(btlMsg[0]), fmt, ap);
  va_end(ap);
  btlMsgCount++;
}

// Turns a TurnLog into narration. Everything here was already decided by the
// engine -- nothing is recomputed, so the text can never disagree with the maths.
// Picks the cue for an action from the TurnLog, so the sound can never
// disagree with what actually happened.
static void btlSfxFor(const TurnLog &lg) {
  if (lg.targetFainted) { sfxPlay(SFX_FAINT); return; }
  if (lg.inflicted) { sfxPlay(SFX_STATUS); return; }
  if (lg.damage && lg.effPct > 100) { sfxPlay(SFX_SUPER); return; }
  if (lg.damage) {
    sfxPlay(lg.move && MOVE_TBL[lg.move].cat == MC_SPEC ? SFX_BEAM : SFX_HIT);
    return;
  }
  if (lg.move && MOVE_TBL[lg.move].cat == MC_STATUS && !lg.missed) sfxPlay(SFX_STATUS);
}

static void btlNarrate(const Combatant &actor, const Combatant &target, const TurnLog &lg) {
  if (lg.skipped) return;
  btlSfxFor(lg);
  if (lg.hurtSelf) { btlSay(T(S_BTL_HURTSELF)); return; }
  if (lg.charged) { btlSay(T(S_BTL_USED), actor.name, MOVE_TBL[lg.move].name); return; }
  if (lg.move) btlSay(T(S_BTL_USED), actor.name, MOVE_TBL[lg.move].name);
  if (lg.missed) { btlSay(T(S_BTL_MISS), actor.name); return; }
  if (lg.immune) { btlSay(T(S_BTL_IMMUNE)); return; }
  if (lg.crit) btlSay(T(S_BTL_CRIT));
  if (lg.damage && lg.effPct > 100) btlSay(T(S_BTL_SUPER));
  else if (lg.damage && lg.effPct < 100) btlSay(T(S_BTL_WEAK));
  if (lg.inflicted) {
    static const StrId AIL_STR[] = { S_AIL_PARA, S_AIL_PARA, S_AIL_BURN, S_AIL_POISON,
                                     S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE };
    if (lg.inflicted < 7) btlSay(T(S_BTL_STATUS), target.name, T(AIL_STR[lg.inflicted]));
  }
  if (lg.targetFainted) btlSay(T(S_BTL_FAINT), target.name);
}

// Builds one opponent through Pet, so it gets the same stat formula and the
// same learnset-driven moveset the player's creatures do.
static void foeFromSpecies(Combatant &c, int16_t dex, uint8_t lvl, uint8_t iv) {
  Pet foe;
  foe.dbgHatchAs(dex, false);
  foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = iv;
  foe.ageMinutes = (uint32_t)(lvl ? lvl - 1 : 0) * MINUTES_PER_LEVEL;
  foe.relearnFromLevel();
  combatantFromPet(c, foe);
}

// Your side: the live pet first, then the banked party.
//
// Both ladders cap your LEVEL to the leader's best, so a gym is always fought
// on its own terms and grinding is never the answer -- the type chart, the
// movesets and the choices are. Hard additionally caps your team SIZE to the
// leader's, so Brock is two-on-two. The caps are applied while BUILDING the
// combatants, so nothing is ever written back to the stored creature, exactly
// like ailments.
static void buildSquad(uint8_t maxLvl, uint8_t maxCount, uint16_t mask) {
  btlSquadN = 0;
  btlSquadAt = 0;
  btlPetIn = false;
  if (maxCount > TRAINER_TEAM_MAX) maxCount = TRAINER_TEAM_MAX;
  if (!pet.isEgg() && btlSquadN < maxCount && (mask & 1)) {
    Pet tmp = pet;                       // a copy: the real pet is untouched
    if (maxLvl && tmp.level() > maxLvl)
      tmp.ageMinutes = (uint32_t)(maxLvl - 1) * MINUTES_PER_LEVEL;
    combatantFromPet(btlSquad[btlSquadN++], tmp);
    btlPetIn = true;      // the training reward goes to whoever fought for it
  }
  for (int i = 0; i < PARTY_SLOTS && btlSquadN < maxCount; i++) {
    if (party.slots[i].empty() || !(mask & (1 << (i + 1)))) continue;
    PartyMon m = party.slots[i];
    if (maxLvl && m.level > maxLvl) m.level = maxLvl;
    combatantFromParty(btlSquad[btlSquadN++], m);
  }
  if (btlSquadN) btlYou = btlSquad[0];
}

// How many you may bring: the leader's own count in hard mode, six otherwise.
uint8_t squadCap(uint8_t idx, bool hard) {
  if (idx >= TRAINER_COUNT) return TRAINER_TEAM_MAX;
  return hard ? TRAINERS[idx].count : TRAINER_TEAM_MAX;
}

// A fight against another device. The squads are already exchanged; the host
// owns resolution and the guest renders what it is sent.
void startLinkBattle() {
  if (!lan.mineN || !lan.theirsN) return;
  // Rebuilt from lan.mine, NOT from squadMask. What we fight with has to be
  // exactly what the peer was told we have -- rebuilding from the party would
  // silently diverge if anything changed between offering and starting.
  btlSquadN = 0;
  btlSquadAt = 0;
  for (uint8_t i = 0; i < lan.mineN && i < TRAINER_TEAM_MAX; i++)
    linkMonTo(btlSquad[btlSquadN++], lan.mine[i]);
  if (!btlSquadN) return;
  btlYou = btlSquad[0];
  btlLink = true;
  btlLinkHost = lan.isHost;
  btlTrainer = -1;
  btlHard = false;
  btlFoeAt = 0;
  btlFoeSquadN = 0;
  for (uint8_t i = 0; i < lan.theirsN && i < TRAINER_TEAM_MAX; i++)
    linkMonTo(btlFoeSquad[btlFoeSquadN++], lan.theirs[i]);
  btlFoe = btlFoeSquad[0];
  btlMyAct = 0;
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  battleOpen = true;
}

void startTrainerBattle(uint8_t idx, bool hard) {
  if (idx >= TRAINER_COUNT || pet.isEgg() || pet.ceremony != CER_NONE) return;
  btlRegion = gymRegion;   // latch the ladder: the fight, badge and next foe must all read this one
  const Trainer &tr = TRAINERS[idx];
  uint8_t top = 0;
  for (int k = 0; k < tr.count; k++)
    if (tr.team[k].level > top) top = tr.team[k].level;
  // BOTH ladders cap your level to the leader's best. Without it a L73 team
  // walks every trainer at 100% and the type chart never matters. Hard adds the
  // size cap on top, plus a smarter AI and better opposing IVs.
  buildSquad(top, hard ? tr.count : TRAINER_TEAM_MAX, squadMask);
  if (!btlSquadN) return;
  btlTrainer = (int8_t)idx;
  btlHard = hard;
  btlFoeAt = 0;
  const Trainer &t = TRAINERS[idx];
  foeFromSpecies(btlFoe, t.team[0].dex, t.team[0].level, hard ? HARD_IV : EASY_IV);
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true;
}

void startBattle(int16_t dex, uint8_t lvl) {
  if (pet.isEgg() || pet.ceremony != CER_NONE) return;
  if (dex < 1 || dex > DEX_COUNT) return;
  buildSquad(0, TRAINER_TEAM_MAX, 0xFFFF);
  if (!btlSquadN) return;
  // The opponent is built through Pet so it gets the same stat formula and the
  // same learnset-driven moveset the player's creature does -- no special-cased
  // "enemy" maths that could quietly diverge.
  Pet foe;
  foe.dbgHatchAs(dex, false);
  foe.ivAtk = foe.ivDef = foe.ivSpe = foe.ivHp = 20;
  foe.ageMinutes = (uint32_t)(lvl ? lvl - 1 : 0) * MINUTES_PER_LEVEL;
  foe.relearnFromLevel();
  combatantFromPet(btlFoe, foe);
  btlMsgCount = 0;
  btlOver = false;
  btlWon = false;
  btlMenu = 0;
  btlWinUntil = 0;
  btlSwapWho = -1;
  btlFaintUntil[0] = btlFaintUntil[1] = 0;
  btlEnterUntil[0] = btlEnterUntil[1] = 0;
  btlHpShown[0] = btlYou.maxHp;
  btlHpShown[1] = btlFoe.maxHp;
  btlSyncSprite(0, btlYou);
  btlSyncSprite(1, btlFoe);
  audioMusic(MUS_BATTLE);
  btlLungeUntil[0] = btlLungeUntil[1] = 0;
  btlHitUntil[0] = btlHitUntil[1] = 0;
  battleOpen = true;
}

// The guest's whole turn: copy in what the host resolved and play the same
// animations the host is playing. It runs no battle logic at all -- that is the
// entire point of one side being authoritative (see link.h).
static void btlApplyResult() {
  if (lan.resultN < sizeof(LinkResult)) { lan.resultNew = false; return; }
  LinkResult r;
  memcpy(&r, lan.result, sizeof(r));
  lan.resultNew = false;

  // The wire says "host"/"guest"; here we are always the guest, so their fields
  // are the foe's and ours are ours.
  uint32_t now = millis();
  if (r.guestIdx < btlSquadN && r.guestIdx != btlSquadAt) {
    btlSquad[btlSquadAt] = btlYou;
    btlSquadAt = r.guestIdx;
    btlYou = btlSquad[btlSquadAt];
    btlSyncSprite(0, btlYou);
    btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
    btlEnterUntil[0] = now + BTL_ENTER_MS;
  }
  if (r.hostIdx != btlFoeAt && r.hostIdx < lan.theirsN) {
    btlFoeAt = r.hostIdx;
    linkMonTo(btlFoe, lan.theirs[btlFoeAt]);
    btlHpShown[1] = btlFoe.maxHp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
  }
  btlYou.hp = r.guestHp > btlYou.maxHp ? btlYou.maxHp : r.guestHp;
  btlFoe.hp = r.hostHp > btlFoe.maxHp ? btlFoe.maxHp : r.hostHp;
  btlYou.ailment = r.guestAil;
  btlFoe.ailment = r.hostAil;

  btlMsgCount = 0;
  if (r.hostMove) btlSay(T(S_BTL_USED), btlFoe.name, MOVE_TBL[r.hostMove].name);
  if (r.guestMove) btlSay(T(S_BTL_USED), btlYou.name, MOVE_TBL[r.guestMove].name);
  if (r.guestDmg) { btlHitUntil[0] = now + BTL_HIT_MS; sfxPlay(SFX_HIT); }
  if (r.hostDmg) { btlHitUntil[1] = now + BTL_HIT_MS; sfxPlay(SFX_HIT); }
  if (btlYou.fainted()) {
    btlFaintUntil[0] = now + BTL_FAINT_MS;
    btlSay(T(S_BTL_FAINT), btlYou.name);
  }
  if (btlFoe.fainted()) {
    btlFaintUntil[1] = now + BTL_FAINT_MS;
    btlSay(T(S_BTL_FAINT), btlFoe.name);
  }
}

// Radio packets land on another task, so the guest picks them up here, once a
// frame, rather than rendering from inside an interrupt.
static void btlLinkPoll() {
  if (!btlLink) return;

  // A peer that stopped answering. Ending the fight is the only honest thing to
  // do -- there is no result coming, and pretending otherwise is the hang this
  // whole layer exists to remove.
  if (!lan.live() && !btlOver) {
    btlOver = true;
    btlWon = false;
    audioMusic(MUS_NONE);
    btlMsgCount = 0;
    btlSay("%s", T(S_LAN_GONE));
    return;
  }

  if (btlLinkHost) {
    // Our own action was latched when it was tapped; theirs arrives whenever
    // the radio manages it. Whichever is second sets the turn going.
    if (btlMyAct && lan.hasPeerAct() && !btlOver && !btlMsgCount &&
        btlSwapWho < 0) {
      uint8_t act = btlMyAct;
      btlMyAct = 0;
      if (LINK_ACT_IS_SWITCH(act)) btlSwitchTo(LINK_ACT_SLOT(act));
      else btlResolve(btlYou.moves[LINK_ACT_SLOT(act) % MOVE_SLOTS]);
    }
    return;
  }

  if (lan.resultNew) btlApplyResult();
  if (lan.state == LINK_DONE && !btlOver) {
    btlOver = true;
    btlWon = lan.youWon;
    audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
    if (btlWon) sfxPlay(SFX_VICTORY);
    btlSay("%s", btlWon ? T(S_BTL_WIN) : T(S_BTL_LOSE));
  }
}

// Packs the outcome for the guest. Only the host ever calls this.
static void btlShipResult(uint8_t yourMove, uint8_t theirMove,
                          uint16_t hp0You, uint16_t hp0Foe) {
  LinkResult r = {};
  r.hostHp = btlYou.hp;   r.guestHp = btlFoe.hp;
  r.hostAil = btlYou.ailment; r.guestAil = btlFoe.ailment;
  r.hostMove = yourMove;  r.guestMove = theirMove;
  r.hostDmg = (hp0You > btlYou.hp) ? hp0You - btlYou.hp : 0;
  r.guestDmg = (hp0Foe > btlFoe.hp) ? hp0Foe - btlFoe.hp : 0;
  r.hostIdx = btlSquadAt; r.guestIdx = btlFoeAt;
  if (btlYou.fainted() || btlFoe.fainted()) r.flags |= 0x04;
  lan.sendResult((const uint8_t *)&r, (uint8_t)sizeof(r));
}

// One exchange: both sides act in speed order, then burn/poison chip.
static void btlResolve(uint8_t yourMove) {
  TurnLog lg;
  // Against another device the opponent's move comes off the wire, never from
  // the AI -- and the host is the only side that runs this at all.
  uint8_t foeMove;
  bool foeSwitched = false;
  uint32_t now = millis();
  if (btlLink) {
    // Off the wire, never from the AI. A switch is carried in the same message
    // as a move, and like our own switch it costs the turn: they change, we act.
    uint8_t act = lan.pendingAct;
    if (LINK_ACT_IS_SWITCH(act)) {
      uint8_t to = LINK_ACT_SLOT(act);
      if (to < btlFoeSquadN && to != btlFoeAt && !btlFoeSquad[to].fainted()) {
        btlFoeSquad[btlFoeAt] = btlFoe;     // remember how battered it was
        btlFoeAt = to;
        btlFoe = btlFoeSquad[to];
        btlHpShown[1] = btlFoe.hp;
        btlSyncSprite(1, btlFoe);
        btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
        btlEnterUntil[1] = now + BTL_ENTER_MS;
        btlSay(T(S_BTL_SENDS), lan.peerName, btlFoe.name);
        foeSwitched = true;
      }
      foeMove = 0;
    } else {
      foeMove = btlFoe.moves[LINK_ACT_SLOT(act) % MOVE_SLOTS];
    }
    lan.pendingAct = 0;
  } else {
    foeMove = aiChooseMove(btlFoe, btlYou, btlHard);
  }
  (void)foeSwitched;

  bool youFirst = battleMovesFirst(btlYou, yourMove, btlFoe, foeMove);
  Combatant *a = youFirst ? &btlYou : &btlFoe;
  Combatant *b = youFirst ? &btlFoe : &btlYou;
  uint8_t ma = youFirst ? yourMove : foeMove;
  uint8_t mb = youFirst ? foeMove : yourMove;

  uint16_t hp0You = btlYou.hp, hp0Foe = btlFoe.hp;
  battleAct(*a, *b, ma, lg);
  btlNarrate(*a, *b, lg);
  if (lg.damage && !lg.hurtSelf) btlLungeUntil[a == &btlYou ? 0 : 1] = now + BTL_LUNGE_MS;
  if (!b->fainted()) {
    battleAct(*b, *a, mb, lg);
    btlNarrate(*b, *a, lg);
    if (lg.damage && !lg.hurtSelf)
      btlLungeUntil[b == &btlYou ? 0 : 1] = now + BTL_LUNGE_MS + BTL_LUNGE_MS;
  }
  // whoever actually lost health flinches, whichever side dealt it
  if (btlYou.hp < hp0You) btlHitUntil[0] = now + BTL_HIT_MS;
  if (btlFoe.hp < hp0Foe) btlHitUntil[1] = now + BTL_HIT_MS;
  if (btlLink && btlLinkHost) btlShipResult(yourMove, foeMove, hp0You, hp0Foe);
  if (!btlYou.fainted() && !btlFoe.fainted()) {
    battleEndTurn(btlYou, lg);
    if (lg.damage) btlNarrate(btlYou, btlYou, lg);
    battleEndTurn(btlFoe, lg);
    if (lg.damage) btlNarrate(btlFoe, btlFoe, lg);
  }
  // Someone went down. The replacement is NOT swapped in here -- that made the
  // change instant and read as a jump cut. Flag it, let the sprite drop out of
  // frame, and swap when the player dismisses the message.
  if (btlFoe.fainted() && btlLink && btlFoeAt + 1 < btlFoeSquadN) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlFoe.fainted() && btlTrainer >= 0 && btlFoeAt + 1 < BTL_TRAINERS[btlTrainer].count) {
    btlFaintUntil[1] = millis() + BTL_FAINT_MS;
    btlSwapWho = 1;
    return;
  }
  if (btlYou.fainted() && btlSquadAt + 1 < btlSquadN) {
    btlFaintUntil[0] = millis() + BTL_FAINT_MS;
    btlSwapWho = 0;
    return;
  }
  if (btlFoe.fainted() || btlYou.fainted()) {
    btlOver = true;
    btlWon = btlFoe.fainted();
    btlNewBadge = false;
    btlTrainGain = 0;
    if (btlWon && btlTrainer >= 0 && !pet.hasBadge(btlRegion, btlTrainer, btlHard)) {
      pet.winBadge(btlRegion, btlTrainer, btlHard);
      btlNewBadge = true;
    }
    // A badge and nothing else made the ladder a one-way checklist. A win now
    // trains the creature that fought for it -- so a leader you can already
    // beat is worth returning to. It goes to the LIVE pet only: banked members
    // are frozen at the level and training they were banked with, and battling
    // already costs the live pet energy, which is what rate-limits the grind
    // without needing a cooldown.
    if (btlWon && btlTrainer >= 0 && btlPetIn) {
      // Later leaders are worth more, and hard mode is worth roughly double.
      uint8_t amt = (btlHard ? 6 + random(5) : 3 + random(3)) + btlTrainer / 3;
      btlTrainGain = pet.rewardTraining(amt, btlTrainWhich);
    }
    audioMusic(btlWon ? MUS_VICTORY : MUS_NONE);
    if (btlWon) sfxPlay(SFX_VICTORY);
    // Tell the peer before anything else: if we stop here without sending, the
    // other device sits on a battle that will never take another turn.
    if (btlLink && btlLinkHost) lan.sendEnd(btlWon);
    if (btlLink) { btlSay("%s", btlWon ? T(S_BTL_WIN) : T(S_BTL_LOSE)); return; }
    if (btlWon && btlTrainer >= 0) { btlWinUntil = millis() + 60000; return; }
    btlSay("%s", T(S_BTL_LOSE));
  }
}

static void btlHpBar(int x, int y, int w, const Combatant &c, uint16_t shown) {
  int fw = c.maxHp ? (w - 4) * shown / c.maxHp : 0;
  uint16_t col = (shown * 2 > c.maxHp) ? UI_BAR_OK
                 : (shown * 4 > c.maxHp) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(x, y, w, 14, 4, UI_TRACK_TEXT);
  if (fw > 0) gfx->fillRoundRect(x + 2, y + 2, fw, 10, 3, col);
  gfx->drawRoundRect(x, y, w, 14, 4, UI_INK);
}

static void btlSide(int tx, int ty, int sx, int sy, const Combatant &c, uint8_t who) {
  // the scenes are busy, so the name and bar sit on their own plate rather
  // than fighting the artwork for contrast
  // Plate is 190 wide; rows: name+level, HP bar, then numbers/status (both
  // sides show numeric HP).
  int ph = 68;
  gfx->fillRoundRect(tx - 8, ty - 8, 190, ph, 8, UI_BG_DAY);
  gfx->drawRoundRect(tx - 8, ty - 8, 190, ph, 8, UI_INK);
  char l[28];
  snprintf(l, sizeof(l), "%s Lv.%u", c.name, c.level);
  uiTextFit(UIF_TINY, tx, ty + 12, l, UI_INK, 0, 174);
  uiText(UIF_TINY, tx, ty + 32, "HP", UI_BAR_WARN, 0);
  btlHpBar(tx + 26, ty + 20, 148, c, btlHpShown[who]);
  char hp[16];
  snprintf(hp, sizeof(hp), "%u/%u", btlHpShown[who], c.maxHp);
  uiText(UIF_TINY, tx + 174, ty + 52, hp, UI_INK, 2);
  if (c.ailment != AIL_NONE) {   // a status is the thing you most need to see
    static const StrId AIL_STR[] = { S_AIL_PARA, S_AIL_PARA, S_AIL_BURN, S_AIL_POISON,
                                     S_AIL_SLEEP, S_AIL_FREEZE, S_AIL_CONFUSE };
    uiText(UIF_TINY, tx, ty + 52, T(AIL_STR[c.ailment]), UI_BAR_BAD, 0);
  }
  // a platform under each creature, so they stand in the scene rather than
  // floating over it
  uint32_t now = millis();
  int ox = 0, oy = 0;
  bool flash = false;
  if (now < btlFaintUntil[who]) {          // sinks out of frame as it faints
    uint32_t left = btlFaintUntil[who] - now;
    oy += (int)((BTL_FAINT_MS - left) * 70 / BTL_FAINT_MS);
  } else if (c.fainted() && btlSwapWho == (int8_t)who) {
    return;                                // gone, waiting to be replaced
  }
  if (now < btlEnterUntil[who]) {          // and the next one rises into place
    uint32_t left = btlEnterUntil[who] - now;
    oy += (int)(left * 70 / BTL_ENTER_MS);
  }
  if (now < btlLungeUntil[who]) {          // lean in, then back out
    uint32_t left = btlLungeUntil[who] - now;
    int amt = (int)(left > BTL_LUNGE_MS / 2 ? BTL_LUNGE_MS - left : left) * 22 / (BTL_LUNGE_MS / 2);
    ox = who == 0 ? amt : -amt;            // you lunge right, the foe lunges left
    oy = who == 0 ? -amt / 2 : amt / 2;
  }
  if (now < btlHitUntil[who]) {
    uint32_t left = btlHitUntil[who] - now;
    ox += ((left / 50) % 2) ? 5 : -5;      // jitter
  }

  // Real PMD playback when the sprite streamed: attack while lunging, hurt
  // while flinching, idle otherwise. `has()` guards every one, because not
  // every species ships every action -- falling through to idle, and to the
  // flat thumbnail if the sprite is missing entirely (no SD).
  if (btlPmd[who].loaded) {
    uint8_t act = PMD_IDLE;
    bool loop = true;
    uint32_t t = now;
    if (now < btlHitUntil[who] && btlPmd[who].has(PMD_HURT)) {
      act = PMD_HURT; loop = false; t = now - (btlHitUntil[who] - BTL_HIT_MS);
    } else if (now < btlLungeUntil[who] && btlPmd[who].has(PMD_ATTACK)) {
      act = PMD_ATTACK; loop = false; t = now - (btlLungeUntil[who] - BTL_LUNGE_MS);
    }
    drawPmdActM(btlPmd[who], act, sx + 24 + ox, sy + 78 + oy, t, loop, false, 4);
    return;
  }
  const uint8_t *th = thumbs.get(c.dex);
  if (!th) return;
  if (now < btlHitUntil[who]) flash = ((btlHitUntil[who] - now) / 60) % 2 == 0;
  drawThumb(th, sx + ox, sy + oy, 3, flash);
}

// Bars drain rather than snap: a hit that removes half your health should be
// visible as it happens, not as a value that was already different.
static void btlEaseBars() {
  const uint16_t real[2] = { btlYou.hp, btlFoe.hp };
  for (int i = 0; i < 2; i++) {
    int diff = (int)real[i] - (int)btlHpShown[i];
    if (!diff) continue;
    int step = diff / 5;
    if (!step) step = diff > 0 ? 1 : -1;
    btlHpShown[i] = (uint16_t)((int)btlHpShown[i] + step);
  }
}

// The moment the ladder builds toward. It used to be one more line in the same
// message box as "It's super effective!", with the badge awarded silently.
void renderWin() {
  const Trainer &t = BTL_TRAINERS[btlTrainer];
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  uiTextFit(UIF_BIG, CX, 75, T(S_BTL_WIN), UI_BAR_WARN, 1, 300);

  char l[40];
  snprintf(l, sizeof(l), T(S_BTL_BEAT), t.name);
  uiTextFit(UIF_SMALL, CX, 110, l, UI_INK, 1, 330);

  // the badge, large, with the hard-mode halo if that is how it was won
  if (btlTrainer < TRAINER_GYMS) {
    int by = 190;
    if (btlHard) {
      for (int r = 62; r >= 56; r--) gfx->drawCircle(CX, by, r, r % 2 ? 0xFEA0 : 0xFF60);
    }
    const BadgeArt *a = badgeArtFor(btlRegion, btlTrainer);
    if (a) {
      for (int r = 0; r < BADGE_PX; r++)
        for (int c = 0; c < BADGE_PX; c++) {
          uint8_t v = a->idx[r * BADGE_PX + c];
          if (v == 0xFF) continue;
          // 3x, so it reads as a prize rather than a list entry
          gfx->fillRect(CX - BADGE_PX * 3 / 2 + c * 3, by - BADGE_PX * 3 / 2 + r * 3,
                        3, 3, a->pal[v]);
        }
    } else {
      // This region has no badge art. Say so with a plain medal in the leader's
      // type colour rather than borrowing another region's badge.
      const Trainer &tr = TRAINER_SETS[btlRegion % GYM_REGIONS].list[btlTrainer];
      gfx->fillCircle(CX, by, 26, typeColor(tr.type));
      gfx->drawCircle(CX, by, 26, UI_INK);
      char n[4];
      snprintf(n, sizeof(n), "%u", (unsigned)(btlTrainer + 1));
      uiText(UIF_BIG, CX, by + 10, n, typeColorIsLight(tr.type) ? UI_INK : UI_WHITE, 1);
    }
    if (btlNewBadge) {
      uiTextFit(UIF_SMALL, CX, 300, T(S_BTL_NEWBADGE), UI_BAR_OK, 1, 300);
    }
  }
  snprintf(l, sizeof(l), T(S_BADGES_FMT), pet.badgeCountIn(btlRegion, btlHard));
  uiTextFit(UIF_SMALL, CX, 330, l, UI_INK, 1, 300);

  // what the win was worth beyond the badge
  if (btlTrainGain) {
    static const StrId NAMES[3] = { S_TR_ATK, S_TR_DEF, S_TR_SPE };
    snprintf(l, sizeof(l), T(S_WIN_TRAIN_FMT),
             T(NAMES[btlTrainWhich % 3]), btlTrainGain);
    uiTextFit(UIF_SMALL, CX, 358, l, UI_BAR_OK, 1, 280);
  } else if (btlPetIn && btlTrainer >= 0) {
    uiTextFit(UIF_TINY, CX, 358, T(S_WIN_MAXED), UI_TRACK_TEXT, 1, 300);
  }

  uiText(UIF_SMALL, CX, 394, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

void renderBattle() {
  if (btlWinUntil) { renderWin(); return; }
  btlEaseBars();
  gfx->fillScreen(RGB565_BLACK);
  drawBattleBack();
  // the lower band stays flat so the move grid and the HP text keep their
  // contrast against it
  gfx->fillRect(0, 254, 466, 212, UI_BG_DAY);

  // x=82 not 58: at y=60 the round bezel starts around x=77, and a longer
  // name like BLASTOISE was losing its first characters off the edge
  btlSide(82, 82, 300, 62, btlFoe, 1);    // foe reads top-left, sprite top-right
  btlSide(250, 190, 76, 168, btlYou, 0);  // you read bottom-right, sprite bottom-left

  // Waiting on the other device. Without this the screen is identical to the
  // one where it is your turn, so a tap that has been sent and a tap that was
  // never registered look exactly the same.
  bool lanWait = btlLink && !btlOver &&
                 (btlLinkHost ? (btlMyAct && !lan.hasPeerAct())
                              : (lan.state == LINK_WAITING));
  if (lanWait && !btlMsgCount) {
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 14, 12, UI_WHITE);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 14, 12, UI_INK);
    uiTextFit(UIF_SMALL, CX, BTL_GRID_Y + 56, T(S_LAN_WAITFOE), UI_TRACK_TEXT, 1, 300);
  } else if (btlMsgCount) {            // narration takes over the menu area
    // box is 6 px taller than the move grid so four 16 px lines plus "tap..." fit
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 14, 12, UI_WHITE);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H * 2 + 14, 12, UI_INK);
    for (uint8_t i = 0; i < btlMsgCount && i < 4; i++)
      uiTextFit(UIF_TINY, CX, BTL_GRID_Y + 20 + i * 18, btlMsg[i], UI_INK, 1, 310);
    uiText(UIF_TINY, CX, BTL_GRID_Y + 92, "tap...", UI_TRACK_TEXT, 1);
  } else if (btlMenu == 0) {
    // FIGHT across the top, then POKEMON and RUN side by side. Three full-width
    // rows do not fit: the panel is round, and at that depth the chord is only
    // ~250 px. The lower two reuse the move grid's cells, so they inherit its
    // padded hit areas -- which is what made POKEMON hard to press before.
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_BG_DAY);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_INK);
    uiText(UIF_SMALL, CX, BTL_GRID_Y + 28, T(S_FIGHT), UI_INK, 1);
    const char *low[2] = { T(S_BTL_SWITCH), T(S_BTL_RUN) };
    for (int i = 0; i < 2; i++) {
      int x = BTL_CELL_X(i + 2), y = BTL_CELL_Y(i + 2);
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_TRACK_TEXT);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      uiTextFit(UIF_SMALL, x + BTL_CELL_W / 2, y + 28, low[i], UI_INK, 1, BTL_CELL_W - 12);
    }
  } else if (btlMenu == 3) {
    gfx->fillRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_BG_DAY);
    gfx->drawRoundRect(BTL_GRID_X, BTL_GRID_Y, 328, BTL_CELL_H, 10, UI_INK);
    char q[24];
    snprintf(q, sizeof(q), "%s?", T(S_BTL_RUN));
    uiText(UIF_SMALL, CX, BTL_GRID_Y + 28, q, UI_INK, 1);
    const char *yn[2] = { T(S_YES), T(S_NO) };
    for (int i = 0; i < 2; i++) {
      int x = BTL_CELL_X(i + 2), y = BTL_CELL_Y(i + 2);
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, i ? UI_BAR_OK : UI_BAR_BAD);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      uiTextFit(UIF_SMALL, x + BTL_CELL_W / 2, y + 28, yn[i], UI_INK, 1, BTL_CELL_W - 12);
    }
  } else if (btlMenu == 2) {
    drawBtlBack();
    // who to bring on instead; the current one and anything fainted is inert
    const char *sn[4]; int sw[4]; int snN = 0;
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      sn[snN] = ((i == btlSquadAt) ? btlYou : btlSquad[i]).name; sw[snN++] = BTL_CELL_W - 20;
    }
    const UiFont &swFont = uiFitAll(UIF_SMALL, sn, sw, snN);   // one size for every name in the grid
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
      const Combatant &m = (i == btlSquadAt) ? btlYou : btlSquad[i];
      bool usable = (i != btlSquadAt) && !m.fainted();
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, usable ? UI_BG_DAY : UI_TRACK_TEXT);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, usable ? UI_INK : 0x8410);
      uint16_t tc = usable ? UI_INK : 0x8410;
      uiText(swFont, x + 10, y + 19, m.name, tc, 0);
      char hp[20];
      snprintf(hp, sizeof(hp), "%u/%u", m.hp, m.maxHp);
      uiText(UIF_TINY, x + 10, y + 38, hp, tc, 0);
    }
  } else {
    drawBtlBack();
    const char *mn[MOVE_SLOTS]; int mw[MOVE_SLOTS]; int mnN = 0;
    for (int i = 0; i < MOVE_SLOTS; i++)
      if (btlYou.moves[i]) { mn[mnN] = MOVE_TBL[btlYou.moves[i]].name; mw[mnN++] = BTL_CELL_W - 20; }
    const UiFont &mvFont = uiFitAll(UIF_SMALL, mn, mw, mnN);   // one size for every move in the grid
    for (int i = 0; i < MOVE_SLOTS; i++) {
      int x = BTL_CELL_X(i), y = BTL_CELL_Y(i);
      uint8_t mv = btlYou.moves[i];
      gfx->fillRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, mv ? UI_BG_DAY : UI_TRACK_TEXT);
      gfx->drawRoundRect(x, y, BTL_CELL_W, BTL_CELL_H, 10, UI_INK);
      if (!mv) continue;
      uiText(mvFont, x + 10, y + 19, MOVE_TBL[mv].name, UI_INK, 0);
      // Same chip as the move list: in a fight the type IS the decision.
      int cw = drawTypeChip(x + 10, y + 23, MOVE_TBL[mv].type);
      if (hasStab(btlYou.dex, MOVE_TBL[mv].type) &&
          MOVE_TBL[mv].cat != MC_STATUS) {
        uiText(UIF_TINY, x + 10 + cw + 4, y + 38, "+", DEX_TBL[btlYou.dex].accent, 0);
      }
    }
  }
  gfx->flush();
}

// Brings on the flagged replacement and starts its entrance.
static void btlDoSwap() {
  uint32_t now = millis();
  if (btlSwapWho == 1 && btlLink) {
    btlFoeSquad[btlFoeAt] = btlFoe;
    // the next one still standing, not simply the next index
    uint8_t nxt = btlFoeAt;
    while (++nxt < btlFoeSquadN && btlFoeSquad[nxt].fainted()) {}
    if (nxt >= btlFoeSquadN) { btlSwapWho = -1; return; }
    btlFoeAt = nxt;
    btlFoe = btlFoeSquad[btlFoeAt];
    btlHpShown[1] = btlFoe.hp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), lan.peerName, btlFoe.name);
  } else if (btlSwapWho == 1) {
    const Trainer &t = BTL_TRAINERS[btlTrainer];
    btlFoeAt++;
    foeFromSpecies(btlFoe, t.team[btlFoeAt].dex, t.team[btlFoeAt].level,
                   btlHard ? HARD_IV : EASY_IV);
    btlHpShown[1] = btlFoe.maxHp;
    btlSyncSprite(1, btlFoe);
    btlLungeUntil[1] = btlHitUntil[1] = btlFaintUntil[1] = 0;
    btlEnterUntil[1] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_SENDS), t.name, btlFoe.name);
  } else if (btlSwapWho == 0) {
    btlSquad[btlSquadAt] = btlYou;     // remember how battered it was
    btlSquadAt++;
    btlYou = btlSquad[btlSquadAt];
    btlHpShown[0] = btlYou.hp;
    btlSyncSprite(0, btlYou);
    btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
    btlEnterUntil[0] = now + BTL_ENTER_MS;
    btlSay(T(S_BTL_GO), btlYou.name);
  }
  btlSwapWho = -1;
}

// Switching spends your turn: the opponent still acts. That is what stops it
// being a free look at the matchup every round.
static void btlSwitchTo(uint8_t i) {
  if (i >= btlSquadN || i == btlSquadAt) return;
  btlSquad[btlSquadAt] = btlYou;
  btlSquadAt = i;
  btlYou = btlSquad[i];
  btlHpShown[0] = btlYou.hp;
  btlSyncSprite(0, btlYou);
  btlLungeUntil[0] = btlHitUntil[0] = btlFaintUntil[0] = 0;
  btlEnterUntil[0] = millis() + BTL_ENTER_MS;
  btlMenu = 0;
  btlSay(T(S_BTL_GO), btlYou.name);
  btlResolve(0);          // move 0 = no attack, so only the foe acts
}

int btlCellIndexAt(int16_t x, int16_t y) {
  for (int i = 0; i < 4; i++)
    if (btlCellHit(i, x, y)) return i;
  return -1;
}

// The way out of the move and switch screens. Without it the only exits were
// choosing something or leaving the fight entirely.
static void drawBtlBack() {
  gfx->fillRoundRect(BTL_BACK_X, BTL_BACK_Y, BTL_BACK_W, BTL_BACK_H, 11, UI_TRACK_TEXT);
  gfx->drawRoundRect(BTL_BACK_X, BTL_BACK_Y, BTL_BACK_W, BTL_BACK_H, 11, UI_INK);
  uiText(UIF_SMALL, CX, BTL_BACK_Y + 28, T(S_BACK), UI_INK, 1);
}

static bool btlBackTap(int16_t x, int16_t y) {
  if (x < BTL_BACK_X || x > BTL_BACK_X + BTL_BACK_W ||
      y < BTL_BACK_Y || y > BTL_BACK_Y + BTL_BACK_H) return false;
  btlMenu = 0;
  sfxPlay(SFX_TAP);
  return true;
}

// Running. A gym leader keeps their badge and the fight simply ends; against
// another device the peer is told, so it does not sit waiting for a move that
// will never come.
static void btlRun() {
  sfxPlay(SFX_DENY);
  btlFreeSprites();
  audioMusic(MUS_NONE);
  if (btlLink) { lanLeave(); btlLink = false; lanOpen = true; }
  battleOpen = false;
  btlMenu = 0;
}

void battleTap(int16_t x, int16_t y) {
  if (btlWinUntil) {          // dismiss the win screen and leave the fight
    btlWinUntil = 0;
    btlFreeSprites();
    audioMusic(MUS_NONE);
    battleOpen = false;
    if (btlLink) { btlLink = false; lanOpen = true; }
    return;
  }
  if (btlMsgCount) {          // a tap clears the narration and returns the menu
    btlMsgCount = 0;
    if (btlOver) {
      btlFreeSprites();
      battleOpen = false;
      // Back to the LAN screen rather than all the way out: that is where a
      // rematch is offered, and re-pairing for every fight would be tedious.
      if (btlLink) { btlLink = false; lanOpen = true; }
      return;
    }
    if (btlSwapWho >= 0) btlDoSwap();   // the replacement arrives on this beat
    return;
  }
  if (btlMenu == 0) {
    if (x >= BTL_GRID_X - BTL_HIT_PAD && x <= BTL_GRID_X + 328 + BTL_HIT_PAD &&
        y >= BTL_HIT_Y0(0) && y <= BTL_HIT_Y1(0)) {
      sfxPlay(SFX_TAP);
      btlMenu = 1;                       // FIGHT
      return;
    }
    if (btlCellHit(2, x, y)) { sfxPlay(SFX_TAP); btlMenu = 2; return; }
    if (btlCellHit(3, x, y)) { sfxPlay(SFX_TAP); btlMenu = 3; return; }
    return;
  }
  if (btlMenu == 3) {
    // Run confirmation. YES sits where POKEMON is and NO where RUN was, so a
    // second tap on the same spot cancels rather than confirms; anywhere else
    // backs out too. Only an explicit YES leaves the fight.
    if (btlCellHit(2, x, y)) { btlRun(); return; }
    sfxPlay(SFX_TAP);
    btlMenu = 0;
    return;
  }
  if (btlMenu == 2) {
    if (btlBackTap(x, y)) return;
    for (uint8_t i = 0; i < btlSquadN && i < 4; i++) {
      if (!btlCellHit(i, x, y)) continue;
      const Combatant &m = (i == btlSquadAt) ? btlYou : btlSquad[i];
      if (i == btlSquadAt || m.fainted()) { sfxPlay(SFX_DENY); return; }
      sfxPlay(SFX_TAP);
      if (btlLink && !btlLinkHost) {
        // The guest asks; it never switches on its own. A switch rides the same
        // message as a move, so the host spends the turn on it exactly as it
        // would for us.
        lan.sendAct(LINK_ACT_SWITCH_TO(i));
        btlMenu = 0;
        return;
      }
      if (btlLink) {            // host: latched like a move, see btlLinkPoll
        btlMyAct = LINK_ACT_SWITCH_TO(i);
        btlMenu = 0;
        if (!lan.hasPeerAct()) return;
        btlMyAct = 0;
      }
      btlSwitchTo(i);
      return;
    }
    btlMenu = 0;      // anywhere else backs out
    return;
  }
  if (btlBackTap(x, y)) return;
  for (int i = 0; i < MOVE_SLOTS; i++) {
    if (!btlYou.moves[i]) continue;
    if (!btlCellHit(i, x, y)) continue;
    sfxPlay(SFX_TAP);
    btlMenu = 0;
    if (btlLink && !btlLinkHost) {
      lan.sendAct(LINK_ACT_MOVE(i));   // the guest asks; the host decides
      return;
    }
    if (btlLink) {
      // Latched, not discarded. The host used to throw the tap away when the
      // rival had not chosen yet, so you had to keep jabbing at the move until
      // the timing happened to line up.
      btlMyAct = LINK_ACT_MOVE(i);
      if (!lan.hasPeerAct()) return;   // resolved by btlLinkPoll when it lands
      btlMyAct = 0;
    }
    btlResolve(btlYou.moves[i]);
    return;
  }
  btlMenu = 0;        // a tap off the grid goes back to FIGHT/POKEMON
}

// ---------- player card (swipe down) ----------
// Everything here is player-wide and outlives the creature: badges, the daily
// streak, the Pokedex and the party. No player sprite yet -- the SD carries
// PMD creature sprites only, so a trainer portrait needs new art.
// Page 1: who you are -- avatar, badges, totals. Tap the avatar to cycle it;
// the four sprites are hand-drawn (see species.h) because SpriteCollab has no
// trainer art and ripped sprites would be unlicensed.
static void renderPlayerBadges() {
  // the player's name if they have set one, the generic title if not; either
  // way tapping it opens the keyboard
  const char *tn = pet.trainerName[0] ? pet.trainerName : T(S_TRAINER);
  uiTextFit(UIF_BIG, CX, 61, tn, UI_INK, 1, 260);

  // Pages 1 and 2 are the other regions' ladders: name them, and drop the
  // avatar so the badges have the room. Only page 0 is "you".
  if (playerBadgeRegion != 0) {
    const char *rn = TRAINER_SETS[playerBadgeRegion].region;
    uiText(UIF_SMALL, CX, 134, rn, UI_INK, 1);
  } else if (gShowAllAvatars) {          // emulator only: every avatar at once
    for (uint8_t i = 0; i < AVATAR_COUNT; i++)
      drawAvatar(i, 60 + (i % 4) * 88, 60 + (i / 4) * 60, 3);
  } else {
    drawAvatar(pet.avatar, CX - AVATAR_PX * 2, 72, 4);
    // the sprite alone is not always obvious at 16x16, so it is named
    const char *an = AVATARS[pet.avatar % AVATAR_COUNT].name;
    uiTextFit(UIF_TINY, CX, 148, an, UI_INK, 1, 200);
    uiTextFit(UIF_TINY, CX, 166, T(S_AVATAR_HINT), UI_TRACK_TEXT, 1, 260);
  }

  // The real badges, 2x4. Unearned ones draw as a faint outline so the shape
  // of what is missing is still visible.
  for (int i = 0; i < TRAINER_GYMS; i++) {
    int bx = 140 + (i % 4) * 62, by = 202 + (i / 4) * 60;
    bool got = pet.hasBadge(playerBadgeRegion, i, false);
    bool hard = pet.hasBadge(playerBadgeRegion, i, true);
    if (hard) {
      // Beaten on hard: a golden halo. Concentric rings, not a filled disc --
      // a disc sat behind the art and read as a gold coin rather than a glow.
      gfx->drawCircle(bx, by, 25, 0xFDE0);
      gfx->drawCircle(bx, by, 24, 0xFEA0);
      gfx->drawCircle(bx, by, 23, 0xFF60);
      gfx->drawCircle(bx, by, 22, 0xFEA0);
      gfx->drawCircle(bx, by, 21, 0xFDE0);
    }
    if (!got) {
      gfx->drawCircle(bx, by, 20, UI_TRACK_TEXT);
      continue;
    }
    const BadgeArt *a = badgeArtFor(playerBadgeRegion, i);
    if (!a) {                 // no art for this region: a filled disc, won is won
      gfx->fillCircle(bx, by, 14, typeColor(TRAINER_SETS[playerBadgeRegion % GYM_REGIONS].list[i].type));
      gfx->drawCircle(bx, by, 14, UI_INK);
      continue;
    }
    for (int r = 0; r < BADGE_PX; r++)
      for (int c = 0; c < BADGE_PX; c++) {
        uint8_t v = a->idx[r * BADGE_PX + c];
        if (v == 0xFF) continue;
        gfx->fillRect(bx - BADGE_PX / 2 + c, by - BADGE_PX / 2 + r, 1, 1, a->pal[v]);
      }
  }
  char l[32];
  snprintf(l, sizeof(l), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  uiText(UIF_SMALL, CX, 312, l, UI_INK, 1);
  snprintf(l, sizeof(l), T(S_POKEDEX_FMT), pet.registeredCount(), DEX_COUNT);
  uiText(UIF_SMALL, CX, 336, l, UI_INK, 1);
  snprintf(l, sizeof(l), T(S_PARTY_FMT), party.count());
  uiText(UIF_SMALL, CX, 360, l, UI_INK, 1);
}

// Page 2: the medals. They used to sit on the creature's card; they belong with
// the player, since totalMedals accumulates across every pet you raise.
static void renderPlayerMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[24];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  uiTextFit(UIF_BIG, CX, 65, head, UI_INK, 1, 300);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 46 + (i % 2) * 190, y = 96 + (i / 2) * 58;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 180, 48, 10, g ? UI_BAR_OK : UI_TRACK_TEXT);
    gfx->drawRoundRect(x, y, 180, 48, 10, UI_INK);
    uiTextFit(UIF_SMALL, x + 90, y + 20, medalLabel(i), g ? UI_BG_DAY : UI_INK, 1, 170);
    uiTextFit(UIF_TINY, x + 90, y + 40, medalDesc(i), g ? UI_BG_DAY : UI_INK, 1, 172);
  }
  char tot[28];
  snprintf(tot, sizeof(tot), T(S_MEDALS_TOTAL_FMT), pet.totalMedals);
  uiTextFit(UIF_SMALL, CX, 350, tot, UI_TRACK_TEXT, 1, 300);
}

void renderPlayer() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (playerPage < GYM_REGIONS) renderPlayerBadges();
  else renderPlayerMedals();

  for (uint8_t i = 0; i < PLAYER_PAGES; i++) {
    int dx = CX - (PLAYER_PAGES - 1) * 13 + i * 26;
    if (i == playerPage) gfx->fillCircle(dx, 394, 5, UI_INK);
    else gfx->drawCircle(dx, 394, 4, UI_INK);
  }
  uiText(UIF_SMALL, CX, 430, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

// ---------- reaction test (trains SPEED) ----------
// The third training verb. The bag is a masher and the ball is a juggler, so
// this one is a reaction: a target appears somewhere on the panel and you tap
// it before it expires. The window shrinks as you go, which is what makes it
// read as speed rather than as endurance.
#define SPD_MS 15000UL       // session length
#define SPD_LIFE0 1100       // first target's window, ms
#define SPD_LIFE_MIN 380
#define SPD_R 46             // target radius

void spdSpawn() {
  // Keep the whole target inside the bezel: pick an angle and a radius that
  // leave SPD_R of margin, rather than a square that clips at the corners.
  int ang = random(360);
  int rad = random(150);
  float a = ang * 3.14159f / 180.0f;
  spdX = CX + (int)(cosf(a) * rad);
  spdY = CY + (int)(sinf(a) * rad);
  spdBorn = millis();
}

void startSpeedGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  spdOpen = true;
  spdUntil = millis() + SPD_MS;
  spdOverUntil = 0;
  spdHits = 0;
  spdMisses = 0;
  spdGain = 0;
  spdNewHi = false;
  spdSpawn();
}

static uint16_t spdLife() {
  int life = SPD_LIFE0 - spdHits * 32;
  return life < SPD_LIFE_MIN ? SPD_LIFE_MIN : life;
}

void spdTap(int16_t x, int16_t y) {
  if (spdOverUntil) return;
  int dx = x - spdX, dy = y - spdY;
  if (dx * dx + dy * dy <= (SPD_R + 14) * (SPD_R + 14)) {
    spdHits++;
    sfxPlay(SFX_TAP);
    spdSpawn();
  }
}

void renderSpeed() {
  uint32_t now = millis();
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (spdOverUntil) {
    if (now > spdOverUntil) { spdOpen = false; return; }
    char b[24];
    snprintf(b, sizeof(b), T(S_SCORE_FMT), spdHits);
    uiTextFit(UIF_BIG, CX, 178, b, ink, 1, 340);
    char g[20];
    snprintf(g, sizeof(g), T(S_SPD_GAIN_FMT), spdGain);
    uiTextFit(UIF_BIG, CX, 231, g, UI_BAR_WARN, 1, 340);
    if (spdNewHi && spdHits > 0) {
      uiText(UIF_SMALL, CX, 270, T(S_NEW_RECORD), UI_BAR_WARN, 1);
    } else {
      char r[20];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.spdHi);
      uiText(UIF_SMALL, CX, 270, r, ink, 1);
    }
    gfx->flush();
    return;
  }

  // session over?
  if (now >= spdUntil) {
    spdNewHi = (spdHits > pet.spdHi);
    spdGain = pet.trainSpeed(spdHits);
    sfxPlay(spdNewHi ? SFX_MEDAL : SFX_PLAY);
    spdOverUntil = now + 3500;
    gfx->flush();
    return;
  }
  // target expired?
  if (now - spdBorn > spdLife()) {
    spdMisses++;
    spdSpawn();
  }

  // the target, shrinking as its window runs out so the urgency is visible
  uint32_t age = now - spdBorn;
  int life = spdLife();
  int r = SPD_R - (int)((uint32_t)SPD_R * age / (life ? life : 1) / 2);
  if (r < 8) r = 8;
  gfx->fillCircle(spdX, spdY, r, UI_BAR_BAD);
  gfx->fillCircle(spdX, spdY, r * 2 / 3, UI_WHITE);
  gfx->fillCircle(spdX, spdY, r / 3, UI_BAR_BAD);

  char b[12];
  snprintf(b, sizeof(b), "%u", spdHits);
  uiText(UIF_BIG, CX, 58, b, ink, 1);
  // seconds left
  uint32_t left = (spdUntil > now) ? (spdUntil - now + 999) / 1000 : 0;
  snprintf(b, sizeof(b), "%us", (unsigned)left);
  uiText(UIF_SMALL, CX, 90, b, ink, 1);
  gfx->flush();
}

// ---------- team select ----------
// Which creatures come to this fight. It exists because hard mode caps your
// team to the leader's size, so the difference between a sweep and a wipe is
// bringing the right type -- and the squad used to be simply whoever sat first
// in the party.

// candidate n: 0 = the live pet, 1..PARTY_SLOTS = banked members
bool pickExists(uint8_t n) {
  if (n == 0) return !pet.isEgg();
  return n <= PARTY_SLOTS && !party.slots[n - 1].empty();
}
uint8_t pickChosen() {
  uint8_t c = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++)
    if (pickExists(n) && (squadMask & (1 << n))) c++;
  return c;
}
uint8_t pickCandidates() {
  uint8_t c = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++)
    if (pickExists(n)) c++;
  return c;
}
// Trims the selection to the first `cap` candidates. The default used to be
// "everything", which with a live pet plus six banked is seven against a cap of
// six -- so the screen opened already invalid.
void pickDefault(uint8_t cap) {
  squadMask = 0;
  uint8_t taken = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS && taken < cap; n++)
    if (pickExists(n)) { squadMask |= (1 << n); taken++; }
}

static void drawPickCell(uint8_t n, int x, int y, uint8_t capLvl) {
  bool on = (squadMask & (1 << n)) != 0;
  int16_t dex; uint16_t lvl; const char *nm; bool shiny;
  if (n == 0) {
    dex = pet.speciesId; lvl = pet.level(); shiny = pet.shiny;
    nm = pet.nick[0] ? pet.nick : DEX_TBL[dex].name;
  } else {
    const PartyMon &m = party.slots[n - 1];
    dex = m.dex; lvl = m.level; shiny = m.shiny;
    nm = m.nick[0] ? m.nick : DEX_TBL[dex].name;
  }
  if (capLvl && lvl > capLvl) lvl = capLvl;   // show the level it will FIGHT at
  gfx->fillRoundRect(x, y, PICK_CELL_W, PICK_CELL_H, 10, on ? UI_BG_DAY : UI_TRACK_TEXT);
  gfx->drawRoundRect(x, y, PICK_CELL_W, PICK_CELL_H, 10, on ? UI_INK : 0x8410);
  const uint8_t *th = thumbs.get(dex);
  if (th) drawThumb(th, x - 12, y - 6, 2, !on);
  uint16_t tc = on ? UI_INK : 0x8410;
  uiTextFit(UIF_TINY, x + 54, y + 18, nm, tc, 0, PICK_CELL_W - 54 - 6);
  char l[16];
  snprintf(l, sizeof(l), "Lv.%u%s", (unsigned)lvl, shiny ? " *" : "");
  uiText(UIF_TINY, x + 54, y + 35, l, tc, 0);
  // its typing is the whole reason you are on this screen
  const DexEntry &d = DEX_TBL[dex];
  uint16_t ac = on ? d.accent : 0x8410;
  uiText(UIF_TINY, x + 54, y + 52, typeName(d.type1), ac, 0);
  if (d.type2 != T_NONE)
    uiText(UIF_TINY, x + 54, y + 69, typeName(d.type2), ac, 0);
  if (on) {                       // a tick in a green disc
    int cx = x + PICK_CELL_W - 18, cy = y + 30;     // beside the level line, clear of the name
    gfx->fillCircle(cx, cy, 10, UI_BAR_OK);
    gfx->drawLine(cx - 5, cy, cx - 2, cy + 4, UI_BG_DAY);
    gfx->drawLine(cx - 5, cy + 1, cx - 2, cy + 5, UI_BG_DAY);
    gfx->drawLine(cx - 2, cy + 4, cx + 5, cy - 4, UI_BG_DAY);
    gfx->drawLine(cx - 2, cy + 5, cx + 5, cy - 3, UI_BG_DAY);
  }
}

void renderPick() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uint8_t cap = squadCap(pickTrainer, pickHard);
  uint8_t top = 0;          // the level cap shown on each cell; 0 = uncapped
  char head[40];
  if (pickTrainer == PICK_LAN) {
    snprintf(head, sizeof(head), "%s: %s", T(S_LAN),
             lanWantHost ? T(S_LAN_HOST) : T(S_LAN_JOIN));
  } else {
    const Trainer &t = TRAINERS[pickTrainer];
    for (int k = 0; k < t.count; k++)
      if (t.team[k].level > top) top = t.team[k].level;
    snprintf(head, sizeof(head), "%s  Lv.%u x%u", t.name, top, t.count);
  }
  uiTextFit(UIF_SMALL, CX, 54, head, UI_INK, 1, 260);
  char sub[28];
  snprintf(sub, sizeof(sub), T(S_PICK_FMT), pickChosen(), cap);
  uiTextFit(UIF_TINY, CX, 78, sub, pickChosen() > cap ? UI_BAR_BAD : UI_TRACK_TEXT, 1, 280);

  uint8_t seen = 0, drawn = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++) {
    if (!pickExists(n)) continue;
    if (seen++ < pickPage * PICK_PER_PAGE) continue;
    if (drawn >= PICK_PER_PAGE) break;
    drawPickCell(n, PICK_X(drawn), PICK_Y(drawn), top);
    drawn++;
  }
  uint8_t pages = (pickCandidates() + PICK_PER_PAGE - 1) / PICK_PER_PAGE;
  if (!pages) pages = 1;
  for (uint8_t i = 0; i < pages && pages > 1; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == pickPage) gfx->fillCircle(dx, 332, 5, UI_INK);
    else gfx->drawCircle(dx, 332, 4, UI_INK);
  }

  bool ok = pickChosen() > 0 && pickChosen() <= cap;
  gfx->fillRoundRect(PICK_BACK_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_TRACK_TEXT);
  gfx->drawRoundRect(PICK_BACK_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_INK);
  uiTextFit(UIF_SMALL, PICK_BACK_X + PICK_BTN_W / 2, PICK_GO_Y + 28, T(S_BACK), UI_INK, 1,
            PICK_BTN_W - 12);
  gfx->fillRoundRect(PICK_GO_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12,
                     ok ? UI_BAR_OK : UI_TRACK_TEXT);
  gfx->drawRoundRect(PICK_GO_X, PICK_GO_Y, PICK_BTN_W, PICK_BTN_H, 12, UI_INK);
  uiTextFit(UIF_SMALL, PICK_GO_X + PICK_BTN_W / 2, PICK_GO_Y + 28, T(S_FIGHT),
            ok ? UI_BG_DAY : 0x8410, 1, PICK_BTN_W - 12);
  gfx->flush();
}

void pickTap(int16_t x, int16_t y) {
  if (y >= PICK_GO_Y && y <= PICK_GO_Y + PICK_BTN_H &&
      x >= PICK_BACK_X && x <= PICK_BACK_X + PICK_BTN_W) {   // BACK
    sfxPlay(SFX_TAP);
    pickOpen = false;
    if (pickTrainer == PICK_LAN) { lanOpen = true; }
    else { gymOpen = true; }
    return;
  }
  if (y >= PICK_GO_Y && y <= PICK_GO_Y + PICK_BTN_H &&
      x >= PICK_GO_X && x <= PICK_GO_X + PICK_BTN_W) {
    uint8_t cap = squadCap(pickTrainer, pickHard);
    if (pickChosen() == 0 || pickChosen() > cap) return;   // GO stays inert
    sfxPlay(SFX_TAP);
    pickOpen = false;
    if (pickTrainer == PICK_LAN) {
      // The squad is chosen BEFORE the radio comes up, so what gets offered to
      // the peer is what the player picked -- lanOffer() builds lan.mine from
      // squadMask, and the fight is then rebuilt from lan.mine rather than from
      // the party (see startLinkBattle).
      lanOffer(lanWantHost);
      lanOpen = true;
      return;
    }
    startTrainerBattle(pickTrainer, pickHard);
    return;
  }
  uint8_t seen = 0, drawn = 0;
  for (uint8_t n = 0; n <= PARTY_SLOTS; n++) {
    if (!pickExists(n)) continue;
    if (seen++ < pickPage * PICK_PER_PAGE) continue;
    if (drawn >= PICK_PER_PAGE) break;
    int cx0 = PICK_X(drawn), cy0 = PICK_Y(drawn);
    drawn++;
    if (x < cx0 || x > cx0 + PICK_CELL_W || y < cy0 || y > cy0 + PICK_CELL_H) continue;
    squadMask ^= (1 << n);
    sfxPlay(SFX_TAP);
    return;
  }
}

// ---------- LAN battle ----------
// Pairing on a touch-only screen: one device hosts, the other joins, and the
// protocol does the rest. There is no MAC entry because there is no keyboard
// worth typing one on.
void renderLan() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uiText(UIF_SMALL, CX, 58, T(S_LAN), UI_INK, 1);

  const char *msg = T(S_LAN_PICK);
  switch (lan.state) {
    case LINK_HANDSHAKE:
    case LINK_LISTENING: msg = T(S_LAN_WAIT); break;
    case LINK_SQUADS:    msg = T(S_LAN_WAIT); break;
    case LINK_READY:     msg = T(S_LAN_READY); break;
    case LINK_REFUSED:   msg = T(S_LAN_REFUSED); break;
    case LINK_LOST:      msg = T(S_LAN_GONE); break;
    case LINK_DONE:      msg = lan.youWon ? T(S_BTL_WIN) : T(S_BTL_LOSE); break;
    default: break;
  }
  uiTextFit(UIF_TINY, CX, 86, msg,
            (lan.state == LINK_REFUSED || lan.state == LINK_LOST) ? UI_BAR_BAD : UI_TRACK_TEXT,
            1, 300);

  if (lan.state == LINK_OFF || lan.state == LINK_REFUSED ||
      lan.state == LINK_LOST) {
    const char *lab[2] = { T(S_LAN_HOST), T(S_LAN_JOIN) };
    for (int i = 0; i < 2; i++) {
      int y = 120 + i * 70;
      gfx->fillRoundRect(90, y, 286, 56, 12, UI_BG_DAY);
      gfx->drawRoundRect(90, y, 286, 56, 12, UI_INK);
      uiTextFit(UIF_SMALL, CX, y + 35, lab[i], UI_INK, 1, 260);
    }
  } else if (lan.state == LINK_READY) {
    char l[40];
    if (lan.peerName[0]) {
      uiTextFit(UIF_SMALL, CX, 144, lan.peerName, UI_INK, 1, 300);
    }
    snprintf(l, sizeof(l), T(S_LAN_VS), lan.theirsN);
    uiTextFit(UIF_SMALL, CX, 174, l, UI_INK, 1, 300);
    gfx->fillRoundRect(120, 220, 226, 56, 12, UI_BAR_OK);
    gfx->drawRoundRect(120, 220, 226, 56, 12, UI_INK);
    uiTextFit(UIF_SMALL, CX, 255, T(S_FIGHT), UI_BG_DAY, 1, 206);
  } else if (lan.state == LINK_DONE) {
    // Both squads are still in hand on both devices, so going again costs one
    // packet -- there is nothing to re-exchange.
    if (lan.peerName[0]) {
      uiTextFit(UIF_SMALL, CX, 154, lan.peerName, UI_INK, 1, 300);
    }
    gfx->fillRoundRect(120, 220, 226, 56, 12, UI_BG_DAY);
    gfx->drawRoundRect(120, 220, 226, 56, 12, UI_INK);
    uiTextFit(UIF_SMALL, CX, 255, T(S_LAN_REMATCH), UI_INK, 1, 206);
  }
  uiText(UIF_SMALL, CX, 406, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

// Hands our chosen squad to the link, then announces.
// Leaving deliberately: tell the peer so it reports at once instead of sitting
// out the timeout, then free the radio -- it costs real current.
void lanLeave() {
  if (lan.live()) lan.sendBye();
  linkNowEnd();
  lan.state = LINK_OFF;
}

static void lanOffer(bool host) {
  // The squad is built BEFORE the radio is touched. What we advertise has to be
  // exactly what the player just chose in the picker, and that does not depend
  // on whether the radio comes up -- doing it the other way round meant a
  // failed radio skipped the squad entirely and left nothing to inspect.
  lan.begin(host, pet.trainerName);
  snprintf(lan.peerName, sizeof(lan.peerName), "%s", pet.trainerName);
  buildSquad(0, TRAINER_TEAM_MAX, squadMask);
  for (uint8_t i = 0; i < btlSquadN; i++) {
    LinkMon m;
    linkMonFrom(m, btlSquad[i]);
    lan.addMon(m);
  }
  if (!linkNowBegin(&lan)) {          // no radio: say so rather than hanging
    lan.state = LINK_REFUSED;
    return;
  }
  // BOTH sides announce. Which of them ends up hosting is settled by id inside
  // the hello, so the buttons are only a preference -- two players who both tap
  // HOST still get a working fight instead of two authorities, and two who both
  // tap JOIN still get one instead of mutual silence.
  lan.start();
}

void lanTap(int16_t x, int16_t y) {
  if (lan.state == LINK_OFF || lan.state == LINK_REFUSED ||
      lan.state == LINK_LOST) {
    for (int i = 0; i < 2; i++) {
      int by = 120 + i * 70;
      if (x < 90 || x > 376 || y < by || y > by + 56) continue;
      sfxPlay(SFX_TAP);
      lanWantHost = (i == 0);
      lanOpen = false;
      pickTrainer = PICK_LAN;
      pickHard = false;
      pickPage = 0;
      pickDefault(squadCap(PICK_LAN, false));
      pickOpen = true;
      return;
    }
  } else if (lan.state == LINK_READY) {
    if (x >= 120 && x <= 346 && y >= 220 && y <= 276) {
      sfxPlay(SFX_TAP);
      lanOpen = false;
      startLinkBattle();
      return;
    }
  } else if (lan.state == LINK_DONE) {
    if (x >= 120 && x <= 346 && y >= 220 && y <= 276) {
      sfxPlay(SFX_TAP);
      lan.sendRematch();      // both sides go back to READY and tap FIGHT
      return;
    }
  }
  if (y > 370) { lanLeave(); lanOpen = false; }   // back
}

// ---------- gym list ----------
void renderGyms() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // The ladder's own region in the title, since a vertical swipe moves between
  // three of them and "GYMS" alone would not say which you are looking at.
  char title[28];
  snprintf(title, sizeof(title), "%s %s", TRAINER_SETS[gymRegion % GYM_REGIONS].region,
           T(S_GYMS));
  uiTextFit(UIF_MID, CX, 54, title, UI_INK, 1, 280);
  // The badge count used to sit here, directly behind the difficulty pill. The
  // region chooser already shows it per region, which is where you are choosing
  // from, so it was both redundant and in the way.
  // difficulty pill: hard caps YOUR team to the leader's size and level, so it
  // is a different ladder with its own badges rather than a damage multiplier
  const char *dif = T(gymHard ? S_HARD : S_EASY);
  int dw = uiTextWidth(UIF_SMALL, dif) + 48;      // wider as well as taller
  if (dw < 120) dw = 120;
  gfx->fillRoundRect(CX - dw / 2, GYMDIF_Y, dw, GYMDIF_H, 12,
                     gymHard ? UI_BAR_BAD : UI_TRACK_TEXT);
  uiText(UIF_SMALL, CX, GYMDIF_Y + 28, dif, gymHard ? UI_BG_DAY : UI_INK, 1);

  for (int i = 0; i < GYM_ROWS; i++) {
    uint8_t idx = gymPage * GYM_ROWS + i;
    if (idx >= TRAINER_COUNT) break;
    const Trainer &t = TRAINERS[idx];
    int y = GYM_ROW_Y(i);
    bool done = pet.hasBadge(gymRegion, idx, gymHard);
    bool open_ = gymUnlocked(idx, gymHard);
    gfx->fillRoundRect(70, y, 326, 44, 10, done ? UI_TRACK_TEXT : UI_BG_DAY);
    gfx->drawRoundRect(70, y, 326, 44, 10, open_ ? UI_INK : UI_TRACK_TEXT);
    uiTextFit(UIF_TINY, 84, y + 19, t.name, open_ ? UI_INK : UI_TRACK_TEXT, 0, 270);
    uiTextFit(UIF_TINY, 84, y + 38, open_ ? t.place : T(S_LOCKED), UI_TRACK_TEXT, 0, 180);
    // the level of the strongest creature: the honest measure of the wall
    uint8_t top = 0;
    for (int k = 0; k < t.count; k++)
      if (t.team[k].level > top) top = t.team[k].level;
    char lv[16];
    snprintf(lv, sizeof(lv), "Lv.%u x%u", top, t.count);
    uiText(UIF_TINY, 384, y + 38, lv, done ? UI_BAR_OK : (open_ ? UI_INK : UI_TRACK_TEXT), 2);
    if (done) uiText(UIF_TINY, 384, y + 19, "*", UI_BAR_OK, 2);
  }
  uint8_t pages = (TRAINER_COUNT + GYM_ROWS - 1) / GYM_ROWS;
  for (uint8_t i = 0; i < pages; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == gymPage) gfx->fillCircle(dx, 394, 5, UI_INK);
    else gfx->drawCircle(dx, 394, 4, UI_INK);
  }
  // LAN battle is reached from the region chooser, not from a ladder
  uiText(UIF_SMALL, CX, 430, T(S_BACK), UI_TRACK_TEXT, 1);   // a tap off the rows goes back
  gfx->flush();
}

// The region pill under a waiting egg. Tapping it cycles; Pet::setRegion swaps
// the egg to that region's creature, keeping the rarity it was granted and
// remembering each region's answer so flipping back and forth is not a re-roll.
#define EGGREG_X 133
#define EGGREG_Y 374
#define EGGREG_W 200
#define EGGREG_H 34
// The hit area is BIGGER than the pill, like the BOX button and the battle
// grid, and for the same reason: a 34 px target is under UI_TAP_MIN and a
// finger is not a stylus.
//
// The guard band matters more than the padding. Missing this pill fell through
// to pet.eggTap(), and THREE taps hatch the egg -- so fumbling at the region
// selector hatched the very egg you were trying to re-aim. A near miss now
// does nothing at all, which is the correct answer for a control whose
// neighbour is irreversible.
#define EGGREG_PAD 16
#define EGGREG_GUARD 14

static void drawEggRegion() {
  char l[24];
  snprintf(l, sizeof(l), "%s >", pet.regionName());
  gfx->fillRoundRect(EGGREG_X, EGGREG_Y, EGGREG_W, EGGREG_H, 10, UI_WHITE);
  gfx->drawRoundRect(EGGREG_X, EGGREG_Y, EGGREG_W, EGGREG_H, 10, UI_INK);
  uiTextFit(UIF_SMALL, EGGREG_X + EGGREG_W / 2, EGGREG_Y + 9 + 14, l, UI_INK, 1, EGGREG_W - 16);
  uiTextFit(UIF_TINY, CX, EGGREG_Y + EGGREG_H + 20, T(S_EGG_REGION), UI_TRACK_TEXT, 1, 230);
}

// True if the tap was on the region pill, so the egg does not also get cracked.
// The egg's region pill: its graphic, and the area that actually accepts a tap.
// Exposed so a test can prove the second is bigger than the first and that a
// near miss does not reach pet.eggTap().
void uiEggPillRect(int *x, int *y, int *w, int *h, bool hitArea) {
  int pad = hitArea ? EGGREG_PAD : 0;
  if (x) *x = EGGREG_X - pad;
  if (y) *y = EGGREG_Y - pad;
  if (w) *w = EGGREG_W + 2 * pad;
  if (h) *h = EGGREG_H + 2 * pad;
}

// 1 = cycled the region, -1 = a near miss that must NOT reach the egg, 0 = not
// ours at all.
static int eggRegionTap(int16_t x, int16_t y) {
  if (!pet.isEgg()) return 0;
  int inset = EGGREG_PAD, guard = EGGREG_PAD + EGGREG_GUARD;
  bool hit = x >= EGGREG_X - inset && x <= EGGREG_X + EGGREG_W + inset &&
             y >= EGGREG_Y - inset && y <= EGGREG_Y + EGGREG_H + inset;
  if (hit) {
    pet.setRegion(nextAvailableRegion(pet.region));
    sfxPlay(SFX_TAP);
    return 1;
  }
  bool near = x >= EGGREG_X - guard && x <= EGGREG_X + EGGREG_W + guard &&
              y >= EGGREG_Y - guard && y <= EGGREG_Y + EGGREG_H + guard;
  if (near) { sfxPlay(SFX_DENY); return -1; }
  return 0;
}

// The region chooser used by the Pokedex and the gym ladder. Each row carries
// its own progress, so the screen answers "where am I up to" as well as "where
// do I want to go".
#define RPICK_X 74
#define RPICK_W 318
#define RPICK_H 62
#define RPICK_Y(i) (108 + (i) * 72)
// The page dots sit BETWEEN the last row and the LAN button, not over it.
// They used to be at y=366, which is inside the LAN button (336..380) and on
// top of its label -- invisible until a region chooser had more than one page,
// so it appeared the moment Sinnoh made GYM_REGIONS 4 and went unnoticed
// because swipe_test drives the paging and never looks at the pixels.
#define RPICK_DOTS_Y 325
#define RPICK_DOT_R 5
// Guards, not decoration: both of these are the collision that shipped, and a
// static_assert is the only check that cannot be forgotten when somebody moves
// a button. The dots must clear the LAN button below and the last row above.
static_assert(RPICK_DOTS_Y + RPICK_DOT_R < LANBTN_Y,
              "the region-chooser page dots overlap the LAN button");
static_assert(RPICK_DOTS_Y - RPICK_DOT_R > RPICK_Y(RPICK_PER_PAGE - 1) + RPICK_H,
              "the region-chooser page dots overlap the last region row");

// How many regions this mode lists. Gyms genuinely only exist for three
// (GYM_REGIONS); the Pokedex and the starter screen list every REAL region,
// which is GAL_REGIONS -- REGION_COUNT minus the ALL pseudo-region.
//
// This used to be GYM_REGIONS for ALL THREE MODES, with the names read out of
// TRAINER_SETS -- a trainer table driving the Pokedex chooser. So the moment
// Sinnoh landed it had no row, while the gallery's vertical swipe cycled it
// happily: built, reachable, and looking absent. That is the exact failure the
// chooser was added to prevent.
uint8_t rpickRegions(uint8_t mode) {
  return (mode == RPICK_FOR_GYMS) ? (uint8_t)GYM_REGIONS : (uint8_t)GAL_REGIONS;
}

uint8_t rpickPageCount(uint8_t mode) {
  uint8_t n = rpickRegions(mode);
  uint8_t p = (uint8_t)((n + RPICK_PER_PAGE - 1) / RPICK_PER_PAGE);
  return p ? p : 1;
}

// The chooser WRAPS rather than closing off the end, unlike the other paged
// screens. It is the root of its own screen, and at first boot there is
// nowhere to go back to at all -- exiting would strand the player before they
// have chosen anything.
// Which chooser is on the panel, or 0xFF for none. THE single answer: the
// swipe handler and swipe_test both ask this rather than each deciding.
uint8_t rpickModeNow() {
  switch (uiCurrentScreen()) {
    case SCR_REGION:  return RPICK_FOR_START;
    case SCR_DEXPICK: return RPICK_FOR_DEX;
    case SCR_GYMPICK: return RPICK_FOR_GYMS;
    default: return 0xFF;
  }
}

static bool rpickSwipe(int dir) {
  uint8_t mode = rpickModeNow();
  if (mode == 0xFF) return false;
  uint8_t pages = rpickPageCount(mode);
  int p = (int)rpickPage + (dir > 0 ? -1 : 1);
  if (p < 0) p = pages - 1;
  if (p >= pages) p = 0;
  rpickPage = (uint8_t)p;
  sfxPlay(SFX_TAP);
  return true;
}

static void renderRegionPick(uint8_t mode) {
  bool forGyms = (mode == RPICK_FOR_GYMS);
  uint8_t nreg = rpickRegions(mode);
  uint8_t pages = rpickPageCount(mode);
  if (rpickPage >= pages) rpickPage = 0;
  uint8_t first = (uint8_t)(rpickPage * RPICK_PER_PAGE);
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char ttl[40];
  if (mode == RPICK_FOR_START) snprintf(ttl, sizeof(ttl), "%s", T(S_CHOOSE_REGION));
  else if (forGyms) snprintf(ttl, sizeof(ttl), "%s", T(S_GYMS));
  else snprintf(ttl, sizeof(ttl), T(S_POKEDEX_FMT), pet.registeredCount(), DEX_COUNT);
  uiTextFit(UIF_MID, CX, 70, ttl, UI_INK, 1, 280);

  // The subtitle a row shows. At first boot there is none: naming the starter here
  // would give away the next screen, and the counts the other two modes show would
  // all read zero on a new save anyway.
  auto subFor = [&](uint8_t i, bool open, char *sub, size_t n) {
    sub[0] = 0;
    if (mode == RPICK_FOR_START && open) return;
    if (!open)
      snprintf(sub, n, "%s", T(S_NEED_PACK));
    else if (mode == RPICK_FOR_GYMS)
      snprintf(sub, n, T(S_BADGES_FMT), pet.badgeCountIn(i, gymHard));
    else if (mode == RPICK_FOR_DEX)
      snprintf(sub, n, "%u/%u", pet.registeredCountIn(REGIONS[i].lo, REGIONS[i].hi),
               (unsigned)(REGIONS[i].hi - REGIONS[i].lo + 1));
  };
  // every region name on the page at one size: the tightest row decides it
  const char *rn[RPICK_PER_PAGE]; int rw[RPICK_PER_PAGE]; int rnN = 0;
  for (uint8_t row = 0; row < RPICK_PER_PAGE && first + row < nreg; row++) {
    uint8_t i = (uint8_t)(first + row);
    char sub[28];
    subFor(i, forGyms || regionAvailable(i), sub, sizeof(sub));
    int subW = sub[0] ? uiTextWidth(UIF_SMALL, sub) : 0;
    rn[rnN] = forGyms ? TRAINER_SETS[i].region : REGIONS[i].name;
    rw[rnN++] = RPICK_W - 36 - subW - (subW ? 8 : 0);
  }
  const UiFont &regFont = uiFitAll(UIF_MID, rn, rw, rnN);

  for (uint8_t row = 0; row < RPICK_PER_PAGE; row++) {
    uint8_t i = (uint8_t)(first + row);
    if (i >= nreg) break;
    int y = RPICK_Y(row);
    // The sprite pack is the gate. A region without it is drawn GREYED with a
    // reason rather than dropped from the list: hiding it would say "this
    // region does not exist" when what we mean is "download its pack".
    bool open = forGyms || regionAvailable(i);
    gfx->fillRoundRect(RPICK_X, y, RPICK_W, RPICK_H, 12, open ? UI_WHITE : UI_BG_DAY);
    gfx->drawRoundRect(RPICK_X, y, RPICK_W, RPICK_H, 12, open ? UI_INK : UI_TRACK_TEXT);
    const char *nm = forGyms ? TRAINER_SETS[i].region : REGIONS[i].name;
    char sub[28];
    subFor(i, open, sub, sizeof(sub));
    if (sub[0])
      uiText(UIF_SMALL, RPICK_X + RPICK_W - 18, uiMidY(UIF_SMALL, y, RPICK_H), sub, UI_TRACK_TEXT, 2);
    uiText(regFont, RPICK_X + 18, uiMidY(regFont, y, RPICK_H), nm, open ? UI_INK : UI_TRACK_TEXT, 0);
  }
  if (forGyms) {
    gfx->fillRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_BG_DAY);
    gfx->drawRoundRect(LANBTN_X, LANBTN_Y, LANBTN_W, LANBTN_H, 11, UI_INK);
    uiTextFit(UIF_SMALL, CX, LANBTN_Y + 28, T(S_LAN), UI_INK, 1, LANBTN_W - 12);
  }
  if (pages > 1) {                        // dots: which page of regions this is
    // on the gym chooser the dots sit BELOW the LAN BATTLE button
    const int dotsY = forGyms ? LANBTN_Y + LANBTN_H + 20 : RPICK_DOTS_Y;
    int total = pages * 16 - 8;
    for (uint8_t d = 0; d < pages; d++) {
      int cx = CX - total / 2 + d * 16;
      if (d == rpickPage) gfx->fillCircle(cx, dotsY, RPICK_DOT_R, UI_INK);
      else gfx->drawCircle(cx, dotsY, RPICK_DOT_R, UI_TRACK_TEXT);
    }
  }
  if (mode != RPICK_FOR_START)            // first boot has nowhere to go back to
    uiText(UIF_SMALL, CX, forGyms ? 436 : 406, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

// Returns the region tapped, or -1. Takes the mode because the row on screen is
// an offset into the current page, not the region index -- and because a region
// whose sprite pack is missing must not be selectable, which is the whole point
// of the gate. The chooser still SHOWS it, greyed, saying why.
static int regionPickTap(int16_t x, int16_t y, uint8_t mode) {
  if (x < RPICK_X || x > RPICK_X + RPICK_W) return -1;
  uint8_t nreg = rpickRegions(mode);
  for (uint8_t row = 0; row < RPICK_PER_PAGE; row++) {
    uint8_t i = (uint8_t)(rpickPage * RPICK_PER_PAGE + row);
    if (i >= nreg) break;
    if (y >= RPICK_Y(row) && y <= RPICK_Y(row) + RPICK_H) {
      if (mode != RPICK_FOR_GYMS && !regionAvailable(i)) {
        sfxPlay(SFX_DENY);      // locked: say no out loud rather than do nothing
        return -1;
      }
      return i;
    }
  }
  return -1;
}

// ---------- level-up learn prompt ----------
void renderLearn() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uint8_t mv = pet.learnOffer();
  char head[40];
  const char *nm = pet.nick[0] ? pet.nick : DEX_TBL[pet.speciesId].name;
  snprintf(head, sizeof(head), T(S_LEARN_Q), nm);
  uiTextFit(UIF_SMALL, CX, 56, head, UI_INK, 1, 250);
  uiTextFit(UIF_BIG, CX, 90, MOVE_TBL[mv].name, DEX_TBL[pet.speciesId].accent, 1, 250);

  for (int i = 0; i < MOVE_SLOTS; i++) drawMoveRow(LEARN_ROW_Y(i), pet.moves[i], false, pet.speciesId);

  gfx->fillRoundRect(70, LEARN_SKIP_Y, 326, 44, 12, UI_TRACK_TEXT);
  gfx->drawRoundRect(70, LEARN_SKIP_Y, 326, 44, 12, UI_INK);
  uiTextFit(UIF_SMALL, CX, LEARN_SKIP_Y + 29, T(S_LEARN_SKIP), UI_INK, 1, 300);
  gfx->flush();
}

// page 2: medals with a descriptive label
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  uiTextFit(UIF_BIG, CX, 70, head, UI_INK, 1, 220);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 28 + (i % 2) * 206, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK_TEXT);
    if (g) {  // earned marker
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      uiText(UIF_SMALL, x + 22, y + 29, "v", UI_BAR_OK, 1);
    }
    uiTextFit(UIF_SMALL, x + 44, y + 29, medalDesc(i), g ? UI_BG_DAY : 0x8410, 0, 148);
  }
}

// page 3: progress (level, evolution, mistakes) -- brings to light mechanics
// that used to be invisible (how long until levelling up/evolving and why)
void renderCardProgress() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  uiTextFit(UIF_BIG, CX, 66, T(S_PROGRESS), UI_INK, 1, 220);

  // big level
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  uiText(UIF_HUGE, CX, 140, lv, UI_INK, 1);

  // progress bar to the next level (1 level = 60 min of play)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK_TEXT);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  uiTextFit(UIF_SMALL, CX, by + 46, nx, UI_INK, 1, 300);

  // evolution status
  uiTextFit(UIF_SMALL, CX, 244, T(S_EVO_LABEL), UI_TRACK_TEXT, 1, 300);
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (d.evolvesTo == 0) {
    evo = T(S_FINAL_FORM);
  } else {
    // The SAME sum canEvolveNow() uses -- including the day owed for retiring
    // the previous creature early. A card that left evoPenalty() out would
    // promise an evolution that then does not happen.
    int needed = d.evolveLevel + pet.careMistakes + pet.evoPenalty();
    if (pet.level() >= needed) {
      if (pet.lowestStat() >= 40) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  uiTextFit(UIF_SMALL, CX, 270, evo, evoCol, 1, 300);

  // the day inherited from an early retire, said out loud -- otherwise this
  // creature simply evolves late and the player has no way to know why
  if (pet.evoPenalty()) {
    uiTextFit(UIF_TINY, CX, 296, T(S_EVO_SLOW), UI_BAR_WARN, 1, 330);
  }

  // mistakes (they delay evolution)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  uiTextFit(UIF_SMALL, CX, 326, ms, pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK, 1, 300);
}

void renderCard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardStats();
  else if (cardPage == 2) renderCardMoves();
  else renderCardProgress();

  // page indicator + help
  for (int i = 0; i < CARD_PAGES; i++) {
    int dx = CX - (CARD_PAGES - 1) * 13 + i * 26;
    if (i == cardPage) gfx->fillCircle(dx, 394, 5, UI_INK);
    else gfx->drawCircle(dx, 394, 4, UI_INK);
  }
  uiText(UIF_SMALL, CX, 430, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

// ---------- menu overlay ----------

// Row labels are built fresh each frame because two of them carry live counts.
static void menuRowLabel(int i, char *out, size_t n) {
  switch (i) {
    case 0: snprintf(out, n, "%s", T(S_STATS)); break;
    case 1: snprintf(out, n, T(S_POKEDEX_FMT), pet.registeredCount(), DEX_COUNT); break;
    case 2: snprintf(out, n, "%s", T(S_SETTINGS)); break;
    case 3: snprintf(out, n, "%s", T(S_RETIRE)); break;
    default: snprintf(out, n, "%s", T(S_CLOSE)); break;
  }
}

void drawMenu() {
  // dim the game behind the panel so the overlay reads as modal, and so it is
  // obvious that tapping the darkened area is a way out
  for (int y = 0; y < 466; y += 2)
    gfx->drawFastHLine(0, y, 466, gNight ? 0x0000 : 0x2104);

  gfx->fillRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 18, UI_WHITE);
  gfx->drawRoundRect(MENU_X, MENU_Y, MENU_W, MENU_H, 18, UI_INK);

  char lbls[MENU_ROWS][28];
  const char *lp[MENU_ROWS]; int lw[MENU_ROWS];
  for (int i = 0; i < MENU_ROWS; i++) {
    menuRowLabel(i, lbls[i], sizeof(lbls[i]));
    lp[i] = lbls[i]; lw[i] = MENU_W - 60;
  }
  const UiFont &rowFont = uiFitAll(UIF_SMALL, lp, lw, MENU_ROWS);   // one size for every row
  for (int i = 0; i < MENU_ROWS; i++) {
    int y = MENU_ROW_Y(i);
    bool close = (i == MENU_ROWS - 1);
    bool dead = (i == 3 && !pet.canRetireNow());   // an egg or a companion
    bool retire = (i == 3 && !dead);               // destructive: red, white text
    gfx->fillRoundRect(MENU_X + 18, y, MENU_W - 36, MENU_ROW_H, 12,
                       retire ? UI_BAR_BAD : close || dead ? UI_TRACK_TEXT : UI_BG_DAY);
    gfx->drawRoundRect(MENU_X + 18, y, MENU_W - 36, MENU_ROW_H, 12, UI_INK);
    uiText(rowFont, CX, uiMidY(rowFont, y, MENU_ROW_H), lbls[i], retire ? UI_WHITE : UI_INK, 1);
  }
}

// ---------- training submenu (5th icon) ----------

// Bars here show progress toward the IV-capped ceiling, not a raw stat: 100%
// means this individual cannot train the stat any higher, which is the whole
// point of trMaxFor() gating training by IV.
static uint8_t trainPct(uint8_t cur, uint8_t cap) {
  return cap ? (uint8_t)((uint16_t)cur * 100 / cap) : 0;
}

void renderTrain() {
  for (int y = 0; y < 466; y += 2)
    gfx->drawFastHLine(0, y, 466, gNight ? 0x0000 : 0x2104);

  gfx->fillRoundRect(TRAIN_X, TRAIN_Y, TRAIN_W, TRAIN_H, 18, UI_WHITE);
  gfx->drawRoundRect(TRAIN_X, TRAIN_Y, TRAIN_W, TRAIN_H, 18, UI_INK);

  uiTextFit(UIF_MID, CX, TRAIN_Y + 38, T(S_TRAIN), UI_INK, 1, TRAIN_W - 60);

  const char *lbl[3] = { T(S_TR_ATK), T(S_TR_SPE), T(S_TR_DEF) };
  uint8_t cur[3] = { pet.trAtk, pet.trSpe, pet.trDef };
  uint8_t cap[3] = { pet.trMaxAtk(), pet.trMaxSpe(), pet.trMaxDef() };

  for (int i = 0; i < 3; i++) {
    int y = TRAIN_ROW_Y(i);
    bool passive = false;      // every row opens a game now, DEF included
    gfx->fillRoundRect(TRAIN_X + 18, y, TRAIN_W - 36, TRAIN_ROW_H, 12,
                       passive ? UI_TRACK_TEXT : UI_BG_DAY);
    gfx->drawRoundRect(TRAIN_X + 18, y, TRAIN_W - 36, TRAIN_ROW_H, 12, UI_INK);
    uiTextFit(UIF_SMALL, TRAIN_X + 32, y + 26, lbl[i], UI_INK, 0, TRAIN_W - 64);

    uint8_t pct = trainPct(cur[i], cap[i]);
    int bx = TRAIN_X + 32, bw = TRAIN_W - 64, bh = 12, by = y + 34;
    gfx->fillRoundRect(bx, by, bw, bh, 4, UI_TRACK_TEXT);
    int fw = (bw - 4) * pct / 100;
    if (fw > 0)
      gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 3, pct >= 100 ? UI_BAR_OK : UI_BAR_WARN);
  }

  uiTextFit(UIF_TINY, CX, TRAIN_Y + TRAIN_H - 12, T(S_TR_DEF_HINT), UI_INK, 1, TRAIN_W - 24);
  gfx->flush();   // without this the panel never updates and the screen freezes
}

// ---------- the box ----------
// Storage past the six that fight. A creature is moved by picking a party slot
// and then a box slot, which swaps them -- so one gesture covers deposit,
// withdraw and exchange rather than needing three.
void renderBox() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  char head[32];
  snprintf(head, sizeof(head), T(S_BOX_FMT), party.boxCount(), BOX_SLOTS);
  uiTextFit(UIF_SMALL, CX, 54, head, UI_INK, 1, 260);
  if (boxSwapFrom) {
    const PartyMon &p = party.slots[boxSwapFrom - 1];
    char sub[40];
    snprintf(sub, sizeof(sub), T(S_BOX_SWAP),
             p.empty() ? "-" : (p.nick[0] ? p.nick : DEX_TBL[p.dex].name));
    uiTextFit(UIF_TINY, CX, 76, sub, UI_BAR_WARN, 1, 300);
  }
  for (uint8_t i = 0; i < BOX_PER_PAGE; i++) {
    uint8_t idx = boxPage * BOX_PER_PAGE + i;
    if (idx >= BOX_SLOTS) break;
    const PartyMon &m = party.box[idx];
    int x = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int y = 88 + (i / 2) * (PARTY_CELL_H + 8);
    gfx->fillRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10,
                       m.empty() ? UI_TRACK_TEXT : UI_WHITE);
    gfx->drawRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10, UI_INK);
    if (m.empty()) {
      uiTextFit(UIF_SMALL, x + PARTY_CELL_W / 2, y + PARTY_CELL_H / 2 + 7,
                T(S_PARTY_EMPTY), 0x8410, 1, PARTY_CELL_W - 8);
      continue;
    }
    const uint8_t *th = thumbs.get(m.dex);
    if (th) drawThumb(th, x - 14, y - 4, 2, false);
    uiTextFit(UIF_SMALL, x + 52, y + 28, m.nick[0] ? m.nick : DEX_TBL[m.dex].name,
              UI_INK, 0, PARTY_CELL_W - 58);
    char l[16];
    snprintf(l, sizeof(l), "Lv.%u%s", (unsigned)m.level, m.shiny ? " *" : "");
    uiText(UIF_TINY, x + 56, y + 52, l, m.shiny ? UI_BAR_WARN : UI_INK, 0);
  }
  uint8_t pages = BOX_SLOTS / BOX_PER_PAGE;
  for (uint8_t i = 0; i < pages; i++) {
    int dx = CX - (pages - 1) * 13 + i * 26;
    if (i == boxPage) gfx->fillCircle(dx, 366, 5, UI_INK);
    else gfx->drawCircle(dx, 366, 4, UI_INK);
  }
  uiText(UIF_SMALL, CX, 406, T(S_BACK), UI_TRACK_TEXT, 1);
  gfx->flush();
}

void boxTap(int16_t x, int16_t y) {
  // The sheet is checked first and is modal in the same way the party's is.
  if (boxDetail) {
    if (releaseConfirm) { monSheetConfirmTap(x, y, true); return; }
    if (monSheetBtn(x, y, true)) {          // TO PARTY
      int free = party.firstFree();
      if (free < 0) { sfxPlay(SFX_DENY); return; }
      party.swapPartyBox((uint8_t)free, boxDetail - 1);
      boxDetail = 0;
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    if (monSheetBtn(x, y, false)) {         // RELEASE -- ask first, always
      releaseConfirm = true;
      sfxPlay(SFX_TAP);
      return;
    }
    boxDetail = 0;                          // anywhere else backs out
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  for (uint8_t i = 0; i < BOX_PER_PAGE; i++) {
    uint8_t idx = boxPage * BOX_PER_PAGE + i;
    if (idx >= BOX_SLOTS) break;
    int cx0 = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int cy0 = 88 + (i / 2) * (PARTY_CELL_H + 8);
    if (x < cx0 || x > cx0 + PARTY_CELL_W || y < cy0 || y > cy0 + PARTY_CELL_H) continue;
    if (boxSwapFrom) {           // a party slot is waiting: complete the trade
      if (party.slots[boxSwapFrom - 1].empty() && party.box[idx].empty()) {
        sfxPlay(SFX_DENY);
        return;
      }
      party.swapPartyBox(boxSwapFrom - 1, idx);
      boxSwapFrom = 0;
      boxSel = 0;
      sfxPlay(SFX_MEDAL);
      return;
    }
    // Otherwise open its SHEET. It used to go straight to the party, which is a
    // lot to happen from one tap and left nowhere to put RELEASE; the sheet
    // offers TO PARTY explicitly and shows what you are about to move.
    if (party.box[idx].empty()) { sfxPlay(SFX_DENY); return; }
    if (party.firstFree() < 0) {
      boxSel = idx + 1;          // party is full: go choose who steps out
      boxOpen = false;
      sfxPlay(SFX_TAP);
      return;
    }
    boxDetail = idx + 1;
    releaseConfirm = false;
    sfxPlay(SFX_TAP);
    return;
  }
  boxOpen = false;               // anywhere else backs out
  boxSwapFrom = 0;
  boxDetail = 0;
  releaseConfirm = false;
}

// ---------- party ----------

void drawPartySlot(int i, int x, int y) {
  const PartyMon &m = party.slots[i];
  gfx->fillRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10,
                     m.empty() ? UI_TRACK_TEXT : UI_WHITE);
  gfx->drawRoundRect(x, y, PARTY_CELL_W, PARTY_CELL_H, 10, UI_INK);
  if (m.empty()) {
    uiTextFit(UIF_SMALL, x + PARTY_CELL_W / 2, y + PARTY_CELL_H / 2 + 7,
              T(S_PARTY_EMPTY), 0x8410, 1, PARTY_CELL_W - 8);
    return;
  }
  const uint8_t *th = thumbs.get(m.dex);
  if (th) drawThumb(th, x - 6, y - 3, 1, false);
  const DexEntry &d = DEX_TBL[m.dex];
  const char *nm = m.nick[0] ? m.nick : d.name;
  uiTextFit(UIF_SMALL, x + 60, y + 28, nm, d.accent, 0, PARTY_CELL_W - 60 - 6);
  char lv[12];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), (unsigned)m.level);
  uiText(UIF_SMALL, x + 62, y + 50, lv, UI_INK, 0);
  if (m.shiny) uiText(UIF_SMALL, x + 62 + uiTextWidth(UIF_SMALL, lv) + 6, y + 50, "*", UI_BAR_WARN, 0);
}

void renderParty() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);

  char head[24];
  snprintf(head, sizeof(head), T(S_PARTY_FMT), party.count());
  uiTextFit(UIF_BIG, CX, 63, head, UI_INK, 1, 250);

  // the box lives behind this button; it also shows how full it is, so the
  // player knows there is anything in there without opening it
  if (!partyPick) {
    char bl[24];
    snprintf(bl, sizeof(bl), T(S_BOX_FMT), party.boxCount(), BOX_SLOTS);
    bool armed = boxSwapFrom != 0;
    gfx->fillRoundRect(BOXBTN_X, BOXBTN_Y, BOXBTN_W, BOXBTN_H, 10,
                       armed ? UI_BAR_WARN : UI_BG_DAY);
    gfx->drawRoundRect(BOXBTN_X, BOXBTN_Y, BOXBTN_W, BOXBTN_H, 10, UI_INK);
    uiTextFit(UIF_SMALL, 233, BOXBTN_Y + 26, bl, UI_INK, 1, BOXBTN_W - 12);
  }

  if (boxSel) {
    const PartyMon &b = party.box[boxSel - 1];
    char sw[44];
    snprintf(sw, sizeof(sw), T(S_BOX_SWAP),
             b.nick[0] ? b.nick : DEX_TBL[b.dex].name);
    uiTextFit(UIF_TINY, CX, 80, sw, UI_BAR_WARN, 1, 300);
  }

  // when a newcomer is waiting, say so instead of the usual hint
  if (partyPick) {
    uiTextFit(UIF_TINY, CX, 82, T(S_PARTY_FULL), UI_BAR_BAD, 1, 300);
  }

  for (int i = 0; i < PARTY_SLOTS; i++) {
    int x = PARTY_GRID_X + (i % 2) * (PARTY_CELL_W + 10);
    int y = PARTY_GRID_Y + (i / 2) * (PARTY_CELL_H + 8);
    drawPartySlot(i, x, y);
  }

  // exit: an explicit button, always in the same place
  const char *ex = partyPick ? T(S_PARTY_LETGO) : T(S_CLOSE);
  gfx->fillRoundRect(PARTYCLOSE_X, PARTYCLOSE_Y, PARTYCLOSE_W, PARTYCLOSE_H, 12,
                     partyPick ? UI_BAR_BAD : UI_TRACK_TEXT);
  gfx->drawRoundRect(PARTYCLOSE_X, PARTYCLOSE_Y, PARTYCLOSE_W, PARTYCLOSE_H, 12, UI_INK);
  uiTextFit(UIF_SMALL, CX, PARTYCLOSE_Y + 28, ex, partyPick ? UI_WHITE : UI_INK, 1,
            PARTYCLOSE_W - 12);
  gfx->flush();
}

// ---------- keyboard for renaming ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

// The keyboard is shared, so it has to be told what it is naming. It used to
// hardcode pet.rename() on commit, which is why a second caller needed this.
void openKeyboardFor(uint8_t target) {
  kbTarget = target;
  kbOpen = true;
  const char *cur = (target == KB_TRAINER) ? pet.trainerName : pet.nick;
  strncpy(nameBuf, cur, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
}
void openKeyboard() { openKeyboardFor(KB_PET); }

void renderKeyboard() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  uiTextFit(UIF_SMALL, CX, 70, T(S_NAME), UI_INK, 1, 240);
  // current buffer
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  uiTextFit(UIF_BIG, 95, 112, nameLen ? nameBuf : "_", UI_INK, 0, 276);

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    char kl[3] = { 0, 0, 0 };
    if (i < 28) kl[0] = KB_KEYS[i];
    else strcpy(kl, (i == 28) ? "<-" : "OK");
    uiText(UIF_SMALL, x + (KB_W - 6) / 2, y + (KB_H - 6) / 2 + 8, kl, UI_INK, 1);
  }
  gfx->flush();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  if (i == 28) {  // delete
    if (nameLen) nameBuf[--nameLen] = 0;
  } else if (i == 29) {  // OK
    if (kbTarget == KB_TRAINER) pet.renameTrainer(nameBuf);
    else pet.rename(nameBuf);
    kbOpen = false;
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
  }
}

// ---------- pokedex gallery ----------

#define GAL_X 73
#define GAL_Y 84
#define GAL_CELL 80

// draws a thumbnail centred in its cell; sil=true paints it in ink
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

void renderGallery() {
  if (galleryDetail) {  // detail view: always redrawn (animated)
    gfx->fillScreen(RGB565_BLACK);
    gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    char head[24];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", reg ? d.name : "???");
    uiTextFit(UIF_BIG, CX, 77, head, reg ? d.accent : UI_INK, 1, 260);  // long names step down
    if (galleryPmd.loaded) {
      // animated and in colour if registered; static silhouette if not ("?" style)
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, reg ? millis() : 0, true, !reg, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !reg);
    }
    uiText(UIF_SMALL, CX, 422, T(S_DETAIL_BACK), UI_INK, 1);
    gfx->flush();
    return;
  }

  if (!galleryDirty) return;  // the grid is static
  galleryDirty = false;

  gfx->fillScreen(RGB565_BLACK);
  gfx->fillCircle(CX, CY, 231, UI_BG_DAY);
  // the region's own name and its own tally: "how much of Johto have I seen"
  // is the question you are actually asking here
  char head[32];
  const RegionInfo &grg = REGIONS[galleryRegion % GAL_REGIONS];
  snprintf(head, sizeof(head), "%s %u/%u", grg.name,
           pet.registeredCountIn(grg.lo, grg.hi), (unsigned)GAL_SPAN);
  uiTextFit(UIF_BIG, CX, 68, head, UI_INK, 1, 250);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = GAL_LO + galleryPage * GAL_PER_PAGE + r * 4 + c;
      if (dex > GAL_HI || dex > DEX_COUNT) break;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        drawThumb(t, x, y, 2, !pet.isRegistered(dex));
        if (pet.isShinyRegistered(dex)) {
          uiText(UIF_SMALL, x + 62, y + 20, "*", UI_BAR_WARN, 0);
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        uiText(UIF_SMALL, x + 40, y + 46, num, UI_TRACK_TEXT, 1);
      }
    }
  }
  // A page number, not a row of dots. 25 dots do not fit across the bottom of
  // a round panel -- the chord at that height is only ~228 px -- and counting
  // them to find where you are is worse than reading the number.
  char pg[12];
  snprintf(pg, sizeof(pg), "%d/%d", galleryPage + 1, (int)GAL_PAGES);
  uiText(UIF_SMALL, CX, 442, pg, UI_TRACK_TEXT, 1);
  gfx->flush();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {  // back to the grid
    galleryDetail = 0;
    galleryPmd.unload();
    galleryDirty = true;
    return;
  }
  if (y < 72) {  // tap the header = quit
    galleryOpen = false;
    galleryPmd.unload();
    return;
  }
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  int16_t dex = GAL_LO + galleryPage * GAL_PER_PAGE + r * 4 + c;
  if (dex > GAL_HI || dex > DEX_COUNT) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
}

// The RTC chip counts live; this just reads it (an I2C transaction) at most
// every 250 ms, since the main screen redraws at 10 fps. Minutes are the finest
// the header shows, so a 250 ms cache can lag a minute rollover by a fraction
// of a second at most.
static const char *headerTimeText(char *t, size_t n) {
  static uint32_t cachedE = 0, cachedAt = 0;
  uint32_t ms = millis();
  if (!cachedAt || ms - cachedAt >= 250) { cachedE = rtcEpoch(); cachedAt = ms ? ms : 1; }
  if (!cachedE) return nullptr;   // RTC not valid: show nothing rather than a wrong time
  int h = (cachedE / 3600) % 24;
  snprintf(t, n, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12,
           (int)((cachedE / 60) % 60), h < 12 ? "AM" : "PM");
  return t;
}

// The time and the battery share one row, centred as a group, so neither
// needs its own band under the other.
void drawBattery() {
  int pc = batPercent();
  char tbuf[16];
  const char *ts = headerTimeText(tbuf, sizeof(tbuf));
  const int GAP = 8, BW = 27;       // battery body 24 + 3 terminal
  int tw = ts ? uiTextWidth(UIF_SMALL, ts) : 0;
  int total = tw + (ts && pc >= 0 ? GAP : 0) + (pc >= 0 ? BW : 0);
  if (!total) return;               // no clock and no battery
  int x0 = CX - total / 2;
  if (ts) {
    uiText(UIF_SMALL, x0, 19 + 14, ts, inkColor(), 0);
  }
  if (pc < 0) return;               // no battery connected
  int x = x0 + tw + (ts ? GAP : 0), y = 22, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // charging bolt (zigzag) instead of the level bar
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

// Edge hints for the two horizontal swipes off the main screen: right opens the
// party (pokeball), left opens the gym ladder (badge). Shown only while the
// swipe is actually allowed -- onSwipe() refuses it during a ceremony, a
// pending confirm and the starter choice -- and nudged back and forth so they
// read as a gesture rather than decoration.
static void drawSwipeHints() {
  if (pet.ceremony || confirmUntil || pet.awaitingStarter()) return;
  const int cy = 233;
  const int nudge = ((millis() / 500) % 2) ? 3 : 0;
  uint16_t ink = inkColor();
  int lx = 12 + nudge;                       // party: swipe RIGHT, chevron points right
  for (int k = 0; k < 2; k++) {              // two passes = a 2 px stroke
    gfx->drawLine(lx + k, cy - 10, lx + 8 + k, cy, ink);
    gfx->drawLine(lx + 8 + k, cy, lx + k, cy + 10, ink);
  }
  gfx->fillCircle(lx + 6, cy + 24, 7, UI_WHITE);      // pokeball
  gfx->drawCircle(lx + 6, cy + 24, 7, ink);
  gfx->drawFastHLine(lx - 1, cy + 24, 14, ink);
  gfx->fillCircle(lx + 6, cy + 24, 2, ink);
  int rx = 454 - nudge;                      // gym: swipe LEFT, chevron points left
  for (int k = 0; k < 2; k++) {
    gfx->drawLine(rx - k, cy - 10, rx - 8 - k, cy, ink);
    gfx->drawLine(rx - 8 - k, cy, rx - k, cy + 10, ink);
  }
  int bx = rx - 6, by = cy + 24;                      // badge
  gfx->fillTriangle(bx, by - 8, bx - 7, by, bx + 7, by, UI_BAR_WARN);
  gfx->fillTriangle(bx, by + 8, bx - 7, by, bx + 7, by, UI_BAR_WARN);
  gfx->drawLine(bx, by - 8, bx + 7, by, ink);
  gfx->drawLine(bx + 7, by, bx, by + 8, ink);
  gfx->drawLine(bx, by + 8, bx - 7, by, ink);
  gfx->drawLine(bx - 7, by, bx, by - 8, ink);
}

// Menu icon beside the clock: the name/status band is a button, and nothing
// said so. Right of the time+battery group (at most ~131 px wide, centred).
static void drawMenuHint() {
  if (pet.ceremony || confirmUntil || pet.awaitingStarter()) return;
  const int x = CX + 78, y = 22;
  for (int i = 0; i < 3; i++) gfx->fillRoundRect(x, y + i * 6, 18, 3, 1, inkColor());
}

// Swipe UP opens the creature's card. Drawn AFTER the lower panel (which paints
// over everything below y 312) and only where onSwipeV() allows it: a creature,
// not an egg, with no dialog or food picker open. Nudges upward like the side hints.
void drawUpHint() {
  if (pet.isEgg() || pet.ceremony || confirmUntil || feedMenuUntil || pet.awaitingStarter()) return;
  const int cx = CX, cy = 448 - (((millis() / 500) % 2) ? 3 : 0);
  uint16_t ink = inkColor();
  for (int k = 0; k < 2; k++) {
    gfx->drawLine(cx - 10, cy + 8 + k, cx, cy + k, ink);
    gfx->drawLine(cx, cy + k, cx + 10, cy + 8 + k, ink);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  drawSwipeHints();
  drawMenuHint();
  uiTextFit(UIF_BIG, CX, 48 + 21, name, nameColor, 1, 270);
  uiTextFit(UIF_SMALL, CX, 84 + 14, msg, inkColor(), 1, 320);
}

// ceremony animation (10s): farewell = bow with hearts and it
// walks away; runaway = it gets scared and runs off. Replaces the idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // fallback if there is no PMD sprite
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 over the 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // sad ending: bluish gloom + rain
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // head hung low, trembling
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // walks away slowly and fades out
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // blinks toward the silhouette
    }
    drawPmdActZ(pmd, act, x, y, now, true, fade, 5, PET_ZOOM, PET_MAX_PX);  // fade=silhouette: dissolves as it leaves
    // tear falling from the creature
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // epic farewell: pulsing golden halo + sparks and hearts that rise
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // rise and reappear at the bottom
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // little heart
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // bow / farewell pose
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // walks away to the right
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdActZ(pmd, act, x, y, now, true, false, 5, PET_ZOOM, PET_MAX_PX);
  if (pet.showHeart())                     // big heart following the creature
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// decision dialog (2 stacked buttons): evolve/keep or farewell/stay together
// The two confirm buttons' hit areas, so a test can hold them to UI_TAP_MIN and
// prove they do not overlap without restating the numbers.
void uiConfirmRects(int *b1Top, int *b1Bot, int *b2Top, int *b2Bot) {
  *b1Top = CONFIRM_B1_Y;
  *b1Bot = CONFIRM_B1_Y + CONFIRM_BTN_H;
  *b2Top = CONFIRM_B2_Y;
  *b2Bot = CONFIRM_B2_Y + CONFIRM_BTN_H;
}

// Draws the panel and its two buttons. sub1/sub2 are optional lines between the
// question and the buttons -- what the choice COSTS, which for anything
// irreversible has to be on screen before the tap, not after it.
void drawConfirmPanel(const char *q, const char *sub1, const char *sub2,
                      uint16_t subCol, const char *o1, uint16_t c1, uint16_t t1,
                      const char *o2, uint16_t c2, uint16_t t2) {
  gfx->fillRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, 16, UI_WHITE);
  gfx->drawRoundRect(CONFIRM_X, CONFIRM_Y, CONFIRM_W, CONFIRM_H, 16, UI_INK);
  uiTextFit(UIF_SMALL, CX, (sub1 || sub2) ? 158 : 176 + 14, q, UI_INK, 1, CONFIRM_W - 24);
  if (sub1) uiTextFit(UIF_TINY, CX, sub2 ? 181 : 190, sub1, subCol, 1, CONFIRM_W - 20);
  if (sub2) uiTextFit(UIF_TINY, CX, 200, sub2, subCol, 1, CONFIRM_W - 20);
  gfx->fillRoundRect(CONFIRM_BTN_X, CONFIRM_B1_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 12, c1);
  uiTextFit(UIF_SMALL, CX, CONFIRM_B1_Y + 18 + 14, o1, t1, 1, CONFIRM_BTN_W - 20);
  gfx->fillRoundRect(CONFIRM_BTN_X, CONFIRM_B2_Y, CONFIRM_BTN_W, CONFIRM_BTN_H, 12, c2);
  uiTextFit(UIF_SMALL, CX, CONFIRM_B2_Y + 18 + 14, o2, t2, 1, CONFIRM_BTN_W - 20);
}

void drawChoiceDialog() {
  const char *q, *o1, *o2;
  const char *sub1 = nullptr, *sub2 = nullptr;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolution
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK_TEXT; t2 = UI_INK;
  } else if (choiceKind == 3) {   // retirement on request
    q = T(S_RETIRE_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
    // The price, spelled out, and only when there is one: retiring a creature
    // that has already earned its farewell costs nothing and must not claim to.
    // An EARLY one now costs TWO things and says both -- the creature is not
    // banked at all, which is the half a player would not otherwise discover
    // until the party screen came up empty.
    if (!pet.retireIsFree()) { sub1 = T(S_RETIRE_COST); sub2 = T(S_RETIRE_GONE); }
  } else {                // farewell
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  drawConfirmPanel(q, sub1, sub2, UI_BAR_BAD, o1, c1, t1, o2, c2, t2);
}

// big red CTA button for evolving (pulses to draw attention)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  const char *t = T(S_EVO_TAP);
  uiTextFit(UIF_BIG, CX, y + h / 2 - 11 + 21, t, UI_WHITE, 1, EVO_BTN_W - 24);
}

// golden farewell CTA button: "<name> wants to tell you something..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : DEX_TBL[pet.speciesId].name;
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  uiTextFit(UIF_SMALL, CX, y + h / 2 - 8 + 14, buf, UI_INK, 1, FAR_BTN_W - 24);
}

// gloomy runaway CTA button from neglect: "<name> feels abandoned..."
// (sad ending: dark blue-grey, slow muted heartbeat)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : DEX_TBL[pet.speciesId].name;
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  uiTextFit(UIF_SMALL, CX, y + h / 2 - 8 + 14, buf, C565(0xc8, 0xd2, 0xe0), 1, FAR_BTN_W - 24);
}

// epic evolution animation: radial halo + spinning rays + sprite blink
// accelerating + sparks shooting out + final flash
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // radial halo that grows and pulses
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // spinning rays from the centre of the creature
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // blink between the PREVIOUS and the NEW form (silhouettes), accelerating; at the
  // end (t>0.9) it stays fixed on the new one for the reveal flash
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActZ(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5, PET_ZOOM, PET_MAX_PX);
  else drawPmdActZ(pmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5, PET_ZOOM, PET_MAX_PX);
  // sparks shooting out
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // final flash before revealing the new form
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // no SD and no flash sprite: clear notice that sprites are missing
    gfx->setTextColor(inkColor());
    gfx->setTextSize(6);
    gfx->setCursor(CX - 18, PET_CY - 80);
    gfx->print("?");
    uiTextFit(UIF_SMALL, CX, PET_CY + 10, T(S_NO_SPRITES), inkColor(), 1, 360);
    uiTextFit(UIF_SMALL, CX, PET_CY + 38, T(S_LOAD_SPRITES), inkColor(), 1, 360);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // evolution animation: alternates the silhouette of the previous form and the new one
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // little hop

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // overlaid expressions using the species' anchors
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawHeartFloat(x + 16 * s, y);
}

// ---------- bath scene ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  uint32_t now = millis();
  if (now > bathUntil) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // joy pose once clean
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  // A bubble is a clear sphere: only the rim is drawn (the creature shows
  // through), with a bright glint at the upper left and a faint one opposite.
  // The old white disc with a dark dot read as an eyeball.
  const uint16_t rim = C565(0x8f, 0xd0, 0xf0), rimIn = C565(0xdf, 0xf4, 0xff);
  float t = now / 220.0f;
  if (left > 800) {
    // foam: bubbles swaying and slowly rising
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->drawCircle(bx, by, b.r, rim);
      gfx->drawCircle(bx, by, b.r - 1, rimIn);
      for (float a = 3.45f; a < 4.35f; a += 0.13f)         // glint arc, upper left
        gfx->fillRect(bx + (int)(cosf(a) * b.r * 0.62f), by + (int)(sinf(a) * b.r * 0.62f), 2, 2, UI_WHITE);
      gfx->fillCircle(bx - b.r * 45 / 100, by - b.r * 45 / 100, b.r / 7 + 1, UI_WHITE);
      gfx->fillRect(bx + b.r * 35 / 100, by + b.r * 35 / 100, 2, 2, rimIn);   // faint opposite glint
    }
  } else {
    // pop: each ring swells and thins out while droplets fly off it
    float p = (800 - (int)left) / 800.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - 24;                                   // where the rise ended
      if (p < 0.5f) gfx->drawCircle(bx, by, b.r + (int)(b.r * p * 0.8f), rim);
      for (int k = 0; k < 6; k++) {
        float a = k * 1.0472f + b.ph * 0.1f;
        float d = b.r * (0.9f + p * 1.6f);
        gfx->fillRect(bx + (int)(cosf(a) * d), by + (int)(sinf(a) * d), 2, 2, (k & 1) ? rimIn : UI_WHITE);
      }
    }
  }
}

// ---------- PMD pet: behaviour ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// draws an action anchored by its base (centre-x, ground) and returns its scale
// draws an action of a specific PmdMon (m); drawPmdAct uses the global pmd.
// zoom100 scales on top of the integer base scale (100 = unchanged, 150 = 1.5x).
// Pixel edges are floored from the fractional position, so a fractional zoom
// leaves no gaps between neighbouring pixels. maxPx caps the drawn height when
// zoomed, so a tall action cannot run up into the header.
void drawPmdActZ(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil,
                 uint8_t maxS, uint16_t zoom100, uint16_t maxPx) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // actions with a large frame (attack)
  uint32_t sc = (uint32_t)s * zoom100;                 // scale x100
  if (zoom100 > 100 && a.h * sc > (uint32_t)maxPx * 100) sc = (uint32_t)maxPx * 100 / a.h;
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anchor by the feet (a.base), not by canvas height: that way actions
  // with different padding (Hurt, Eat...) all end up at the same ground height
  int x0 = cx - (int)(a.w * sc / 200), y0 = groundY - (int)((a.base ? a.base : a.h) * sc / 100);
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    int ya = y0 + (int)(r * sc / 100), yb = y0 + (int)((r + 1) * sc / 100);
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      int xa = x0 + (int)(c * sc / 100), xb = x0 + (int)((c + 1) * sc / 100);
      gfx->fillRect(xa, ya, xb - xa, yb - ya, sil ? INK_K : m.pal[idx]);
    }
  }
}
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActZ(m, actId, cx, groundY, t, loop, sil, maxS, 100, 0);
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// picks the creature's next whim when it is content
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // walk
    beh.targetX = 180 + random(106);   // narrower: the creature is wider now
    beh.until = now + 15000;
  } else if (r < 60) {
    // random gesture among the available ones
    // (Hop excluded: jumps too high; Sit excluded: looks backwards)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // look straight ahead
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // evolution finished: free the previous form

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // content: the planner decides (idle / walk / gesture)
    if (now > beh.until) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdActZ(pmd, act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5,
              PET_ZOOM, PET_MAX_PX);

  if (pet.showHeart()) {
    int hx, hy;
    petHeadAnchor(PMD_IDLE, hx, hy);
    drawHeartFloat(hx, hy);
  }
}

// animated sprite from the SD: integer zoom per pixel, frames at their own pace
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // little hop
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes instead of expressions (imported sprites have no anchors)
  if (pet.showHeart()) drawHeartFloat(x + w / 2, y);
}

// closed eye: erases the 3x4 eye and draws the eyelid
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// erases the base smile and paints an open mouth (eating) or a frown (sad)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    const int px = 36 + i * 46, py = 244;
    drawMap(SPR_POOP, 32, px, py, 2, false);
    // two flies circle each pile on out-of-phase, off-round orbits
    const uint32_t t = millis();
    for (int f = 0; f < 2; f++) {
      float a = t * (f ? -0.0071f : 0.0053f) + i * 2.1f + f * 3.0f;
      int fx = px + 32 + (int)(cosf(a) * (24 + f * 6));
      int fy = py + 24 + (int)(sinf(a * 1.7f) * (14 + f * 4));
      bool up = ((t / 70 + f * 3 + i) & 1) != 0;   // wing flutter
      gfx->fillRect(fx, fy, 3, 3, INK_K);
      gfx->fillRect(fx - 1, fy - (up ? 2 : 1), 2, 2, UI_WHITE);
      gfx->fillRect(fx + 2, fy - (up ? 2 : 1), 2, 2, UI_WHITE);
    }
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78, 340, T(S_BAR_ENE), pet.energy);       // 22 px under the top row, clear of the buttons
  drawBar(244, 340, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  uiTextFit(UIF_TINY, x, y + 12, label, inkColor(), 0, 46);
  int bx = x + 48, bw = 100, bh = 15;  // +48: leaves room for 4-letter labels (EN)
  uint16_t fill = (val >= 50) ? UI_BAR_OK_PALE : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK_TEXT);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  // Soft pastel tile per icon (indexed by BTN_*), a darker ring of the same hue
  // and a drop shadow, so the row reads as raised buttons.
  const uint16_t tint[BTN_COUNT]   = { C565(255, 230, 224), C565(255, 244, 204),
                                       C565(220, 238, 255), C565(226, 232, 248) };
  const uint16_t accent[BTN_COUNT] = { C565(232, 110, 96), C565(232, 180, 60),
                                       C565(90, 160, 220), C565(110, 128, 190) };
  for (int i = 0; i < BTN_COUNT; i++) {
    bool off = uiButtonDisabled(i);   // asleep only LIGHT works
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    int bs = 2 * BTN_HALF;
    if (!pet.sleeping) {
      gfx->fillRoundRect(bx + 1, by + 4, bs, bs, 14, gNight ? C565(8, 10, 20) : C565(150, 144, 128));
      gfx->fillRoundRect(bx, by, bs, bs, 14, tint[i]);
      gfx->fillRoundRect(bx + 4, by + 3, bs - 8, bs / 2 - 4, 10, UI_WHITE);   // gloss
      gfx->drawRoundRect(bx + 1, by + 1, bs - 2, bs - 2, 13, accent[i]);
    }
    gfx->drawRoundRect(bx, by, bs, bs, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 24, buttons[i].cy - 24, 3, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

// Where a creature's head is, for the Zs and hearts that leave it: x is the
// sprite's centre, y the top of the given action's frame, worked out the way
// drawPmdActZ() scales it. With no PMD sprite loaded it is a fixed guess.
void petHeadAnchor(uint8_t actId, int &hx, int &hy) {
  if (!pmd.loaded) { hx = CX + 30; hy = PET_CY - 40; return; }
  const PmdAct &a = pmd.has(actId) ? pmd.acts[actId] : pmd.acts[PMD_IDLE];
  uint8_t s = pmd.acts[PMD_IDLE].h ? 170 / pmd.acts[PMD_IDLE].h : 5;
  if (s < 2) s = 2;
  if (s > 5) s = 5;
  while (s > 2 && a.h * s > 250) s--;
  uint32_t sc = (uint32_t)s * PET_ZOOM;
  if (a.h * sc > (uint32_t)PET_MAX_PX * 100) sc = (uint32_t)PET_MAX_PX * 100 / (a.h ? a.h : 1);
  hx = (int)beh.x;
  hy = PET_GROUND - (int)((a.base ? a.base : a.h) * sc / 100) + 6;
}

// Snoring: Zs drift up and to the right from the creature's head, growing as
// they rise. Three are in flight at once, staggered, so there is always one
// leaving. Replaces a static "Zz" and the "Zzz..." status line.
void drawSnore() {
  uint32_t now = millis();
  int hx, hy;
  petHeadAnchor(PMD_SLEEP, hx, hy);
  const uint32_t PERIOD = 2700;
  for (int i = 0; i < 3; i++) {
    float p = ((now + i * (PERIOD / 3)) % PERIOD) / (float)PERIOD;   // 0 at the head, 1 gone
    int x = hx + (int)(p * 56) + (int)(sinf(p * 9.0f + i) * 5);
    int y = hy - (int)(p * 78);
    uint8_t sz = p < 0.34f ? 2 : (p < 0.67f ? 3 : 4);
    if (p > 0.92f) continue;                                    // gone before the top edge
    gfx->setTextSize(sz);
    gfx->setTextColor(INK_K);                                   // 1 px shadow so it reads on any sky
    gfx->setCursor(x + 1, y + 1);
    gfx->print("Z");
    gfx->setTextColor(UI_WHITE);
    gfx->setCursor(x, y);
    gfx->print("Z");
  }
}

// One heart of half-width 2r centred on x, drawn from two lobes and a point, with
// a dark rim so it reads on any background and a glint like the bubbles have.
static void drawHeartShape(int x, int y, int r) {
  const uint16_t rim = C565(0x5a, 0x10, 0x28), fill = C565(0xff, 0x5c, 0x7c);
  for (int pass = 0; pass < 2; pass++) {
    int rr = pass ? r : r + 1;
    uint16_t c = pass ? fill : rim;
    gfx->fillCircle(x - r, y, rr, c);
    gfx->fillCircle(x + r, y, rr, c);
    gfx->fillTriangle(x - 2 * r - (pass ? 0 : 1), y + r / 3, x + 2 * r + (pass ? 0 : 1), y + r / 3,
                      x, y + 3 * r + (pass ? 0 : 1), c);
  }
  gfx->fillCircle(x - r - r / 3, y - r / 3, r / 4 + 1, UI_WHITE);   // glint
}

// Affection: one heart floats up from the head, swelling slightly as it rises,
// over the whole HEART_MS the pet stays "pleased". The same idea as the snoring
// Zs, so the two read as one family.
void drawHeartFloat(int hx, int hy) {
  uint32_t age = HEART_MS - pet.heartLeftMs();           // ms since the heart began
  if (age >= HEART_MS) return;
  float p = age / (float)HEART_MS;                       // 0 at the head, 1 gone
  int x = hx + 10 + (int)(sinf(p * 6.0f) * 7);
  int y = hy - (int)(p * 66);
  drawHeartShape(x, y, 3 + (int)(p * 3));
}

const char *statusMsg() {
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // universal onomatopoeia
  if (pet.sleeping) return "";   // the Zs above its head say it
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// draws an n x n pixel map scaled; silhouette=true paints it in ink
// An 8bpp indexed avatar, same shape as the badge art: 0xFF is transparent.
void drawAvatar(uint8_t which, int x, int y, int s) {
  const AvatarArt &a = AVATARS[which % AVATAR_COUNT];
  for (int r = 0; r < AVATAR_PX; r++)
    for (int c = 0; c < AVATAR_PX; c++) {
      uint8_t v = a.idx[r * AVATAR_PX + c];
      if (v == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, a.pal[v]);
    }
}

// The food, held in front of the creature and eaten in EAT_BITES bites. Each
// bite takes a round chunk out of the icon, the food jolts and a few crumbs
// fly; the last bite takes what is left. Progress comes from the pet's own
// eat timer, so the animation can never outlive or undershoot the eating mood.
#define EAT_BITES 4
void drawEatFood() {
  if (!pet.eating()) return;
  static const char *const *const ICONS[4] = { SPR_ICON_FOOD, SPR_ICON_BERRY_B,
                                               SPR_ICON_BERRY_G, SPR_ICON_CANDY };
  const char *const *ic = ICONS[eatItem < 4 ? eatItem : 0];
  uint32_t el = EAT_ANIM_MS - pet.eatLeftMs();
  // bite k lands at BITE_AT[k] ms and the food jolts for 160 ms after it
  static const uint16_t BITE_AT[EAT_BITES] = { 350, 900, 1450, 2000 };
  // chunk taken by each bite: centre and radius, in icon pixels
  static const int8_t BITE_X[EAT_BITES] = { 13, 3, 12, 8 };
  static const int8_t BITE_Y[EAT_BITES] = { 5, 7, 12, 8 };
  static const uint8_t BITE_R[EAT_BITES] = { 5, 5, 6, 10 };
  uint8_t bites = 0;
  int jolt = 0;
  for (uint8_t k = 0; k < EAT_BITES; k++) {
    if (el >= BITE_AT[k]) {
      bites = k + 1;
      uint32_t since = el - BITE_AT[k];
      if (since < 160) jolt = (since < 80) ? 3 : 1;
    }
  }
  if (bites >= EAT_BITES) return;   // the last bite finished it
  int cx = pmd.loaded ? (int)beh.x : CX;
  const int S = 4, x0 = cx - 8 * S, y0 = PET_GROUND - 88 + jolt;
  for (int r = 0; r < 16; r++)
    for (int c = 0; c < 16; c++) {
      char ch = ic[r][c];
      if (ch == '.') continue;
      bool gone = false;
      for (uint8_t k = 0; k < bites && !gone; k++) {
        int dx = c - BITE_X[k], dy = r - BITE_Y[k];
        gone = dx * dx + dy * dy <= BITE_R[k] * BITE_R[k];
      }
      if (!gone) gfx->fillRect(x0 + c * S, y0 + r * S, S, S, spriteColor(ch));
    }
  // crumbs: a short spray from the bite that just landed
  for (uint8_t k = 0; k < bites; k++) {
    uint32_t since = el - BITE_AT[k];
    if (since >= 380) continue;
    int bx = x0 + BITE_X[k] * S, by = y0 + BITE_Y[k] * S;
    for (int i = 0; i < 3; i++) {
      int dx = (i - 1) * (6 + (int)(since / 14)) + (BITE_X[k] > 8 ? 6 : -6);
      int dy = -4 + (int)(since * since / 3600) - i * 3;
      gfx->fillRect(bx + dx, by + dy, 3, 3, spriteColor(ic[8][8] == '.' ? 'w' : ic[8][8]));
    }
  }
}

void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}
