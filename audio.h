#pragma once
#include <stdint.h>

// Game sound effects (queue, non-blocking). The order matches the
// SFX table in audio.cpp.
enum Sfx : uint8_t {
  SFX_TAP = 0,  // tap / button
  SFX_EAT,      // comer
  SFX_PLAY,     // minigame point / hit
  SFX_HEART,    // likes it / cuddle
  SFX_HATCH,    // eclosion
  SFX_EVOLVE,   // evolution
  SFX_MEDAL,    // medal / milestone
  SFX_DENY,     // action not allowed
  SFX_BYE,      // farewell
  SFX_LEVEL,    // level up
  // battle cues: a fight in silence is what made it feel flat
  SFX_HIT,      // a physical move landing
  SFX_BEAM,     // a special move
  SFX_STATUS,   // a status move or an ailment taking hold
  SFX_SUPER,    // super effective
  SFX_FAINT,    // something goes down
  SFX_VICTORY,  // the fight is won
  SFX_COUNT
};

// Background music. One square-wave voice and a single blocking audio task, so
// this is not mixed with the effects -- the task plays the tune a note at a
// time and hands the voice to any effect that arrives, resuming after. An
// effect therefore always cuts through, which is what you want anyway.
enum Music : uint8_t { MUS_NONE = 0, MUS_BATTLE, MUS_VICTORY };
void audioMusic(uint8_t id);

// 0..10. Stored, and applied as the square wave's amplitude; 0 is silence
// without disabling the system, which is what a mute expects to do.
void audioSetVolume(uint8_t v);   // 0..100, RAM only: cheap enough to call while dragging
void audioSaveVolume();           // persist it (NVS), once the drag ends
uint8_t audioVolume();

void audioBegin();          // init ES8311 + I2S + amplifier + audio task
void sfxPlay(uint8_t id);   // queues an effect (does not block the loop)
void audioSetEnabled(bool on);
bool audioEnabled();
