#pragma once
#include <pebble.h>

// The POST success beep, on the hour, when the settings select consents (off
// by default). Numbers are the BIOS beep: 880 Hz (A5) as the PIT quantizes it
// (divisor 1331 → ~896 Hz measured on real hardware), for the IBM BIOS's half
// second, square as the 8253 always was.
#define CHIME_FREQ_HZ 896
#define CHIME_DURATION_MS 500
// The boot default for the "Chime volume" select; at call time the setting's
// s_settings_chime_volume rules, clamped to the speaker's 0-100 below.
#define CHIME_DEFAULT_VOLUME 5

// The gate, kept pure so the host suite can assert every combination without
// a speaker: full-hour minute, consent on, not muted (system mute and Quiet
// Time, polled via speaker_is_muted() at the call site below).
bool chime_should_play(int minute, int enabled, bool muted);

// Settings arrive as arbitrary ints; clamp to [0, 100] so an off-wire value
// never reaches the SDK out of range.
int chime_effective_volume(int requested);

// One POST beep at the configured (clamped) volume, ungated — called by
// chime_on_tick at the full hour and by messaging.c for the settings page's
// test button.
void chime_play(void);

// Tick hook; main.c's tick_handler calls this on every MINUTE_UNIT tick.
void chime_on_tick(const struct tm* tick_time);
