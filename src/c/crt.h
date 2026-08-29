#pragma once
#include <pebble.h>
#include "data.h"

// The CRT effect lives entirely here: a full-screen overlay layer stacked
// above the clock whose update proc captures the framebuffer (the public
// graphics_capture_frame_buffer API) and runs a per-pixel pass — a software
// "shader". Stage order: 1) chromatic aberration, 2) vignette + dither (in
// content space, so the darkening bends WITH the curvature; light-background
// themes take a steeper ease-in LUT — dot density over shade, black only at
// the rim — and hold ink one step clear of the level their field renders as,
// which a plain gain quantizes away), 3) curvature — the pincushion warp.
// Each axis maps its edge distance onto the LUTs' 0..16 domain, so
// CRT_VIGNETTE_SIDE_PX and CRT_VIGNETTE_BAND_PX size the four rims on their
// own. The degauss strike's row jitter rides the warp's sampling stage. The
// toggle gates the pass; off means the proc returns before capturing, so the
// effect costs exactly nothing.
//
// main.c owns the layer's lifecycle (create/stack/destroy), like the canvas;
// the handle is mirrored here the way drawing.h mirrors s_canvas_layer.
extern Layer* s_crt_layer;

// Curvature geometry. The warp magnifies radially — dest (x,y) samples
// source at cx + (x-cx)·M/65536 with M = 65536 + CRT_WARP_R2_K·r² (r² = the
// elliptical Q8 xq+yq the CA zones also use). K=8 ≈ 3.1px of pull at a
// mid-edge (r²=256), smoothly more toward the corners. The vignette darkens
// within a band of the nearest edge, counted around the corner arcs.
//
// K also sets how much solid black the sides carry, which is not obvious: the
// outermost dest columns' sources run off the frame and clamp to column 0,
// which the vignette has already taken to depth 0. K=12 clamped four of them,
// and with the depth-0 and depth-1 columns behind it that was 6px of black
// before the falloff got a single pixel — no vignette constant could touch it.
// K=8 clamps three. Lower K is a flatter tube, so this trades bow for rim.
//
// VIGNETTE_PX is the falloff LUTs' domain — 17 sample points, depth 0..16 —
// not a size. Both axes map their edge distance onto that domain, so the two
// BAND constants below set how far the rim actually reaches without touching
// the tuned curves. Shrink or grow the rims there and nowhere else.
#define CRT_VIGNETTE_PX 16
// Side band, in CONTENT px. The warp COMPRESSES the rim inward rather than
// stretching it — 13 source columns land in 12 dest — so this is very nearly
// its own width on the glass, sitting behind the clamp below.
//
// It sets the width of the RAMP, not of the black: the clamp's columns do not
// move with it at all. Judge it with the per-pixel view, never by mean level —
// a mean saturates once a column reaches the field, so it cannot show the ramp
// lengthening at all, and reading one produced a wrong "this constant is
// inert" conclusion that stood here for a while.
//
// On a level-2 field, 12 renders the ladder 1,3,4,5,6 — that jump from 1 to 3
// is the sharp edge coming out of black. 16 renders 1,2,3,4,5,6, every step
// present. Wider still works now that the channels dither out of phase; before
// that, quantization dips punched black columns back through the middle of the
// ramp and made anything past 12 look worse.
#define CRT_VIGNETTE_SIDE_PX 16
// Top/bottom band, in DISPLAY px — the warp is horizontal-only, so nothing
// resamples this axis and it is the figure you see. Sized so both axes reach
// full brightness at the same offset (measured: dest 9 on each).
#define CRT_VIGNETTE_BAND_PX 16
// Rows held at depth 0 before the falloff starts, standing in for the columns
// the warp's clamp renders solid on the sides. Nothing clamps vertically, so
// without it the top carried 2px of black against the sides' 6 and read as
// missing. Track it to the clamp: at K=8 that is three each way.
#define CRT_VIGNETTE_EDGE_PX 3
// Gain below which the falloff is bezel, not content — the light LUT's depths
// 0-3, where the field is crushed and nothing is meant to be read. Ink/field
// separation (crt.c's clear_of_ground) stops here so the rim can reach black.
#define CRT_VIGNETTE_READ_Q8 128
#define CRT_CORNER_RADIUS 14
#define CRT_WARP_R2_K 8
// CA zones by elliptical radius (Q8 of r²-summated terms; mid-edges ≈ 256,
// corner ≈ 362). r < R2: clean; beyond: a flat 1+1/3px per channel —
// expressed in thirds (0/4) because stage 1 samples fractionally, and a
// per-half ±1 third in the pass cancels the panel's built-in element offset
// (the stripe does not mirror: −1 left, +1 right) so the centre of the screen
// converges instead of carrying a 2/3px floor.
// Vertical uses one threshold (R2V) with a 1px cap, whole pixels still.
#define CRT_CA_R2_Q8 170
#define CRT_CA_R2V_Q8 170

