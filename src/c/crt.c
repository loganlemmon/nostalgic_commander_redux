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
static AppTimer* s_flash_timer = NULL;  // latest tick arm — cancelled on retrigger
// Strike primed by the backlight handler, started by the overlay proc AFTER
// the wake frame lands: measured on hardware, the wake burst (bg fill, canvas
// glyphs, clock, pass) blocks long enough to drain the audio ring once — the
// audible pop-gap-hum. Delaying sound+chain past that frame keeps the ring fed.
static bool s_strike_pending = false;

static void crt_flash_tick(void* data);

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

// Radial magnification of the curvature stage, Q16. Identity at the centre,
// K per unit of the elliptical squared radius (same xq/yq formulation as the
// CA zone map). K=12 gives ≈4.7px pull at a mid-edge, tuned on hardware
// against the 16px vignette (K=13's 5px bent the rim darkening over the
// bright side frames). Grows smoothly to 1.09x in the corners — a curved
// surface bows its sides too, so mid-height now warps where the old y-only
// gain was identity. Rank-1 quantized all outer columns on the same row
// boundaries (visible seams); the per-column boundary curve does not.
int crt_warp_q16(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  int dy = 2 * y - (h - 1);
  int xq = (dx * dx * 256) / ((w - 1) * (w - 1));
  int yq = (dy * dy * 256) / ((h - 1) * (h - 1));
  return 65536 + CRT_WARP_R2_K * (xq + yq);
}

// Source column for dest (x,y) under the warp, without strike jitter —
// nearest-rounded. Pin target only: the pass works in floor+frac of the same
// S (for the blend), a half-px phase offset from this form.
int crt_warp_sx(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  return ((w - 1) * 65536 + dx * crt_warp_q16(x, y, w, h) + 65536) >> 17;
}

// Horizontal CA rung from the squared Q8 radius (units: xq + yq from
// crt_ca_shift3), returning per-channel displacement in THIRDS of a pixel:
// 2 / 4 / 8 = 2/3 / 1+1/3 / 2+2/3 px. Thirds because the pass now samples
// channels fractionally (two weighted taps) instead of copying whole
// neighbours — see stage 1 in crt_apply_framebuffer. The dead zone gets 2/3px
// of base misconvergence (hardware A/B: zero read flat-eyed on the big
// digits). Zone boundaries squared once:
//   rq ≥ R ⇔ xq+yq ≥ ceil(R²/256) — so no per-pixel sqrt is needed.
static int crt_ca_t3_h2(int xy_sum) {
  return xy_sum < CRT_CA_R2_X2Q8 ? 2 : (xy_sum < CRT_CA_R3_X2Q8 ? 4 : 8);
}
// Vertical wider zones, 1px max, same formulation.
static int crt_ca_shift_v2(int xy_sum) {
  return xy_sum < CRT_CA_R2V_X2Q8 ? 0 : 1;
}

