#include <pebble.h>
#include "crt.h"
#include "main.h"
#include "theme.h"

// Everything runs on GColor8 bytes (0xAARRGGBB, 2 bits per channel) captured
// from the framebuffer. Emery-only build; the face targets no B&W platform.

#define GCOLOR8_ALPHA 0xC0
#define GCOLOR8_OPAQUE_BLACK 0xC0

// Current warm-up phase, or CRT_FLASH_IDLE. Advanced by the self-re-arming
// flash tick; written by crt_backlight_handler on backlight-on.
static int s_flash_phase = CRT_FLASH_IDLE;

static int isqrt_floor(int v) {
  if (v <= 0) return 0;
  int r = 1;
  while ((r + 1) <= v / (r + 1)) r++;  // no overflow: r*r <= v
  return r;
}

// Parallel 4x4 ordered-dither threshold grid — the only way a 4-level
// channel ramps smoothly: the pass quantizes c*f/256 with this threshold, so
// the vignette falloff shows as dither pattern, not banded steps.
static const uint8_t s_bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

int crt_warp_inset(int y, int h) {
  int dy = 2 * y - (h - 1);  // -(h-1)..h-1
  return (CRT_WARP_MAX_PX * dy * dy) / ((h - 1) * (h - 1));
}

// Declared before use in apply below.
static int crt_ca_shift_from_q8(int q8, int t);

int crt_ca_shift(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  int dy = 2 * y - (h - 1);
  // Squared distance to the nearest edge, Q8 (256 = an edge), per-axis
  // normalised — the stronger axis wins, so mid-edge and corner cells fringe
  // equally.
  int xq = (dx * dx * 256) / ((w - 1) * (w - 1));
  int yq = (dy * dy * 256) / ((h - 1) * (h - 1));
  return crt_ca_shift_from_q8(xq > yq ? xq : yq, 8);
}

// The q8 -> px mapping: remap the radius after the dead zone across the full
// shift range, then jitter the rounding line by the pixel's Bayer cell. Right
// at a step boundary adjacent cells round differently, so the CA dawns over a
// granular 4x4 dot pattern rather than a contour line on the glass.
static int crt_ca_shift_from_q8(int q8, int t) {
  if (q8 <= CRT_CA_START_Q8) return 0;
  int remap = (q8 - CRT_CA_START_Q8) * 256 / (256 - CRT_CA_START_Q8) + CRT_CA_LIFT_Q8;
  int f16 = remap * CRT_CA_MAX_SHIFT;
  int shift = (f16 + (t - 8) * 16) >> 8;  // Bayer nudge moves the rounding line
  if (shift < 0) shift = 0;
  return shift > CRT_CA_MAX_SHIFT ? CRT_CA_MAX_SHIFT : shift;
}

// Distance toward the nearest frame edge, counting around the corner arcs:
// inside the corner square the boundary is the arc, elsewhere the straight
// edge. 0 = boundary itself, growing inward.
static int crt_edge_distance(int x, int y, int w, int h) {
  int ex = x < w - 1 - x ? x : w - 1 - x;
  int ey = y < h - 1 - y ? y : h - 1 - y;
  if (ex < CRT_CORNER_RADIUS && ey < CRT_CORNER_RADIUS) {
    int rx = CRT_CORNER_RADIUS - ex;
    int ry = CRT_CORNER_RADIUS - ey;
    int d = CRT_CORNER_RADIUS - isqrt_floor(rx * rx + ry * ry);
    return d < 0 ? 0 : d;
  }
  return ex < ey ? ex : ey;
}

// The vignette falloff over edge-depth d, smoothstepped, Q8. A LUT: the pass
// does no per-pixel division, which is what kept the wake-up at 3–4 fps on
// hardware.
static const uint16_t s_vignette_q8[CRT_VIGNETTE_PX + 1] = {
    0, 1, 6, 15, 26, 40, 54, 71, 89, 108, 128, 145, 165, 183, 200, 216, 228, 240, 248, 254, 256};

// Same falloff without per-pixel arithmetic (the LUT above carries it).
static int crt_vignette_q8_from_depth(int d) {
  return d >= CRT_VIGNETTE_PX ? 256 : s_vignette_q8[d];
}

int crt_vignette_q8(int x, int y, int w, int h) {
  return crt_vignette_q8_from_depth(crt_edge_distance(x, y, w, h));
}

