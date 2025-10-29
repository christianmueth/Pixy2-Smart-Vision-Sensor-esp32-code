// ZumoBuzzer.cpp — ESP32 port for Arduino-ESP32 core 3.x (tested 3.3.1)
// Replaces AVR timers/ISRs with LEDC PWM (pin-based API) + one-shot hardware timer.
// Public API matches the original (playNote, playFrequency, play, stopPlaying, etc).

#ifdef ARDUINO_ARCH_ESP32

#include <Arduino.h>
#include <pgmspace.h>           // OK on ESP32 (no avr/ prefix)
#include <esp32-hal-ledc.h>     // LEDC (PWM) helpers (3.x, pin-based)
#include <esp32-hal-timer.h>    // HW timer helpers (3.x)
#include "ZumoBuzzer.h"

// ===================== CONFIGURABLES =====================
#ifndef ZUMO_BUZZER_PIN
#define ZUMO_BUZZER_PIN 13      // change to your buzzer GPIO
#endif

// LEDC: 12-bit duty resolution (0..4095). Works fine up to 10 kHz tones.
static const uint8_t  LEDC_RES_BITS = 12;
static const uint32_t LEDC_DUTY_MAX = (1U << LEDC_RES_BITS) - 1;

// ===================== STATE (mirrors the AVR version) =====================
unsigned char buzzerInitialized = 0;
volatile unsigned char buzzerFinished = 1;     // 0 while playing
const char *buzzerSequence = 0;

static volatile unsigned int buzzerTimeout_ms = 0;   // not strictly needed with one-shot
static char play_mode_setting = PLAY_AUTOMATIC;

static unsigned char use_program_space = 0;

// Music settings and defaults (same as original)
static unsigned char  octave = 4;
static unsigned int   whole_note_duration = 2000;
static unsigned int   note_type = 4;
static unsigned int   duration = 500;
static unsigned int   volume = 15;              // 0..15
static unsigned char  staccato = 0;
static unsigned char  staccato_rest_duration = 0;

static void nextNote();

// ================== ESP32 TIMER (one-shot for note duration) ==================
static hw_timer_t* s_timer = nullptr;
// Protect tiny flags touched in ISR:
static portMUX_TYPE s_timerMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_noteElapsed = false;

static void IRAM_ATTR onTimerDone() {
  // Stop PWM immediately (silence)
  ledcWrite(ZUMO_BUZZER_PIN, 0);
  portENTER_CRITICAL_ISR(&s_timerMux);
  buzzerFinished = 1;
  s_noteElapsed = true;  // main context will advance the sequence
  portEXIT_CRITICAL_ISR(&s_timerMux);
}

// Arm one-shot timer for dur_ms (milliseconds)
static void armOneShotTimer(uint32_t dur_ms) {
  if (!s_timer) {
    // New 3.x API: frequency-based begin. Use 1 MHz so alarm values are microseconds.
    s_timer = timerBegin(1000000);              // 1,000,000 Hz => 1 tick = 1 µs
    timerAttachInterrupt(s_timer, &onTimerDone);
  }
  timerStop(s_timer);
  timerWrite(s_timer, 0);

  uint64_t us = (uint64_t)dur_ms * 1000ULL;
  // One-shot alarm (no autoreload). 4th arg (reload_count) is required in 3.x.
  timerAlarm(s_timer, us, false, 0);
  timerStart(s_timer);
}

// ================== LEDC HELPERS (3.x pin-based API) ===================
static bool     s_ledcAttached = false;
static uint32_t s_currFreq     = 0;

static void ledcEnsureSetup(uint32_t freq) {
  if (!s_ledcAttached) {
    // Attach the pin and allocate a channel automatically.
    // Signature: bool ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolution_bits)
    // Returns true on success.
    ledcAttach(ZUMO_BUZZER_PIN, freq, LEDC_RES_BITS);
    s_ledcAttached = true;
    s_currFreq = freq;
  } else if (freq != s_currFreq) {
    // Change frequency for this pin
    ledcChangeFrequency(ZUMO_BUZZER_PIN, freq, LEDC_RES_BITS);
    s_currFreq = freq;
  }
}

