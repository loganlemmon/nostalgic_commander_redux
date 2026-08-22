# CRT Weighted Fractional CA (Subpixel Thirds) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the CRT pass's whole-pixel horizontal chromatic-aberration zones with fractional (thirds-of-a-pixel) weighted sampling, so the fringe displacement is continuous and zero at the screen centre instead of banded at 0/1/2 px rungs with a 2/3 px floor.

**Architecture:** Stage 1 of `crt_apply_framebuffer` (src/c/crt.c) stops copying a whole neighbour channel and instead takes a two-tap weighted horizontal sample per shifted channel (R and B; G stays the untapped reference). Zone boundaries keep their existing squared-radius form; only the rung values change (0/3/6 thirds instead of 0/1/2 px) and a constant −1 third cancels the panel's built-in element offset. Strike `ca_boost` stays in whole pixels, added as `3*boost` thirds. Vertical CA, vignette, warp and strike geometry are untouched. Aperture grille and subpixel text are explicitly out of scope (probe session narrowed the proposal to the CA fringe only).

**Tech Stack:** Pebble C SDK code compiled/emulated via `pebble` CLI; host unit tests in `test/test_watchface.c` (Unity, includes `../src/c/crt.c` directly, so `static` functions and globals are visible to tests); `make test` = format-check + JS tests + C suite; `make visual-baseline`/`visual-check` = emery-emulator screenshot gate.

**Spec:** docs/superpowers/specs/2026-08-22-crt-subpixel-design.md

## Global Constraints

- Scope is the CA fringe (stage 1) only. Spec's stage 2 ("Vertical CA — no change"), stage 3 ("Curvature — do not use subpixel") hold: touch neither. Stage 4 (aperture grille) is NOT implemented — the 2026-08-22 hardware session narrowed the proposal; the grille proceeds (if ever) only after the fringe A/Bs on hardware. No scanlines, no subpixel text, no gamma round trip.
- Zone boundaries stay in squared-radius form; the `CRT_CA_R2_Q8`/`CRT_CA_R3_Q8`/`CRT_CA_R2V_Q8` macros and their `*_X2Q8` squared variants are unchanged.
- 0↔3 transitions must yield levels exactly 2 and 1 ("a real centroid shift with no duplication and no periodic pattern").
- `ca_boost` is in whole pixels, not thirds; at max (`s = 2 + boost 3 = 5`) both taps of a channel clamp to the same edge column and the weighted sample degenerates to a plain copy.
- No gamma round trip before weighting: the probe confirmed the panel's levels are spatial area-fill, so code values are (approximately) linear in emitted light and are weighted directly.
- `ComplicationDataSource` enum and `PERSIST_KEY_*` values are stable identifiers — this change touches neither.
- VCS: `jj` only, read-only except the already-created working change (`jj new` was run by the parent). NO per-task commits, NO `jj describe` changes, NO git. Test gates per task stand in for per-task commits.
- Style: match crt.c's existing dense-comment register (comments explain *why*, with the arithmetic inline). Run `make format` if clang-format complains.
- Tests live in `test/test_watchface.c`; register each new test with `RUN_TEST(...)` next to the other CRT tests (~line 4154-4168). The mock framebuffer is `mock_framebuffer` (200x228 GColor8); `s_fake_ctx` is any stable pointer; set `s_settings_crt = 1` and `s_flash_phase` explicitly in every new framebuffer test.
- The pass runs on an MCU: integer arithmetic only, no division by non-constant without reason (`/3` by literal is fine — it compiles to multiply-shift). No libm.

---

### Task 1: Zone rungs in thirds (pure geometry)

**Files:**
- Modify: `src/c/crt.h` (zone comment + `crt_ca_shift` declaration)
- Modify: `src/c/crt.c` (`crt_ca_shift_h2` → `crt_ca_t3_h2`, `crt_ca_shift` → `crt_ca_shift3`)
- Test: `test/test_watchface.c` (rewrite two named tests, add ladder test)

**Interfaces:**
- Consumes: nothing new; zone macros from crt.h unchanged.
- Produces: `int crt_ca_shift3(int x, int y, int w, int h)` — per-channel horizontal displacement in THIRDS of a pixel (0 inside the dead zone, 3 mid, 6 toward corners), replacing `int crt_ca_shift(...)`. `static int crt_ca_t3_h2(int xy_sum)` — same thresholds, returns 0/3/6. Task 2's pass loop consumes `crt_ca_t3_h2`.