// Strike amplitude per frame, px. Decays to zero; the strike reads as a
// wobble that settles, never a sequence of bands.
static const int s_strike_amp_px[CRT_FLASH_PHASES] = {6, 5, 4, 3, 2, 2, 1, 1};

// Per-row jitter: keyed hashing, deterministic, adjacent rows uncorrelated.
int crt_strike_offset(int y, int flash_phase) {
  if (flash_phase < 0 || flash_phase >= CRT_FLASH_PHASES) return 0;
  int amp = s_strike_amp_px[flash_phase];
  // Knuth multiplicative, cheap avalanche for row index × phase.
  uint32_t noise = (uint32_t)(y + 1) * 2654435761u + (uint32_t)flash_phase * 40503u;
  noise ^= noise >> 13;
  return (int)(noise % (uint32_t)(2 * amp + 1)) - amp;
}

// c' = round(c * f / 256) with the pixel's Bayer threshold breaking the
// rounding direction — shift-only (f <= 256 keeps c*16*f under 12k).
static int dither_channel(int c, int f_q8, int t) {
  int v = ((c * 16 * f_q8 >> 8) + t) >> 4;
  return v > 3 ? 3 : v;
}

void crt_apply_framebuffer(uint8_t* fb, int w, int h, int flash_phase) {
  static uint8_t row_ca[200];
  // CA's x² column term: filled once for the actual width (constant on emery,
  // but the field keeps the host tests honest).
  static uint16_t s_ca_xterm[200];
  static int s_ca_xterm_w = 0;
  if (w > (int)sizeof(row_ca)) return;
  if (s_ca_xterm_w != w) {
    for (int x = 0; x < w; x++) {
      int dx = 2 * x - (w - 1);
      s_ca_xterm[x] = (uint16_t)((dx * dx * 256) / ((w - 1) * (w - 1)));
    }
    s_ca_xterm_w = w;
  }
  // CA boost during the strike: the separation balloons while the mask
  // degausses. Phase-indexed through the same decay table as the row jitter.
  int ca_boost = (flash_phase >= 0 && flash_phase < CRT_FLASH_PHASES)
                     ? (s_strike_amp_px[flash_phase] + 1) / 2
                     : 0;
  const int h1sq = (h - 1) * (h - 1);

  for (int y = 0; y < h; y++) {
    uint8_t* row = fb + (size_t)y * w;
    int dy = 2 * y - (h - 1);
    int yterm = dy * dy * 256 / h1sq;
    int row_off = crt_strike_offset(y, flash_phase);

    // 1) CA on the RAW row: the fringe samples clean pixels, so a uniform
    //    background stays uniform — neither side borrows vignette darkness.
    //    Jitter cell shares the vignette's mirrored Bayer grid.
    for (int x = 0; x < w; x++) {
      int q8 = s_ca_xterm[x] > yterm ? s_ca_xterm[x] : yterm;
      int ex = x < w - 1 - x ? x : w - 1 - x;
      int t = s_bayer4[((y < h - 1 - y ? y : h - 1 - y) & 3) * 4 + (ex & 3)];
      int s = crt_ca_shift_from_q8(q8, t) + ca_boost;
      if (s <= 0) {
        row_ca[x] = row[x];
      } else {
        // Fringe direction mirrors across the vertical centreline, as it does
        // on a real tube's shadow-mask misregistration.
        bool left = x * 2 < w - 1;
        int r_from = left ? x - s : x + s;
        int b_from = left ? x + s : x - s;
        if (r_from < 0) r_from = 0;
        if (r_from >= w) r_from = w - 1;
        if (b_from < 0) b_from = 0;
        if (b_from >= w) b_from = w - 1;
        row_ca[x] = GCOLOR8_ALPHA | (row[r_from] & 0x30) | (row[x] & 0x0C) | (row[b_from] & 0x03);
      }
    }

    // 2) Vignette + dither on the CA'd row — dimming in content space, so
    //    the curvature pass below bends the already-darkened rim with the
    //    image. Mirror-symmetric Bayer thresholds keep both rims identical.
    int inset = crt_warp_inset(y, h);
    int mul16 = (w << 16) / (w - 2 * inset);  // Q16 jacobian; ==65536 mid-rows
    int ey = y < h - 1 - y ? y : h - 1 - y;
    int ty = ey & 3;
    bool corner_row = ey < CRT_CORNER_RADIUS;
    for (int x = 0; x < w; x++) {
      int ex = x < w - 1 - x ? x : w - 1 - x;
      int d;
      if (corner_row && ex < CRT_CORNER_RADIUS) {
        int rx = CRT_CORNER_RADIUS - ex;
        int ry = CRT_CORNER_RADIUS - ey;
        d = CRT_CORNER_RADIUS - isqrt_floor(rx * rx + ry * ry);
        if (d < 0) d = 0;
      } else {
        d = ex < ey ? ex : ey;
      }
      int f = crt_vignette_q8_from_depth(d);
      uint8_t p = row_ca[x];
      int t = s_bayer4[ty * 4 + (ex & 3)];
      int r = dither_channel((p >> 4) & 3, f, t);
      int g = dither_channel((p >> 2) & 3, f, t);
      int b = dither_channel(p & 3, f, t);
      row_ca[x] = GCOLOR8_ALPHA | (uint8_t)((r << 4) | (g << 2) | b);
    }

    // 3) Curvature (+ strike jitter): source column
    //    sx = cx + (x - cx) * (w / (w - 2*inset)), cx=(w-1)/2, then the strike
    //    slides the whole row sideways. Rounded to nearest on BOTH signs — a
    //    >> on negative deltas floors away from zero and clipped the left rim
    //    a quantum earlier than the right (visible on hardware as a left-edge
    //    shift under each header).
    for (int x = 0; x < w; x++) {
      int dx = 2 * x - (w - 1);
      int sn = (w - 1) * 65536 + dx * mul16 + 65536;  // sx*131072 + half
      int sx = (sn >> 17) + row_off;
      row[x] = (sx < 0 || sx >= w) ? GCOLOR8_OPAQUE_BLACK : row_ca[sx];
    }
  }
}