static void ledcSetTone(uint32_t freq, uint8_t vol0_15) {
  if (freq < 40)     freq = 40;
  if (freq > 10000)  freq = 10000;

  ledcEnsureSetup(freq);

  if (vol0_15 == 0) {
    ledcWrite(ZUMO_BUZZER_PIN, 0);
    return;
  }
  // Map 0..15 → 0..LEDC_DUTY_MAX (linear; matches original semantics)
  uint32_t duty = (uint32_t)vol0_15 * LEDC_DUTY_MAX / 15U;
  ledcWrite(ZUMO_BUZZER_PIN, duty);
}

// ================== PUBLIC API (ESP32) ===================

ZumoBuzzer::ZumoBuzzer() {}

inline void ZumoBuzzer::init() {
  if (!buzzerInitialized) {
    buzzerInitialized = 1;
    init2();
  }
}

// Initialize PWM + timer, silence buzzer
void ZumoBuzzer::init2() {
  // Ensure LEDC is attached at a safe default
  ledcEnsureSetup(1000);
  ledcWrite(ZUMO_BUZZER_PIN, 0);

  if (!s_timer) {
    s_timer = timerBegin(1000000);    // 1 MHz
    timerAttachInterrupt(s_timer, &onTimerDone);
  }

  buzzerFinished = 1;
  s_noteElapsed  = false;
}

// Play frequency (Hz or .1 Hz if DIV_BY_10 set) for dur ms with volume 0..15
void ZumoBuzzer::playFrequency(unsigned int freq, unsigned int dur, unsigned char vol_in) {
  init();

  unsigned char multiplier = 1;
  if (freq & DIV_BY_10) {
    multiplier = 10;
    freq &= ~DIV_BY_10;
  }

  unsigned int minHz = 40 * multiplier;
  if (freq < minHz) freq = minHz;
  if (multiplier == 1 && freq > 10000) freq = 10000;

  if (multiplier == 10) {
    // Convert .1 Hz units back to Hz for LEDC
    freq = (freq + 5) / 10;
  }

  unsigned char vol = vol_in;
  if (vol > 15) vol = 15;

  // Start tone (use 1000 Hz for "silent" bookkeeping if vol==0, matching original)
  if (vol == 0) {
    ledcSetTone(1000, 0);
  } else {
    ledcSetTone(freq, vol);
  }

  // Arm the duration
  buzzerFinished   = 0;
  s_noteElapsed    = false;
  buzzerTimeout_ms = dur;
  armOneShotTimer(dur);
}

// Same integer math as original (no floats)
void ZumoBuzzer::playNote(unsigned char note, unsigned int dur, unsigned char vol) {
  unsigned int freq = 0;
  unsigned char offset_note = note - 16;

  if (note == SILENT_NOTE || vol == 0) {
    freq = 1000; // silent note convention
    playFrequency(freq, dur, 0);
    return;
  }

  if (note <= 16) offset_note = 0;
  else if (offset_note > 95) offset_note = 95;

  unsigned char exponent = offset_note / 12;

  // Base table in tenths of Hz for the lowest 12 notes (same as original)
  switch (offset_note - exponent * 12) {
    case 0:  freq = 412; break;  // E1 = 41.2 Hz
    case 1:  freq = 437; break;  // F1
    case 2:  freq = 463; break;  // F#1
    case 3:  freq = 490; break;  // G1
    case 4:  freq = 519; break;  // G#1
    case 5:  freq = 550; break;  // A1
    case 6:  freq = 583; break;  // A#1
    case 7:  freq = 617; break;  // B1
    case 8:  freq = 654; break;  // C2
    case 9:  freq = 693; break;  // C#2
    case 10: freq = 734; break;  // D2
    case 11: freq = 778; break;  // D#2
  }

  if (exponent < 7) {
    freq = freq << exponent;      // * 2^exponent
    if (exponent > 1) freq = (freq + 5) / 10;   // drop .1Hz precision if >160 Hz
    else freq += DIV_BY_10;                      // keep .1Hz flag
  } else {
    freq = (freq * 64 + 2) / 5;   // == freq * 2^7 / 10 without overflow
  }

  if (vol > 15) vol = 15;
  playFrequency(freq, dur, vol);
}

unsigned char ZumoBuzzer::isPlaying() {
  return !buzzerFinished || buzzerSequence != 0;
}

// ===== Sequence helpers (unchanged semantics) =====

static char currentCharacter() {
  char c = 0;
  do {
    c = use_program_space ? pgm_read_byte(buzzerSequence) : *buzzerSequence;
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  } while (c == ' ' && (buzzerSequence++));
  return c;
}