int crt_ca_shift3(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  int dy = 2 * y - (h - 1);
  // Elliptical radius from the centre, Q8: corner ≈ 362, mid-edge = 256.
  int xq = (dx * dx * 256) / ((w - 1) * (w - 1));
  int yq = (dy * dy * 256) / ((h - 1) * (h - 1));
  return crt_ca_t3_h2(xq + yq);
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

// The vignette falloff over edge-depth d, Q8; smoothstep gamma-lifted by 0.7 —
// 4 levels quantize the raw smoothstep into a noticed black cliff near the rim
// (hardware A/B: the fade 'started too sharp'). Lifting the low half softens
// the rim start and raises the mid band ~15% without widening the fade.
// A LUT: the pass does no per-pixel division, which is what kept the wake-up
// at 3–4 fps on hardware.
static const uint16_t s_vignette_q8[CRT_VIGNETTE_PX + 1] = {
    0, 11, 28, 48, 70, 92, 114, 136, 158, 178, 196, 213, 227, 239, 248, 254, 256};

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

// Vertical CA samples neighbour rows; the row the pass writes into must stay
// raw for the reader. Ring of the last three raw rows, populated in the row's
// own turn, read for ±s_v offsets below/above.
static uint8_t s_vraw_ring[3][200];

void crt_apply_framebuffer(uint8_t* fb, int w, int h, int flash_phase) {
  static uint8_t row_ca[200];
  static uint16_t s_ca_xq[200];
  static int s_ca_xq_w = 0;
  if (w > (int)sizeof(row_ca) || w <= 1 || h <= 1) return;
  if (s_ca_xq_w != w) {
    for (int x = 0; x < w; x++) {
      int dx = 2 * x - (w - 1);
      s_ca_xq[x] = (uint16_t)((dx * dx * 256) / ((w - 1) * (w - 1)));
    }
    s_ca_xq_w = w;
  }
  // CA boost during the strike: the separation balloons while the mask
  // degausses. Phase-indexed through the same decay table as the row jitter.
  int ca_boost = (flash_phase >= 0 && flash_phase < CRT_FLASH_PHASES)
                     ? (s_strike_amp_px[flash_phase] + 1) / 2
                     : 0;
  const int h1sq = (h - 1) * (h - 1);

  // Per-row body, applied in two half-passes. Both passes read away from
  // the screen centreline: from captured (raw) ring rows only, which is the
  // pass-order invariant that lets dest rows be rewritten in place.
  for (int pass = 0; pass < 2; pass++) {
    // pass 0: 0..(h-1)/2 forward; pass 1: h-1..(h-1)/2+1 backward.
    for (int yi = (pass == 0 ? 0 : h - 1); pass == 0 ? yi <= (h - 1) / 2 : yi > (h - 1) / 2;
         yi += pass == 0 ? 1 : -1) {
      int y = yi;
      uint8_t* row = fb + (size_t)y * w;
      int dy = 2 * y - (h - 1);
      int yterm = dy * dy * 256 / h1sq;
      int row_off = crt_strike_offset(y, flash_phase);

      // Capture this row while it is still raw, and (if not done on this row)
      // one step ahead in traversal direction — the CA stage reads ±1 rows on
      // both halves and the ring can only hold what traversal has seen.
      memcpy(s_vraw_ring[y % 3], row, w);
      int y_next = pass == 0 ? y + 1 : y - 1;
      // Lookahead stays inside this pass's own half: the other half's rows may
      // already carry processed output by traversal time (ring reads raw).
      bool owns_next = pass == 0 ? y_next <= (h - 1) / 2 : y_next > (h - 1) / 2;
      if (owns_next && y_next >= 0 && y_next < h) {
        memcpy(s_vraw_ring[y_next % 3], fb + (size_t)y_next * w, w);
      }

      // 1) CA from raw sources: horizontal fringe takes a two-tap weighted
      //    sample per shifted channel (thirds-of-a-px pull); vertical
      //    fringe samples ring rows toward/away from the centreline.
      for (int x = 0; x < w; x++) {
        bool left = x * 2 < w - 1;
        // Horizontal pull in THIRDS: the rung (0/4/8), plus the element-offset
        // correction and the strike boost in whole pixels. The correction does
        // NOT mirror: the RGB stripe puts R 1/3px left (B 1/3px right) of its
        // pixel centre on BOTH halves, so the sign follows the half, not the
        // pull direction — folding a flat -1 into q before the tap mirror
        // (old form) carried a 2/3px residual splay through the right half,
        // measured on hardware. The cancellation makes the dead zone actually
        // converge: at the centre the rasters land on the content.
        int q = crt_ca_t3_h2(s_ca_xq[x] + yterm) + (left ? -1 : 1) + 3 * ca_boost;
        int j = (q - (q < 0 ? 2 : 0)) / 3;  // floor(q/3); q >= -1 by construction
        int f = q - 3 * j;                  // 0..2 — all three occur across current rungs
        int s_v = crt_ca_shift_v2(s_ca_xq[x] + yterm);

        bool top = y * 2 < h - 1;
        // Sampling signs: left half pulls R from the right; top half pulls R
        // from below. Mirrors across both centrelines. Forward reads (below on
        // the top half, above on the bottom half) rely on the ring capture
        // being one row ahead in traversal direction.
        int rt0 = left ? x + j : x - j;
        int rt1 = left ? x + j + 1 : x - j - 1;
        int bt0 = left ? x - j : x + j;
        int bt1 = left ? x - j - 1 : x + j + 1;
        int ry = top ? y + s_v : y - s_v;
        int by = top ? y - s_v : y + s_v;
        // The ring only holds rows this pass has captured; at the half
        // boundary the forward tap's row belongs to the OTHER pass (its slot
        // carries a 3-rows-stale capture — pass 0 at y=113 would read row
        // 111). Fall back to the own row; only the forward read can cross.
        if (pass == 0 && ry > (h - 1) / 2) ry = y;
        if (pass == 1 && ry <= (h - 1) / 2) ry = y;
        // Both taps of a channel come from the same (valid) ring ROW; clamp
        // the COLUMNS — at max strike the right half reaches q = 8+1+9 = 18
        // (rung + sign fix + 3·boost) → j = 6, and the taps land on the same
        // edge column and the weighted sample degenerates to a plain copy.
        if (rt0 < 0) rt0 = 0;
        if (rt0 >= w) rt0 = w - 1;
        if (rt1 < 0) rt1 = 0;
        if (rt1 >= w) rt1 = w - 1;
        if (bt0 < 0) bt0 = 0;
        if (bt0 >= w) bt0 = w - 1;
        if (bt1 < 0) bt1 = 0;
        if (bt1 >= w) bt1 = w - 1;
        // The ring holds rows y-2..y+2 at most; s_v stays within 1 today.
        // Both halves only read rows already captured into the ring.
        if (ry < 0) ry = 0;
        if (ry >= h) ry = h - 1;
        if (by < 0) by = 0;
        if (by >= h) by = h - 1;

        const uint8_t* rrow = s_vraw_ring[ry % 3];
        const uint8_t* brow = s_vraw_ring[by % 3];
        // Fractional channel sample: sum of both taps weighted (3-f, f),
        // rounded to nearest — plain truncation biased R/B down half a level
        // on average, a faint green cast on mid-tones (G is never blended).
        // A 0<->3 edge yields exactly 2 and 1; weights act on code values,
        // linear in light here (the panel makes levels by area fill) — no
        // gamma round trip.
        int r = ((3 - f) * ((rrow[rt0] >> 4) & 3) + f * ((rrow[rt1] >> 4) & 3) + 1) / 3;
        int b = ((3 - f) * (brow[bt0] & 3) + f * (brow[bt1] & 3) + 1) / 3;
        row_ca[x] = GCOLOR8_ALPHA | (uint8_t)(r << 4) | (row[x] & 0x0C) | (uint8_t)b;
      }

      // 2) Vignette + dither on the CA'd row — dimming in content space, so
      //    the curvature pass below bends the already-darkened rim with the
      //    image. Mirror-symmetric Bayer thresholds keep both rims identical.
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

      // 3) Curvature (+ strike jitter): dest (x,y) samples source column
      //    sx = cx + (x - cx)·M(x,y)/65536 with the radial M of crt_warp_q16 —
      //    recomposed here from the CA stage's cached terms — then the strike
      //    slides the whole row sideways. M grows with x too, so integer
      //    quantization boundaries curve instead of aligning into seams.
      //    sn's +half cancels exactly in S below: stage 3 no longer rounds —
      //    floor(S) + fraction IS the warp position. Rim symmetry survives
      //    without rounding: floor+frac is pointwise symmetric in dx (the
      //    floor(−a) vs floor(a) ±1 phases sit under the vignette's black
      //    rim). The fraction then blends the column pair instead of picking
      //    one: magnification decimates source columns under nearest-
      //    neighbour (every damaged 8px letter cell measured 7px wide), and
      //    keeping their energy as fractional levels reads as tube edge
      //    defocus while letters keep their rhythm. Rounding (+128) avoids a
      //    truncation bias toward dark.
      for (int x = 0; x < w; x++) {
        int dx = 2 * x - (w - 1);
        int mul16 = 65536 + CRT_WARP_R2_K * (s_ca_xq[x] + yterm);
        int sn = (w - 1) * 65536 + dx * mul16 + 65536;  // sx*131072 + half
        int S = sn - 65536;                             // exact warp position, Q17
        int sx = (S >> 17) + row_off;                   // strike stays whole-pixel
        if (sx < 0 || sx >= w) {
          row[x] = GCOLOR8_OPAQUE_BLACK;
          continue;
        }
        int fr = (S >> 9) & 0xFF;  // 0..255, weight toward sx+1
        int sx1 = sx + 1;
        if (sx1 >= w) sx1 = w - 1;
        uint8_t p0 = row_ca[sx], p1 = row_ca[sx1];
        int r = ((256 - fr) * ((p0 >> 4) & 3) + fr * ((p1 >> 4) & 3) + 128) >> 8;
        int g = ((256 - fr) * ((p0 >> 2) & 3) + fr * ((p1 >> 2) & 3) + 128) >> 8;
        int b = ((256 - fr) * (p0 & 3) + fr * (p1 & 3) + 128) >> 8;
        row[x] = GCOLOR8_ALPHA | (uint8_t)((r << 4) | (g << 2) | b);
      }
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
  // Strike trigger lives at the END of the wake frame (not the backlight
  // handler): the sound starts once the ring can survive what follows.
  if (s_strike_pending) {
    s_strike_pending = false;
    crt_play_strike_sound();
    s_flash_phase = 0;
    if (s_crt_layer) layer_mark_dirty(s_crt_layer);
    s_flash_timer = app_timer_register(CRT_FLASH_TICK_MS, crt_flash_tick, NULL);
  }
}

static void crt_flash_tick(void* data) {
  (void)data;
  s_flash_phase++;
  if (s_flash_phase >= CRT_FLASH_PHASES) {
    s_flash_phase = CRT_FLASH_IDLE;
    s_flash_timer = NULL;
  }
  if (s_crt_layer) layer_mark_dirty(s_crt_layer);
  // One-shot timers free themselves on firing.
  if (s_flash_phase != CRT_FLASH_IDLE) {
    s_flash_timer = app_timer_register(CRT_FLASH_TICK_MS, crt_flash_tick, NULL);
  }
}

void crt_backlight_handler(bool on) {
  // Backlight-only gating: the flash is the strike of a tube warming up;
  // backlight-off transitions and a disabled effect start nothing.
  if (!on || !s_settings_crt) return;
  // Prime, don't strike: the overlay proc starts sound + chain after the wake
  // frame. A mid-strike backlight-on cancels the old chain and re-primes.
  if (s_flash_timer) {
    app_timer_cancel(s_flash_timer);
    s_flash_timer = NULL;
  }
  s_flash_phase = CRT_FLASH_IDLE;
  s_strike_pending = true;
  if (s_crt_layer) layer_mark_dirty(s_crt_layer);
}

// The degauss woomp as synthesized PCM: note-table playback clicks at every
// note boundary (pitch and velocity step), so the hum is generated instead —
// a pitch-gliding sine (≈90 → 55 Hz) under a swell-and-decay amplitude
// envelope. Continuous by construction. 320ms at 16 kHz 16-bit sits well
// under SPEAKER_MAX_SAMPLE_BYTES_TOTAL. Synthesized once on first use.
#define CRT_STRIKE_PCM_MS \
  110  // <= stock FW's 128ms ring: the sound is a thunk, frames can't starve it
#define CRT_STRIKE_PCM_RATE 16000
#define CRT_STRIKE_PCM_SAMPLES (CRT_STRIKE_PCM_MS * CRT_STRIKE_PCM_RATE / 1000)
static int16_t s_strike_pcm[CRT_STRIKE_PCM_SAMPLES];
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
  // Thunk, not hum. Stock firmware's audio ring holds 128ms and every render
  // frame blocks the loop ~60ms (measured 2026-08-23; the refill callback is
  // droppable under KernelMain load), so the strike sound must fit inside one
  // ring: 110ms. It also wants the band the speaker reproduces — the old
  // 90→55Hz hum put 100% of its energy under 300Hz, mostly inaudible. The
  // coil's energisation IS the perceptible event: constant 190 Hz + partials
  // 380/570/760 at 4:3:2:1, ~5.5ms attack, exponential decay (thermistor
  // shape), last 64 samples silent so the codec never cuts mid-cycle.
  const size_t attack = n / 20;
  uint32_t phase = 0;
  int env = 32767;  // Q15, thermistor decay: env -= env>>10 per sample
  for (size_t i = 0; i < n; i++) {
    phase += (uint32_t)((190u * 65536) / CRT_STRIKE_PCM_RATE);
    int y = (4 * crt_wave_q15(phase) + 3 * crt_wave_q15(phase * 2) + 2 * crt_wave_q15(phase * 3) +
             crt_wave_q15(phase * 4)) /
            10;
    int gain = i < attack ? (int)(i * 256 / attack) : 256;
    if (i + 64 >= n) gain = 0;
    buf[i] = (int16_t)(((int64_t)y * gain * env * 30000) >> 38);
    if (env && gain && i >= attack) env -= env >> 10;
  }
}

#define CRT_STRIKE_VOLUME 90

void crt_play_strike_sound(void) {
  // speaker_is_muted covers the system mute preference (including Quiet
  // Time-mutes-speaker when the user set it). Honor it, and our own toggle.
  if (!s_settings_crt_sound || speaker_is_muted()) return;
  if (!s_strike_pcm_ready) {
    crt_strike_synth(s_strike_pcm, CRT_STRIKE_PCM_SAMPLES);
    s_strike_pcm_ready = true;
  }
  // Audio underrun notes (hardware-measured 2026-08-23): the driver ring is
  // 128ms and each strike frame blocks the main loop ~95ms, so refill
  // callbacks lose the race and the woomp gaps. The stream API that would
  // let the app top the ring itself only exists from FW 4.33.2 — older stock
  // firmware HARD-FAULTS on the missing syscall (fallback-by-return-false is
  // impossible), so the old track path stays until those builds die out.
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
  speaker_play_tracks(&track, 1, CRT_STRIKE_VOLUME);
}

void crt_apply_setting_change(void) {
  if (s_settings_crt) {
    if (s_crt_layer) layer_mark_dirty(s_crt_layer);
    return;
  }
  s_flash_phase = CRT_FLASH_IDLE;
  s_strike_pending = false;  // a primed-but-unstarted strike dies with the toggle
  // Mid-strike toggle-off: kill the armed chain, or the next tick re-arms
  // from IDLE into a full strike nobody asked for.
  if (s_flash_timer) {
    app_timer_cancel(s_flash_timer);
    s_flash_timer = NULL;
  }
  // Off must ERASE, not just stop painting: the shader's pixels live in the
  // shared framebuffer and the next paint of anything underneath only happens
  // at the minute edge. Re-applying the window background dirties the whole
  // tree now — canvas and clock repaint, the overlay (disabled) adds nothing.
  if (s_main_window) window_set_background_color(s_main_window, s_active_theme->center_bg);
  if (s_crt_layer) layer_mark_dirty(s_crt_layer);
}