void crt_update_proc(Layer* layer, GContext* ctx) {
  (void)layer;
  if (!s_settings_crt || !ctx) return;
  // The captured bitmap IS the framebuffer on a native-format platform;
  // release before returning or the layer never lands.
  GBitmap* fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect bounds = gbitmap_get_bounds(fb);
  crt_apply_framebuffer(gbitmap_get_data(fb), bounds.size.w, bounds.size.h, s_flash_phase);
  graphics_release_frame_buffer(ctx, fb);
}

static void crt_flash_tick(void* data) {
  (void)data;
  s_flash_phase++;
  if (s_flash_phase >= CRT_FLASH_PHASES) s_flash_phase = CRT_FLASH_IDLE;
  if (s_crt_layer) layer_mark_dirty(s_crt_layer);
  // One-shot timers free themselves on firing. A NULL return (pool
  // exhausted) just ends the flash early — the next backlight-on restarts it
  // at the strike phase anyway.
  if (s_flash_phase != CRT_FLASH_IDLE) {
    app_timer_register(CRT_FLASH_TICK_MS, crt_flash_tick, NULL);
  }
}

void crt_backlight_handler(bool on) {
  // Backlight-only gating: the flash is the strike of a tube warming up;
  // backlight-off transitions and a disabled effect start nothing.
  if (!on || !s_settings_crt) return;
  s_flash_phase = 0;
  crt_play_strike_sound();
  if (s_crt_layer) layer_mark_dirty(s_crt_layer);
  app_timer_register(CRT_FLASH_TICK_MS, crt_flash_tick, NULL);
}

// The degauss woomp as synthesized PCM: note-table playback clicks at every
// note boundary (pitch and velocity step), so the hum is generated instead —
// a pitch-gliding sine (≈95 → 65 Hz) under a swell-and-decay amplitude
// envelope. Continuous by construction. 320ms at 16 kHz 16-bit sits well
// under SPEAKER_MAX_SAMPLE_BYTES_TOTAL. Synthesized once on first use.
#define CRT_STRIKE_PCM_MS 320
#define CRT_STRIKE_PCM_RATE 16000
#define CRT_STRIKE_PCM_SAMPLES (CRT_STRIKE_PCM_MS * CRT_STRIKE_PCM_RATE / 1000)
// DMA'd to the codec verbatim — keep the buffer word-aligned.
static int16_t s_strike_pcm[CRT_STRIKE_PCM_SAMPLES] __attribute__((aligned(4)));
static bool s_strike_pcm_ready = false;