- [ ] **Step 1: Rewrite the two spec-named tests in thirds + add the ladder test (failing)**

In `test/test_watchface.c`, replace the body of `test_crt_ca_onset_should_cut_at_the_dead_zone`:

```c
void test_crt_ca_onset_should_cut_at_the_dead_zone(void) {
  // Centre never fringes (separation 0, not the old 2/3px floor); corners
  // split the full 4px (6 thirds per channel).
  TEST_ASSERT_EQUAL_INT(0, crt_ca_shift3(100, 113, 200, 228));
  TEST_ASSERT_EQUAL_INT(6, crt_ca_shift3(0, 0, 200, 228));
  TEST_ASSERT_EQUAL_INT(6, crt_ca_shift3(199, 227, 200, 228));
  // Mid-edge cells land in the middle band (1px = 3 thirds), monotone out.
  TEST_ASSERT_EQUAL_INT(3, crt_ca_shift3(0, 113, 200, 228));
  TEST_ASSERT_TRUE(crt_ca_shift3(190, 113, 200, 228) >= crt_ca_shift3(160, 113, 200, 228));
}
```

In `test_crt_pure_geometry_should_match_the_spec`, replace the CA block:

```c
  // CA: zero at the centre, 6 thirds (2px per channel) at the corners,
  // monotone along x.
  TEST_ASSERT_EQUAL_INT(0, crt_ca_shift3(100, 113, 200, 228));
  TEST_ASSERT_EQUAL_INT(6, crt_ca_shift3(0, 0, 200, 228));
  TEST_ASSERT_EQUAL_INT(6, crt_ca_shift3(199, 227, 200, 228));
  TEST_ASSERT_TRUE(crt_ca_shift3(190, 113, 200, 228) >= crt_ca_shift3(160, 113, 200, 228));
```

Add the element-ladder test right after `test_crt_ca_onset_should_cut_at_the_dead_zone`:

```c
void test_crt_ca_ladder_should_be_monotone_and_mirror_symmetric(void) {
  // Thirds ladder: displacement grows with radius and mirrors cleanly about
  // both centrelines (the pass derives left/right and top/bottom pull signs
  // from the half, so any asymmetry here doubles at the seam).
  for (int y = 0; y < 228; y++) {
    int prev = crt_ca_shift3(0, y, 200, 228);
    for (int x = 0; x < 200; x++) {
      int s = crt_ca_shift3(x, y, 200, 228);
      TEST_ASSERT_TRUE(s == 0 || s == 3 || s == 6);
      TEST_ASSERT_EQUAL_INT(s, crt_ca_shift3(199 - x, y, 200, 228));
      TEST_ASSERT_EQUAL_INT(s, crt_ca_shift3(x, 227 - y, 200, 228));
      if (x <= 100) {
        TEST_ASSERT_TRUE(s <= prev || !"monotone toward the rim");
        prev = s;
      } else {
        TEST_ASSERT_TRUE(s >= prev);
        prev = s;
      }
    }
    prev = prev;  // (no-op; keeps prev live across the row)
  }
}
```

Register the new test in `main()` after `RUN_TEST(test_crt_ca_onset_should_cut_at_the_dead_zone);`:

```c
  RUN_TEST(test_crt_ca_ladder_should_be_monotone_and_mirror_symmetric);
```

- [ ] **Step 2: Run to verify failure**

Run: `make -C test test 2>&1 | tail -20`
Expected: compile error — `crt_ca_shift3` undeclared (RED by rename, which is fine; the runner must not pass).

- [ ] **Step 3: Implement the thirds rungs**

