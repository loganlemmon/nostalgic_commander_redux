#include <pebble.h>
#include "chime.h"
#include "data.h"

bool chime_should_play(int minute, int enabled, bool muted) {
  return minute == 0 && enabled && !muted;
}

int chime_effective_volume(int requested) {
  if (requested < 0) return 0;
  if (requested > 100) return 100;
  return requested;
}

void chime_play(void) {
  // Square — the PC speaker's 8253 never did anything else.
  speaker_play_tone(CHIME_FREQ_HZ, CHIME_DURATION_MS,
                    chime_effective_volume(s_settings_chime_volume), SpeakerWaveformSquare);
}

void chime_on_tick(const struct tm* tick_time) {
  if (chime_should_play(tick_time->tm_min, s_settings_hourly_chime, speaker_is_muted())) {
    chime_play();
  }
}