// Q15 sine: |x| in [-1,1] shaped by the cubic smoothstep y = (3x − x³)/2 —
// within 1.1% of sin(πx/2), no libm. (A hard fault inside newlib's
// __ieee754_rem_pio2 took down the app on hardware — sin() has no business
// in a watchface sound anyway.)
static int16_t crt_wave_q15(uint32_t phase_q16) {
  // Triangle coordinate over each half-cycle: x sweeps -1..+1 per half.
  int32_t half = (int32_t)((phase_q16 >> 15) & 1);
  int32_t x = (int32_t)((phase_q16 & 32767) * 2) - 32767;
  int64_t x2 = (int64_t)x * x;
  int y = (int)((3 * (int64_t)x - (x2 * x >> 30)) >> 1);
  // The cubic kisses ±(32767+2) near |x|=1 — cast to int16_t would wrap the
  // sign at the peaks (heard as clicks). Clamp before casting.
  if (y > 32767) y = 32767;
  if (y < -32767) y = -32767;
  return (int16_t)(half ? -y : y);
}

void crt_strike_synth(int16_t* buf, size_t n) {
  // Phase accumulator, Q16 per sample; glide 90 → 55 Hz; envelope: rise
  // 0-20%, hold to 40%, linear decay reaching silence 64 samples before the
  // end (the trailing zeros keep the codec from cutting mid-cycle).
  // Voice: 4/7 fundamental, 2/7 second, 1/7 third harmonic — the watch
  // speaker barely reproduces the sub-100Hz fundamental, so the audible
  // weight rides the harmonics. All integer.
  uint32_t phase = 0;
  for (size_t i = 0; i < n; i++) {
    int f_q8 = 90 * 256 - (int)((35 * 256 * i) / n);  // Hz, fixed point
    phase += (uint32_t)((f_q8 * 65536) / (256 * CRT_STRIKE_PCM_RATE));
    int env;
    if (i < n / 5) {
      env = (int)((i * 256) / (n / 5));  // rise
    } else if (i < 2 * n / 5) {
      env = 256;  // hold
    } else if (i < n - 64) {
      env = 256 - (int)(((i - 2 * n / 5) * 256) / (n - 64 - 2 * n / 5));  // decay
      if (env < 0) env = 0;
    } else {
      env = 0;  // trailing silence
    }
    int y = (4 * crt_wave_q15(phase) + 2 * crt_wave_q15(phase * 2) + crt_wave_q15(phase * 3)) / 7;
    buf[i] = (int16_t)(((int64_t)y * env * 28000) >> 23);  // 64-bit: the product is ~2³¹
  }
}

void crt_play_strike_sound(void) {
  // speaker_is_muted covers both system mute and Quiet Time; honor it, and
  // the sound toggle.
  if (!s_settings_crt_sound || speaker_is_muted()) return;
  if (!s_strike_pcm_ready) {
    crt_strike_synth(s_strike_pcm, CRT_STRIKE_PCM_SAMPLES);
    s_strike_pcm_ready = true;
  }
  // One unshifted note over the whole sample plays it raw: the PCM IS the
  // sound.
  static const SpeakerNote through[1] = {
      {60, SpeakerWaveformSine, CRT_STRIKE_PCM_MS + 30, 127, 0},
  };
  static const SpeakerSample sample = {.data = s_strike_pcm,
                                       .num_bytes = CRT_STRIKE_PCM_SAMPLES * 2,
                                       .format = SpeakerPcmFormat_16kHz_16bit,
                                       .base_midi_note = 60,
                                       .loop = false};
  const SpeakerTrack track = {.notes = through, .num_notes = 1, .sample = &sample};
  speaker_play_tracks(&track, 1, 85);
}

void crt_apply_setting_change(void) {
  if (s_settings_crt) {
    if (s_crt_layer) layer_mark_dirty(s_crt_layer);
    return;
  }
  s_flash_phase = CRT_FLASH_IDLE;
  // Off must ERASE, not just stop painting: the shader's pixels live in the
  // shared framebuffer and the next paint of anything underneath only happens
  // at the minute edge. Re-applying the window background dirties the whole
  // tree now — canvas and clock repaint, the overlay (disabled) adds nothing.
  if (s_main_window) window_set_background_color(s_main_window, s_active_theme->center_bg);
  if (s_crt_layer) layer_mark_dirty(s_crt_layer);
}