static unsigned int getNumber() {
  unsigned int arg = 0;
  char c = currentCharacter();
  while (c >= '0' && c <= '9') {
    arg = arg * 10 + (c - '0');
    buzzerSequence++;
    c = currentCharacter();
  }
  return arg;
}

void ZumoBuzzer::play(const char *notes) {
  buzzerSequence = notes;
  use_program_space = 0;
  staccato_rest_duration = 0;
  nextNote();
}

void ZumoBuzzer::playFromProgramSpace(const char *notes_p) {
  buzzerSequence = notes_p;
  use_program_space = 1;
  staccato_rest_duration = 0;
  nextNote();
}

void ZumoBuzzer::stopPlaying() {
  // Silence output and stop the note timer
  ledcWrite(ZUMO_BUZZER_PIN, 0);
  if (s_timer) timerStop(s_timer);
  buzzerFinished = 1;
  buzzerSequence = 0;
}

static void nextNote() {
  unsigned char note = 0, rest = 0;
  unsigned char tmp_octave = octave;
  unsigned int  tmp_duration;
  unsigned int  dot_add;
  char c;

  if (staccato && staccato_rest_duration) {
    ZumoBuzzer::playNote(SILENT_NOTE, staccato_rest_duration, 0);
    staccato_rest_duration = 0;
    return;
  }

parse_character:
  c = currentCharacter();
  buzzerSequence++;

  switch (c) {
    case '>': tmp_octave++; goto parse_character;
    case '<': tmp_octave--; goto parse_character;
    case 'a': note = NOTE_A(0); break;
    case 'b': note = NOTE_B(0); break;
    case 'c': note = NOTE_C(0); break;
    case 'd': note = NOTE_D(0); break;
    case 'e': note = NOTE_E(0); break;
    case 'f': note = NOTE_F(0); break;
    case 'g': note = NOTE_G(0); break;
    case 'l':
      note_type = getNumber();
      duration = whole_note_duration / note_type;
      goto parse_character;
    case 'm':
      if (currentCharacter() == 'l') staccato = false;
      else { staccato = true; staccato_rest_duration = 0; }
      buzzerSequence++;
      goto parse_character;
    case 'o':
      octave = getNumber();
      tmp_octave = octave;
      goto parse_character;
    case 'r':
      rest = 1; break;
    case 't':
      // (same arithmetic as your original)
      whole_note_duration = 60 * 400 / getNumber() * 10;
      duration = whole_note_duration / note_type;
      goto parse_character;
    case 'v':
      volume = getNumber();
      goto parse_character;
    case '!':
      octave = 4; whole_note_duration = 2000; note_type = 4; duration = 500; volume = 15; staccato = 0;
      tmp_octave = octave; tmp_duration = duration;
      goto parse_character;
    default:
      buzzerSequence = 0; return;
  }

  note += tmp_octave * 12;

  // Sharps/flats
  c = currentCharacter();
  while (c == '+' || c == '#') { buzzerSequence++; note++; c = currentCharacter(); }
  while (c == '-') { buzzerSequence++; note--; c = currentCharacter(); }

  // Duration of this note
  tmp_duration = duration;

  if (c > '0' && c < '9') tmp_duration = whole_note_duration / getNumber();

  // Dotted notes
  dot_add = tmp_duration / 2;
  while (currentCharacter() == '.') {
    buzzerSequence++;
    tmp_duration += dot_add;
    dot_add /= 2;
  }

  if (staccato) {
    staccato_rest_duration = tmp_duration / 2;
    tmp_duration -= staccato_rest_duration;
  }

  ZumoBuzzer::playNote(rest ? SILENT_NOTE : note, tmp_duration, volume);
}

// Advance sequence in main context (not inside ISR)
unsigned char ZumoBuzzer::playCheck() {
  if (s_noteElapsed) {
    portENTER_CRITICAL(&s_timerMux);
    s_noteElapsed = false;
    portEXIT_CRITICAL(&s_timerMux);
    if (buzzerSequence && (play_mode_setting == PLAY_AUTOMATIC)) {
      nextNote();
    }
  }
  if (buzzerFinished && buzzerSequence != 0 && play_mode_setting == PLAY_AUTOMATIC) {
    nextNote();
  }
  return buzzerSequence != 0;
}

void ZumoBuzzer::playMode(unsigned char mode) {
  play_mode_setting = mode;
  if (mode == PLAY_AUTOMATIC) playCheck();
}

#endif // ARDUINO_ARCH_ESP32