// Squared thresholds for the zone compare — no per-pixel sqrt needed.
#define CRT_CA_R2_X2Q8 ((CRT_CA_R2_Q8 * CRT_CA_R2_Q8 + 255) >> 8)
#define CRT_CA_R2V_X2Q8 ((CRT_CA_R2V_Q8 * CRT_CA_R2V_Q8 + 255) >> 8)

// The wake-up: a degauss strike. On backlight-on, FRAMES 90ms ticks of
// per-row horizontal jitter with decaying amplitude plus amplified channel
// separation — the CRT's shadow-mask demagnetization wobble.
#define CRT_FLASH_TICK_MS 90
#define CRT_FLASH_PHASES 8
#define CRT_FLASH_IDLE (-1)

// Pure pieces, unit-tested (all integer, fixed point):

// Q8 brightness factor at (x,y): 256 = untouched, 0 = black. Vignette
// falloff, smoothstepped, idle-only — the strike shakes geometry, not gain.
int crt_vignette_q8(int x, int y, int w, int h);
// Row x-offset during strike frame `flash_phase`: 0 when idle. Amplitude
// decays to 0 across phases; deterministic per (y, phase).
int crt_strike_offset(int y, int flash_phase);
// Radial warp magnification in Q16 at (x,y): 65536 = identity, growing with
// the elliptical squared radius. Side content warps too (deliberate — a
// curved tube bows on all edges); the outermost ~4 columns' sources run off
// the frame at mid-height and replicate the edge column — which the vignette
// has already darkened to the rim's level anyway.
int crt_warp_q16(int x, int y, int w, int h);
// Source column for dest (x,y) under the warp alone (no strike jitter).
int crt_warp_sx(int x, int y, int w, int h);
// CA per-channel displacement in THIRDS of a px at (x,y): 0 inside the dead
// zone, 4 past it — see crt.h's CRT_CA_R*_Q8 zone map.
int crt_ca_shift3(int x, int y, int w, int h);

// The whole pass over an 8-bit GColor8 framebuffer (0xAARRGGBB packed bytes,
// row-major, w*h). Touch only via these: tests drive it with a mock buffer.
void crt_apply_framebuffer(uint8_t* fb, int w, int h, int flash_phase);

void crt_update_proc(Layer* layer, GContext* ctx);
void crt_backlight_handler(bool on);

// Synthesises the degauss woomp to PCM once, then plays it via the speaker
// API. Silent when the speaker is muted by system preference (including
// Quiet Time when the user set it to mute), or while the face is covered by
// a notification/alarm/modal/quick view — see the crt_app_focus_* handlers.
void crt_play_strike_sound(void);
// App Focus service callbacks (subscribed in main.c): track whether
// anything covers the face. They gate the strike sound only; the visual
// strike runs regardless.
void crt_app_focus_will_handler(bool in_focus);
void crt_app_focus_did_handler(bool in_focus);
// the overlay dirty; turning off repaints — the OFF frame is painted by the
// canvas/clock underneath once the background re-dirties, the overlay then
// paints nothing over it.
void crt_apply_setting_change(void);