In `src/c/crt.c`, rename and re-value the rung function (keep the comment's squared-threshold explanation, update units):

```c
// Horizontal CA rung from the squared Q8 radius (units: xq + yq from
// crt_ca_shift3), returning per-channel displacement in THIRDS of a pixel:
// 0 / 3 / 6 = 0 / 1 / 2 px. Thirds because the pass now samples channels
// fractionally (two weighted taps) instead of copying whole neighbours —
// see stage 1 in crt_apply_framebuffer. Zone boundaries squared once:
//   rq ≥ R ⇔ xq+yq ≥ ceil(R²/256) — so no per-pixel sqrt is needed.
static int crt_ca_t3_h2(int xy_sum) {
  return xy_sum < CRT_CA_R2_X2Q8 ? 0 : (xy_sum < CRT_CA_R3_X2Q8 ? 3 : 6);
}
```

Rename the exported pure function and update its body (keeping the xq/yq computation):

```c
int crt_ca_shift3(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  int dy = 2 * y - (h - 1);
  // Elliptical radius from the centre, Q8: corner ≈ 362, mid-edge = 256.
  int xq = (dx * dx * 256) / ((w - 1) * (w - 1));
  int yq = (dy * dy * 256) / ((h - 1) * (h - 1));
  return crt_ca_t3_h2(xq + yq);
}
```

The pass body still calls `crt_ca_shift_h2` — update that one call site to `crt_ca_t3_h2` as part of Step 3, choosing Task 2's integration shape now so the tree compiles: in the stage-1 loop replace `int s_h = crt_ca_shift_h2(s_ca_xq[x] + yterm) + ca_boost;` with `int s_h = (crt_ca_t3_h2(s_ca_xq[x] + yterm) + 1) / 3 + ca_boost;` — a temporary whole-pixel projection that keeps Task 1's diff behaviour-free within the pass (all current rungs map back to the old 0/1/2 sampling). Task 2 replaces it with the real fractional sampler.

In `src/c/crt.h`, update the zone comment and the declaration:

```c
// CA zones by elliptical radius (Q8 of r²-summated terms; mid-edges ≈ 256,
// corner ≈ 362). r < R2: clean; R2..R3: 1px; beyond R3: 2px per channel —
// expressed in thirds (0/3/6) because stage 1 samples fractionally, and a
// constant −1 third in the pass cancels the panel's built-in element offset
// so the centre of the screen converges instead of carrying a 2/3px floor.
// Vertical uses one threshold (R2V) with a 1px cap, whole pixels still.
```
(Keep the three `#define CRT_CA_R*_Q8` untouched.)

```c
// CA per-channel displacement in THIRDS of a px at (x,y): 0 inside the dead
// zone, 3 or 6 toward the edge — see crt.h's CRT_CA_R*_Q8 zone map.
int crt_ca_shift3(int x, int y, int w, int h);
```

- [ ] **Step 4: Run tests**

Run: `make -C test test 2>&1 | tail -20`
Expected: PASS, including the rewritten blocks and the ladder (the temporary `(t3+1)/3` projection preserves old pass behaviour: t3∈{0,3,6} → (1/3)=0, (4/3)=1, (7/3)=2).

- [ ] **Step 5: Format check**

Run: `make format-check`
Expected: PASS; else `make format` and re-run Step 4.

---

### Task 2: Weighted fractional sampling in stage 1

**Files:**
- Modify: `src/c/crt.c` (stage-1 loop in `crt_apply_framebuffer`, ~lines 152-176)
- Test: `test/test_watchface.c` (zero-point, strike-stack, edge-clamp, no-duplication tests)

**Interfaces:**
- Consumes: `static int crt_ca_t3_h2(int xy_sum)` from Task 1 (returns 0/3/6).
- Produces: no new interface. Stage-1 inline arithmetic per destination x:
  - `q` = pull distance in thirds = `crt_ca_t3_h2(sum) - 1 + 3 * ca_boost` (the −1 cancels the R-element-left/B-element-right built-in 1/3 px offset; G, centred at the element position, stays untapped).
  - `j` = floor(q/3), `f` = q − 3j ∈ {0,1,2} (2 for every current rung).
  - Left half: R taps `x+j`,`x+j+1`; B taps `x−j`,`x−j−1`. Right half mirrored.
  - Weighted channel: `( (3−f)*v(tap0) + f*v(tap1) ) / 3`, v = the channel's 2-bit level from the vertical-CA ring row. All four tap columns clamped to [0,w) individually.
  - R reads ring row `ry`, B reads ring row `by` (both taps of a channel from the SAME ring row — vertical CA unchanged, ring memcpys unchanged).

- [ ] **Step 1: Write the four new framebuffer tests (failing)**

Insert after `test_crt_ca_should_pull_red_from_the_left`, then register each with `RUN_TEST` in `main()`. That pre-existing bar test must survive UNCHANGED — the new geometry still gives its dest(22,109) an R level of exactly 2 (`(0 + 2*3)/3`); if it fails, the implementation is wrong, not the test.

```c
void test_crt_ca_zero_point_should_mirror_ghosts_about_the_line(void) {
  // Dead-zone correction pins separation to 0: a 1px white line at the
  // centre becomes symmetric {2,1} fringes — red ghost shifts right, blue
  // left, G full. The old whole-pixel sampling left it untouched (2/3px of
  // fringe nobody asked for).
  s_settings_crt = 1;
  s_flash_phase = CRT_FLASH_IDLE;
  memset(mock_framebuffer, 0xC0, sizeof(mock_framebuffer));
  for (int y = 112; y <= 114; y++) mock_framebuffer[y * 200 + 60] = 0xFF;
  crt_update_proc(NULL, s_fake_ctx);

  uint8_t* row = &mock_framebuffer[113 * 200];
  TEST_ASSERT_EQUAL_HEX8(2, (row[60] >> 4) & 3);  // R weighted at the line
  TEST_ASSERT_EQUAL_HEX8(1, (row[61] >> 4) & 3);  // R ghost shifts right
  TEST_ASSERT_EQUAL_HEX8(3, (row[60] >> 2) & 3);  // G untouched
  TEST_ASSERT_EQUAL_HEX8(2, row[60] & 3);         // B weighted at the line
  TEST_ASSERT_EQUAL_HEX8(1, row[59] & 3);         // B ghost shifts left
  TEST_ASSERT_EQUAL_HEX8(0, (row[59] >> 4) & 3);  // no red left of the line
  TEST_ASSERT_EQUAL_HEX8(0, row[61] & 3);         // no blue right of it
}
```
((60,113) is dead zone: squared sum ≈ 40 < CRT_CA_R2_X2Q8 = 113, so t3=0, q=−1, j=−1, f=2; s_v=0; vignette depth 60 → full; warp inset 0 at y=113 → identity.)

```c
void test_crt_strike_should_stack_ca_boost_in_whole_pixels(void) {
  // ca_boost is whole PIXELS added as 3*boost thirds: at phase 0 (amp 6,
  // boost 3) a dead-zone line's red motif lands exactly 3px left — not 1/3.
  // Reading boost as thirds would silently cut the strike's splay to a
  // third of its amplitude; this is the pin against that.
  s_settings_crt = 1;
  s_flash_phase = 0;
  memset(mock_framebuffer, 0xC0, sizeof(mock_framebuffer));
  for (int y = 112; y <= 114; y++) mock_framebuffer[y * 200 + 60] = 0xFF;
  crt_update_proc(NULL, s_fake_ctx);

  int off = crt_strike_offset(113, 0);  // stage-3 row jitter rids with it
  uint8_t* row = &mock_framebuffer[113 * 200];
  TEST_ASSERT_EQUAL_HEX8(2, (row[57 - off] >> 4) & 3);
  TEST_ASSERT_EQUAL_HEX8(1, (row[58 - off] >> 4) & 3);
  TEST_ASSERT_EQUAL_HEX8(0, (row[59 - off] >> 4) & 3);
  TEST_ASSERT_EQUAL_HEX8(2, row[63 - off] & 3);
  TEST_ASSERT_EQUAL_HEX8(1, row[62 - off] & 3);
}
```
(q = 0−1+3\*3 = 8 → j=2, f=2: R samples x+2(w1)/x+3(w2) → motif {2 at c−3, 1 at c−2}; B mirrored. s_v=0 at this radius. Stage-3 output col = motif col − off.)

```c
void test_crt_strike_max_shift_should_clamp_both_taps_at_the_edge(void) {
  // Max shift (t3=6 + boost 3 → q=14, j=4, f=2): at the right edge both
  // blue taps overshoot and must clamp to x=199 — degenerating to a plain
  // copy of the edge column. A blue level of 1 at x=196 (through vignette
  // f=15, Bayer t=15: (3*16*15>>8 + 15)>>4 = 1) is only possible if BOTH
  // taps read the white edge column; a wrapped or unclamped read yields 0.
  s_settings_crt = 1;
  s_flash_phase = 0;
  memset(mock_framebuffer, 0xC0, sizeof(mock_framebuffer));
  for (int y = 111; y <= 115; y++) mock_framebuffer[y * 200 + 199] = 0xFF;
  crt_update_proc(NULL, s_fake_ctx);

  // Scan [188,199] (stage-3 jitter slides the motif within ±6px).
  bool saw_edge_blue = false;
  for (int x = 188; x <= 199; x++) {
    if ((mock_framebuffer[113 * 200 + x] & 3) >= 1) saw_edge_blue = true;
  }
  TEST_ASSERT_TRUE(saw_edge_blue);
  // No wraparound to the far side of the row.
  for (int x = 0; x <= 5; x++) {
    TEST_ASSERT_EQUAL_HEX8(0xC0, mock_framebuffer[113 * 200 + x]);
  }
}
```
((196,113): right half, sum = 240 → t3=6 ✓, s_v=1 → B reads ring row y−1=112 which carries the white edge ✓.)

```c
void test_crt_ca_should_never_split_a_feature_into_two_ghosts(void) {
  // The regression test against reintroducing shift DITHERING along x: with
  // weighted taps a 1px feature appears exactly once per channel — one
  // contiguous run of nonzero R and one of nonzero B, ≤3px wide. Dithering
  // the shift by column would double a ghost at full level 2px away.
  // Row 20 (5px-tall line, ring covers s_v=1) spans all three rungs as the
  // line column sweeps 20..179; vignette is full there (depth 20).
  s_settings_crt = 1;
  s_flash_phase = CRT_FLASH_IDLE;
  for (int c = 20; c <= 179; c++) {
    memset(mock_framebuffer, 0xC0, sizeof(mock_framebuffer));
    for (int y = 18; y <= 22; y++) mock_framebuffer[y * 200 + c] = 0xFF;
    crt_update_proc(NULL, s_fake_ctx);

    uint8_t* row = &mock_framebuffer[20 * 200];
    for (int ch = 0; ch < 2; ch++) {  // 0 = R, 1 = B
      int runs = 0;
      int run_start = -1, run_end = -1;
      bool in = false;
      for (int x = 10; x <= 189; x++) {
        int v = ch == 0 ? (row[x] >> 4) & 3 : row[x] & 3;
        bool lit = v > 0;
        if (lit && !in) { runs++; run_start = x; in = true; }
        if (!lit && in) { run_end = x - 1; in = false; }
      }
      if (in) run_end = 189;
      TEST_ASSERT_TRUE_MESSAGE(runs == 1, "channel ghost split into two edges");
      TEST_ASSERT_TRUE_MESSAGE(run_end - run_start + 1 <= 3, "ghost too wide");
      TEST_ASSERT_TRUE_MESSAGE(abs(run_start + run_end - 2 * c) <= 12,
                               "ghost drifted from the feature");
    }
  }
}
```
(Level-1 ghosts can dither to 0 at Bayer t=0 near the rim — that only shortens a run; runs must stay contiguous and non-empty because the level-2 peak survives: (31+0)>>4 = 1 at f=248.)

Register all four after the existing `RUN_TEST(test_crt_ca_should_pull_red_from_the_left);`:

```c
  RUN_TEST(test_crt_ca_zero_point_should_mirror_ghosts_about_the_line);
  RUN_TEST(test_crt_strike_should_stack_ca_boost_in_whole_pixels);
  RUN_TEST(test_crt_strike_max_shift_should_clamp_both_taps_at_the_edge);
  RUN_TEST(test_crt_ca_should_never_split_a_feature_into_two_ghosts);
```

- [ ] **Step 2: Run to verify failure**

Run: `make -C test test 2>&1 | grep -E "FAIL|zero_point|stack|clamp|split" | head -20`
Expected: at least the zero_point and strike_stack tests FAIL (old whole-pixel pass leaves the line untouched / shifts it differently). RED confirmed.

- [ ] **Step 3: Implement the weighted sampler in stage 1**

In `src/c/crt.c`, replace the stage-1 per-pixel block. The old block:

```c
      for (int x = 0; x < w; x++) {
        int s_h = (crt_ca_t3_h2(s_ca_xq[x] + yterm) + 1) / 3 + ca_boost;
        int s_v = crt_ca_shift_v2(s_ca_xq[x] + yterm);

        bool left = x * 2 < w - 1;
        bool top = y * 2 < h - 1;
        // Sampling signs: left half pulls R from the right; top half pulls R
        // from below. Mirrors across both centrelines. Forward reads (below on
        // the top half, above on the bottom half) rely on the ring capture
        // being one row ahead in traversal direction.
        int rx = left ? x + s_h : x - s_h;
        int bx = left ? x - s_h : x + s_h;
        int ry = top ? y + s_v : y - s_v;
        int by = top ? y - s_v : y + s_v;
        if (rx < 0) rx = 0;
        if (rx >= w) rx = w - 1;
        if (bx < 0) bx = 0;
        if (bx >= w) bx = w - 1;
        // The ring holds rows y-2..y+2 at most; s_v stays within 1 today.
        // Both halves only read rows already captured into the ring.
        if (ry < 0) ry = 0;
        if (ry >= h) ry = h - 1;
        if (by < 0) by = 0;
        if (by >= h) by = h - 1;

        row_ca[x] = GCOLOR8_ALPHA | (s_vraw_ring[ry % 3][rx] & 0x30) | (row[x] & 0x0C) |
                    (s_vraw_ring[by % 3][bx] & 0x03);
      }
```

becomes:

```c
      for (int x = 0; x < w; x++) {
        // Horizontal pull in THIRDS: the rung (0/3/6) minus one third to
        // cancel the RGB stripe's built-in element offset (R sits 1/3px
        // left of the pixel centre, B 1/3px right — G at the centre stays
        // untapped), plus the strike boost in whole pixels. The −1 makes
        // the dead zone actually converge: at the centre the red and blue
        // rasters land on the content, not a 2/3px apart.
        int q = crt_ca_t3_h2(s_ca_xq[x] + yterm) - 1 + 3 * ca_boost;
        int j = (q - (q < 0 ? 2 : 0)) / 3;  // floor(q/3); q >= -1 by construction
        int f = q - 3 * j;  // 0..2 (2 for every current rung: q ≡ -1 mod 3)
        int s_v = crt_ca_shift_v2(s_ca_xq[x] + yterm);

        bool left = x * 2 < w - 1;
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
        // Both taps already crowdsourced to the same (valid) ring ROW; clamp
        // the COLUMNS — at max strike (j=4) they land on the same edge
        // column and the weighted sample degenerates to a plain copy.
        if (rt0 < 0) rt0 = 0;
        if (rt0 >= w) rt0 = w - 1;
        if (rt1 < 0) rt1 = 0;
        if (rt1 >= w) rt1 = w - 1;
        if (bt0 < 0) bt0 = 0;
        if (bt0 >= w) bt0 = w - 1;
        if (bt1 < 0) bt1 = 0;
        if (bt1 >= w) bt1 = w - 1;
        if (ry < 0) ry = 0;
        if (ry >= h) ry = h - 1;
        if (by < 0) by = 0;
        if (by >= h) by = h - 1;

        const uint8_t* rrow = s_vraw_ring[ry % 3];
        const uint8_t* brow = s_vraw_ring[by % 3];
        // Fractional channel sample: ((3-f)*near + f*far)/3. A 0↔3 edge
        // yields exactly 2 and 1 — a real centroid shift with no duplicated
        // ghost and no spatial pattern. Truncating divide: code values are
        // linear in light here (the panel makes levels by area fill), so no
        // gamma round trip.
        int r = ((3 - f) * ((rrow[rt0] >> 4) & 3) + f * ((rrow[rt1] >> 4) & 3)) / 3;
        int b = ((3 - f) * (brow[bt0] & 3) + f * (brow[bt1] & 3)) / 3;
        row_ca[x] = GCOLOR8_ALPHA | (uint8_t)(r << 4) | (row[x] & 0x0C) | (uint8_t)b;
      }
```

Also update the `// 1) CA from raw sources:` comment above the loop to note the weighted taps:
```c
      // 1) CA from raw sources: horizontal fringe takes a two-tap weighted
      //    sample per shifted channel (thirds-of-a-px pull); vertical
      //    fringe samples ring rows toward/away from the centreline.
```

- [ ] **Step 4: Run the full suite**

Run: `make -C test test 2>&1 | tail -25`
Expected: PASS, all CRT tests including the four new ones GREEN, and the pre-existing bar/warp/strike-slide/vignette tests unchanged and passing.

If the new tests fail: recompute the expected levels from the q/j/f formulas against the actual coordinates before touching the code — the plan's derivation (comments under each test) is the ground truth for what the fixture should produce; a mismatch means the implementation deviates from the formula, not that the test is stale.

- [ ] **Step 5: Format check**

Run: `make format-check`
Expected: PASS; else `make format` and re-run Step 4.

---

### Task 3: Visual baseline regeneration + full gate

**Files:**
- Modify: `test/visual/baseline.png` (binary, regenerated)
- Modify: `screenshot_current.png` (binary, regenerated — face appearance changed; AGENTS.md requires regen)

**Interfaces:**
- Consumes: working build from Tasks 1-2.
- Produces: a passing `make visual-check`.

- [ ] **Step 1: Full test gate**

Run: `make test`
Expected: PASS (format + JS + C suite).

- [ ] **Step 2: Regenerate the visual baseline**

The weighted CA changes logical pixels everywhere (inside the dead zone, white text's R/B drop from 3 to 2 — the zero-point correction), so the committed baseline must be regenerated or `visual-check` fails forever.

Run: `make visual-baseline`
Expected: new `test/visual/baseline.png` committed to the working copy (jj tracks it automatically — verify with `jj status` that it appears as modified).

Requires the emery emulator; if `pebble install --emulator emery` cannot connect in this environment, STOP and report — do not hand-fake the PNG.

- [ ] **Step 3: Verify the gate passes with the new baseline**

Run: `make visual-check`
Expected: `visual-check (attempt 1): 0 pixels differ outside the masks` (a later attempt's 0 is acceptable per the Makefile's retry semantics; report which attempt passed).

- [ ] **Step 4: Regenerate the README screenshot**

The centre-tint change alters the face's rendered appearance, and AGENTS.md ties `screenshot_current.png` to any appearance change.

Run: `pebble screenshot --emulator emery --no-open screenshot_current.png`
(The face should already be installed from Step 2; if the emulator was restarted, re-run `pebble install --emulator emery` first and wait ~5s for the first real render.)
Expected: file updated; confirm with `jj status`.

- [ ] **Step 5: Report**

State: tests pass count, which visual-check attempt passed, both PNGs updated. Remind the requester that the fringe's actual look is a hardware judgement — the spec says only the watch can answer whether weighted CA reads better than the 0/1/2px zones; the emulator cannot.

---

## Self-Review

**Spec coverage:**
- Stage 1 (weighted fractional horizontal CA, zero-point cancellation, strike boost stacking, clamp degeneracy) → Tasks 1-2 ✓
- Stage 2 vertical CA "no change" → untouched; ring-row read structure preserved verbatim ✓
- Stage 3 curvature "do not use subpixel", no y-dithering → untouched ✓
- Stage 4 aperture grille → explicitly excluded by the 2026-08-22 scope narrowing (fringe first, grille only if the fringe experiment lands) — recorded in the plan header so it is not silently dropped ✓
- Explicitly-not-doing list (scanlines, subpixel text, gamma round trip) → none implemented; sampler weights code values directly per the probe result ✓
- Tests: element ladder ✓ (Task 1), no-duplication ✓ (Task 2), zero point ✓ (Task 2 + rewritten onset test), strike px-vs-thirds pin ✓, clamp-collapse ✓, named-test rewrites in thirds ✓, baseline regen ✓. Grille tests — N/A (grille not built) ✓
- Cost note (5 reads/px, weighted average per shifted channel, hardware timing to watch) → implementation matches; hardware timing check is the requester's A/B, surfaced in Task 3 Step 5 ✓

**Placeholder scan:** all test code and the full sampler block are concrete; expected run outputs stated. ✓

**Type consistency:** `crt_ca_t3_h2(int)→int` used identically in Tasks 1-2; `crt_ca_shift3(x,y,w,h)→int` used identically in both rewritten tests and the ladder test; `crt_strike_offset(113, 0)` matches its existing signature. No references to undefined functions. `crt_ca_shift_v2` keeps its name/signature (1px cap, unchanged). ✓

**One deliberate sequencing note:** Task 1 Step 3 installs a temporary `(t3+1)/3` whole-pixel projection so the tree compiles and the suite stays green between tasks; Task 2 Step 3 replaces it. Do not ship the projection alone.
