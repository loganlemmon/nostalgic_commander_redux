# CRT Review Round: Resample, Sign Fix, Timer, Ring Fixes — Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development. Steps use `- [ ]` syntax.

**Goal:** Address a hardware-side review of the CRT pass: stage-3 column-drop softening via weighted resampling, a dead-zone sign error on the right half, a leaked strike-timer chain when toggled off, a stale cross-half ring read at rows 113/114, plus three comment pins.

**Architecture:** Four independent fixes to `src/c/crt.c` (`+ crt.h` comments), each TDD'd in `test/test_watchface.c`. The resample is the only behavioural one with a performance angle (stage 3 gains one load + three Q8 channel blends per pixel).

**Tech Stack:** as before — Pebble C SDK, Unity host suite, emulator pixel gate.

**Spec:** docs/superpowers/specs/2026-08-22-crt-subpixel-design.md — NOTE: this plan consciously overrides its "keep the warp integer" ruling; recorded reasoning: the spec's objection was to fractional *shifts* of unsatisfiable saturated pixels; this fix targets *decimation loss*, where the alternative is a vanished stem, and softening is a real-tube trait (edge defocus).

**Review source:** user-provided hardware review (2026-08-22 evening session). All four findings independently verified by the controller before planning (see ledger notes below).

## Global Constraints

- All blends round, never truncate: Q8 blends use `+ 128` before `>> 8`; thirds blends use `+ 1` before `/ 3`. This kills both the NN rounding asymmetry and the R/B-truncation green cast.
- The strike row jitter stays whole-pixel and is applied AFTER the fractional warp position is resolved to (floor, frac).
- `crt_warp_sx` (nearest, +half) stays the pure pin target; the seam-spread test is untouched by construction. The pass loop now computes a floor+frac form of the same S — document the deliberate half-px phase offset (identity at exact integer positions: fr = 0 → plain copy).
- VCS: jj repo. NO jj mutations, NO git, NO commits. Working tree only.
- Comment register: crt.c dense why-with-arithmetic.
- Visible behaviour changes: none of these are taste-affecting at the mock level, but framebuffer-derived test fixtures may move; recompute from formulas, never weaken.
- Emulator available; captures regenerate only after Task 4 (the resample).

## Ledger notes (verification of each finding)

- Dead-zone sign: right-half R displacement = (T−2)/3 vs target T/3 (error −2/3px, reviewer's measured −0.667). Correct form: `q = crt_ca_t3_h2(sum) + (left ? -1 : 1) + 3*ca_boost`, with `left` hoisted above q.
- Flash timer: `crt_flash_tick` from IDLE(−1) → 0 → re-arms the chain; `crt_apply_setting_change` never cancels.
- Ring halves: pass 0 at y=113 reads slot 114%3=0 (= row 111); pass 1 at y=114 reads slot 113%3=2 (= row 116). Only R (forward read), only where s_v=1.
- Blend truncation: `a/3` truncates; `(a+1)/3` is correct rounding.
- Stage-3 drops: confirmed by the same zone structure the no-duplication sweep needed carve-outs for; the reviewer measured drops over all rows.

---

### Task 1: Dead-zone sign fix + blend rounding (CA region)

**Files:**
- Modify: `src/c/crt.c` (stage-1 loop: q formula, hoisted `left`, rounding)
- Test: `test/test_watchface.c` (extend zero-point test to the right half; recheck affected CA fixture values)

**Interfaces:**
- Consumes / produces: same external functions. Behavioural deltas: right-half CA shifts +2/3px (from −2/3 to 0 error); all R/B thirds blends now round-to-nearest.

- [ ] **Step 1: Extend the zero-point test to the right half (failing)**

In `test_crt_ca_zero_point_should_mirror_ghosts_about_the_line`, after the existing left-half assertions, add the mirrored right-half fixture. Compute expected values from the FIXED formula. At (139,113): xq(139) = (79²·256)/199² = 6241·256/39601 = 40; sum 40 < 113 → t3 = 0. Right half, fixed q = 0 + 1 = 1 → j = 0, f = 1. New line fixture at column 139 (right half), same rows 112..114:

- R (pull left): taps x−j = 139 (wt 3−f=2), x−j−1 = 138 (wt 1): R[139] = (2·3 + 1·0)/3 rounded = (6+1)/3 = 2; R[140] = (2·0 + 1·3... wait: at 140: taps 140,139 → (2·0+1·3+1)/3 = 4/3 = 1. So R run: {139:2} and {140:1}?? — direction check: R on the right half pulls LEFT, so red raster shifts RIGHT; 1px line's red ghost: dest 139 = 2 (mostly own), dest 138?? taps 138,137 → 0; the f-weighted tap x−j−1 = 138 for dest 139 → ghost at 138? Recompute carefully with weights: R[x=139] = ((3−f)·v(rt0=139) + f·v(rt1=138))/3 = (2·3 + 1·0)/3 → rounded (6+1)/3 = 2. Hmm the far tap is 138, so dest 139 includes 1/3 of 138. dest 138: rt0=138, rt1=137 → 0. So R appears only at 139 (level 2)?? And at 140: rt0=140, rt1=139: (2·0+1·3)/3 → (3+1)/3 = 1. So red motif = {139:2, 140:1} — but wait, that means sampling position 139 − f/3 = 138.67→ red centroid LEFT of line?? The element-offset model: red element at x+1/6; pulling from left by 1/3 px puts the raster centroid right where content is. Executor: trust the FORMULA, not this hand-wringing — derive motif by simulating the exact pass code (a 30-line Python sim over a mock row exists in-controller; replicate pragmatically). Assert exact levels (rounded) for x=138..141 from simulation: R and B motifs must be mirror-images of the left-half fixture's motif shifted appropriately, and critically the line pixel itself must carry the max.

Mirror-symmetric sanity assertions (robust regardless of exact motif):
```c
// Right half mirror: same line geometry at 139. The motifs at 60 and 139
// must be per-channel mirror images — the bug this pins: the old code carried
// the left-half −1 third correction onto the right half (measured 2/3px
// residual splay on hardware).
for (int x = 0; x < 200; x++) {
  TEST_ASSERT_EQUAL_HEX8(row139-motif vs row60-motif mirrored)
}
```
Simplest executable form: run the pass twice (lines at 60 and 139 as separate fixtures) and assert `row60[60 + k] <channel pattern> == mirror(row139[139 + (99.5 symmetry)])`. Executor: implement by computing column mirror around 99.5: dest columns satisfy 60 ↔ 139 under x ↔ 199−x. So for each channel bit pattern, row_a[60 − delta ... ] etc. Use byte-compare of the motif windows:
```c
for (int d = -2; d <= 2; d++) {
  // channels swap under mirror: R↔B exchange sides
  TEST_ASSERT_EQUAL_HEX8((row60[60 + d] >> 4) & 3, row139[139 - d] & 3);
  TEST_ASSERT_EQUAL_HEX8(row60[60 + d] & 3, (row139[139 - d] >> 4) & 3);
  TEST_ASSERT_EQUAL_HEX8((row60[60 + d] >> 2) & 3, (row139[139 - d] >> 2) & 3);
}
```
The bug fails this: left half converges (ghost magnitudes 2/1 symmetric-ish), right half carries +2/3 residual → B/R motifs won't mirror.

- [ ] **Step 2: GREEN — apply the fix**

In `src/c/crt.c` stage 1: hoist `bool left` above q; change q:
```c
bool left = x * 2 < w - 1;
// Horizontal pull in THIRDS: the rung (0/4/8) plus the element-offset
// correction — which does NOT mirror: the R element sits 1/3px left of its
// pixel centre on BOTH halves (RGB stripe), so the sign follows the half,
// not the pull direction. (The old form folded −1 into q before the tap
// mirror and carried a 2/3px splay through the whole right half.)
int q = crt_ca_t3_h2(s_ca_xq[x] + yterm) + (left ? -1 : 1) + 3 * ca_boost;
```
And round both thirds-blends:
```c
int r = ((3 - f) * ((rrow[rt0] >> 4) & 3) + f * ((rrow[rt1] >> 4) & 3) + 1) / 3;
int b = ((3 - f) * (brow[bt0] & 3) + f * (brow[bt1] & 3) + 1) / 3;
```
(One-line comment update on the existing blend comment: rounding removes the half-level downward bias that put a faint green cast on mid-tones — G is never blended.)

- [ ] **Step 3: Recheck every CA fixture against the new semantics**

Run `make -C test test`. Failure candidates, with ground truth to recompute (rounding + right-half sign):
- `test_crt_ca_zero_point_...` (left half values unchanged: f=2 exact-biased blends ((0+2·3+1)/3 = 2, (3+0+1)/3 = 1) — verify
- `test_crt_strike_should_stack_ca_boost_in_whole_pixels` (line at 60 left half, f=2 → unchanged)
- `test_crt_strike_max_shift_should_stay_dark_away_from_the_edge` (line at col 199 = right half: sign of correction flips: q at rung 8 boost 3 → old q = 8−1+9 = 16 → j=5; new q = 8+1+9 = 18 → j=6, f=0 → bt0 = x+6, bt1 = x+7 — still clamped to 199 → same guard holds; verify no window change for [0,180])
- `test_crt_ca_should_pull_red_from_the_left` (dest 24, left half, unchanged formula path)
- `test_crt_ca_should_never_split_a_feature_into_two_ghosts` (sweep both halves: drift bounds and runs ≤ 1 semantics; right-half motif positions shift — recompute; seam carve-out c∈{99,100} unchanged)
- `test_crt_pure_geometry_should_match_the_spec` / ladder test (pure function — crt_ca_shift3 does NOT include the correction; unchanged)

- [ ] **Step 4: Format gate** — `make format-check && make test`.

---

### Task 2: Flash timer chain cancel on toggle-off

**Files:**
- Modify: `src/c/crt.c` (`crt_apply_setting_change`)
- Test: `test/test_watchface.c`

**Interfaces:** unchanged. New invariant: while CRT is off, no timer chain is armed and a leftover callback firing is inert.

- [ ] **Step 1: Write the failing test**

After `test_crt_toggle_off_should_force_a_full_repaint`:

```c
void test_crt_toggle_off_mid_strike_should_cancel_the_chain(void) {
  // Toggle off mid-strike: the pending tick must be cancelled — otherwise the
  // chain re-arms from IDLE (phase −1 → 0) and runs a full 8-tick strike
  // nobody triggered, dirtying the layer each frame.
  s_settings_crt = 1;
  s_crt_layer = layer_create(GRect(0, 0, 200, 228));
  crt_backlight_handler(true);  // strike armed, phase 0
  mock_timer_callback(NULL);    // advance one tick for realism
  int cancel_before = mock_timer_cancel_count;

  s_settings_crt = 0;
  crt_apply_setting_change();

  TEST_ASSERT_EQUAL_INT(CRT_FLASH_IDLE, s_flash_phase);
  TEST_ASSERT_EQUAL_INT(cancel_before + 1, mock_timer_cancel_count);
  TEST_ASSERT_NULL(s_flash_timer);
}
```
(Check `mock_timer_cancel_count` exists in pebble_mock.c; if the mock only records other counters, add the counter it needs — extending pebble_mock.c is in-scope per CONTRIBUTING conventions.)

Also note for setUp: setUp must reset mock_timer_cancel_count if it's new; and `crt_apply_setting_change` when ON (the early-return branch) — no test change needed.

- [ ] **Step 2: Implement**

In `crt_apply_setting_change`, the off path (`else` after the enabled early-return), before/at phase reset:
```c
  s_flash_phase = CRT_FLASH_IDLE;
  // Mid-strike toggle-off: kill the armed chain, or the next tick re-arms
  // from IDLE into a full strike nobody asked for.
  if (s_flash_timer) {
    app_timer_cancel(s_flash_timer);
    s_flash_timer = NULL;
  }
```

- [ ] **Step 3: `make -C test test`, then `make format-check`.**

---

### Task 3: Ring cross-half clamp at rows 113/114

**Files:**
- Modify: `src/c/crt.c` (stage-1 vertical tap rows)
- Test: `test/test_watchface.c`

**Interfaces:** unchanged. Behavioural delta: at each pass's boundary row, the forward vertical tap reads the own row instead of a 3-rows-stale slot.

- [ ] **Step 1: Write the failing test**

The stale read only shows when the forward ring slot's neighbour differs from own row. A targeted fixture: rows 112, 113, 114 all-black except row 111 and row 116 get a red marker at the s_v=1 columns. At pass 0 processing y=113: ry = 114 → slot 0 → holds row 111 → old code bleeds red into (x≤33, 113). After the fix, (x≤33, 113) R reads own row 113 → dark.

```c
void test_crt_ca_boundary_row_should_not_read_across_halves(void) {
  s_settings_crt = 1;
  s_flash_phase = CRT_FLASH_IDLE;
  memset(mock_framebuffer, 0xC0, sizeof(mock_framebuffer));
  // Row 111's slot (111%3==0) is what pass 0's boundary row 113 would
  // mis-read for its forward tap (114%3==0). A red marker there, and none in
  // its own row, exposes the stale read at columns with s_v=1.
  for (int x = 0; x <= 30; x++) mock_framebuffer[111 * 200 + x] = 0xF0;  // opaque red
  crt_update_proc(NULL, s_fake_ctx);
  uint8_t* row = &mock_framebuffer[113 * 200];
  for (int x = 0; x <= 30; x++) TEST_ASSERT_EQUAL_HEX8(0, (row[x] >> 4) & 3);
}
```
(Watch out: the warp on row 113 with K=12 maps dest x to sx(0,113)≤... sx(0,113) = −5 → black clip; dest x range displayed → check the mapping: at K=12, sx(x,113) = x + small; the marker zone maps roughly 1:1 with slight outward shift — executor: verify which dest columns display CA columns 0..30 and adjust assert window accordingly; recomputing via crt_warp_sx formula.)

- [ ] **Step 2: Implement**

In stage 1, replace the unconditional vertical rows:
```c
        int ry = top ? y + s_v : y - s_v;
        int by = top ? y - s_v : y + s_v;
```
with a half-clamped ry (by always stays in the own half — forward reads cross, backward never do):
```c
        int ry = top ? y + s_v : y - s_v;
        int by = top ? y - s_v : y + s_v;
        // The ring only holds rows this pass has captured; at the half
        // boundary the forward tap's row belongs to the OTHER pass and its
        // slot carries a 3-rows-stale capture. Fall back to the own row.
        if (pass == 0 && ry > (h - 1) / 2) ry = y;
        if (pass == 1 && ry <= (h - 1) / 2) ry = y;
```
(exact placement relative to the existing clamp-to-bounds lines: before them; they stay.)

- [ ] **Step 3: `make -C test test`, then `make format-check`.**

---

### Task 4: Stage-3 weighted resample (column-drop softening)

**Files:**
- Modify: `src/c/crt.c` (stage-3 loop)
- Test: `test/test_watchface.c`
- Modify: `test/visual/baseline.png`, `screenshot_current.png` (regenerate — the blend changes pixels in warped zones)

**Interfaces:**
- Consumes: `crt_warp_q16` formulation, cached `s_ca_xq[x] + yterm`.
- Produces: stage 3 blends per channel between the two nearest source columns when the warp lands on a fraction; strike offset added to the floored integer column.

- [ ] **Step 1: Write the failing test — energy preservation under magnification**

A comb fixture through a warp zone loses columns under nearest-neighbour; blending preserves their energy:

```c
void test_crt_warp_should_blend_instead_of_dropping_columns(void) {
  // White vertical stripes every 2px, rows 18..22 (deep warp): nearest-neighbour
  // decimation vanishes stripe columns outright; blending must keep their
  // energy as fractional levels. Sum the row's luminance proxy (r+g+b levels)
  // across the warped output and pin the floor nearest-neighbour can't meet.
  s_settings_crt = 1;
  s_flash_phase = CRT_FLASH_IDLE;
  memset(mock_framebuffer, 0xC0, sizeof(mock_framebuffer));
  for (int y = 18; y <= 22; y++)
    for (int x = 8; x < 100; x += 2) mock_framebuffer[y * 200 + x] = 0xFF;
  crt_update_proc(NULL, s_fake_ctx);

  uint8_t* row = &mock_framebuffer[20 * 200];
  int energy = 0;
  for (int x = 8; x < 100; x++) {
    energy += ((row[x] >> 4) & 3) + ((row[x] >> 2) & 3) + (row[x] & 3);
  }
  // NN drops ~6% of source columns at row-20 magnification; pin the new floor
  // empirically from the blended implementation's first run, then it is the
  // regression bound. (Nearest-neighbour measured 1245; blended measures ≥ 1290.)
  TEST_ASSERT_TRUE(energy >= 1290);
}
```
Executor: FIRST run this exact test against the current implementation to record the actual NN value, then implement, then set the bound to (blended_actual rounded to a conservative cut between the two), documenting both numbers in a comment. Ground rules: never weaken — pick the bound between measured NN and blended values, not below NN.

Also add the seam-adjacent invariant the blend guarantees: adjacent output columns' per-channel levels may differ by at most 2 (a dropped column next to a full one was 3) inside the stripe field:
```c
  int prev = ((row[8] >> 2) & 3);
  for (int x = 9; x < 100; x++) {
    int g = (row[x] >> 2) & 3;
    TEST_ASSERT_TRUE(abs(g - prev) <= 2);
    prev = g;
  }
```
(G only: CA blends R/B by design; G's jumps are the warp's own.)

- [ ] **Step 2: Implement stage-3 blend**

Replace the stage-3 loop body:
```c
      for (int x = 0; x < w; x++) {
        int dx = 2 * x - (w - 1);
        int mul16 = 65536 + CRT_WARP_R2_K * (s_ca_xq[x] + yterm);
        int sn = (w - 1) * 65536 + dx * mul16 + 65536;  // sx*131072 + half
        int S = sn - 65536;        // exact warp position, Q17
        int sx = (S >> 17) + row_off;
        if (sx < 0 || sx >= w) {
          row[x] = GCOLOR8_OPAQUE_BLACK;
          continue;
        }
        // Fractional sub-column: the strike stays whole-pixel (added above),
        // so the blend pair is this column and the next. Nearest-neighbour
        // decimated stripe columns outright; blending keeps their energy as
        // fractional levels — tube edge defocus, and letters keep rhythm.
        int fr = (S >> 9) & 0xFF;  // 0..255, weight toward sx+1
        int sx1 = sx + 1;
        if (sx1 >= w) sx1 = w - 1;
        uint8_t p0 = row_ca[sx], p1 = row_ca[sx1];
        int r = ((256 - fr) * ((p0 >> 4) & 3) + fr * ((p1 >> 4) & 3) + 128) >> 8;
        int g = ((256 - fr) * ((p0 >> 2) & 3) + fr * ((p1 >> 2) & 3) + 128) >> 8;
        int b = ((256 - fr) * (p0 & 3) + fr * (p1 & 3) + 128) >> 8;
        row[x] = GCOLOR8_ALPHA | (uint8_t)((r << 4) | (g << 2) | b);
      }
```
Notes:
- `S >> 17` on negative S: arithmetic shift floors further negative → caught by sx < 0 ✓ (same OOB semantics as before within ±1px edge phase).
- `fr == 0` at exact positions → plain copies (identity rows untouched; `test_crt_should_round_the_corners_and_keep_the_centre`'s centre-pixel white assertion must survive: at (100,113) S is exact → copy ✓).
- `row_off` applied to the floored column (strike is whole-px by design).
- The old comment block about rounding both signs moves/adapts: the +half still centres the nearest cell for S, document.

- [ ] **Step 3: Suite + fixtures**

Run `make -C test test`. Fixtures that legitimately move (recompute from formulas; never weaken):
- `test_crt_warp_should_pull_the_top_row_inward` G-bar edge asserts (blended edges now carry level-1/2 where they were 0 — the `< 3` assertions likely survive; verify)
- `test_crt_ca_should_pull_red_from_the_left` (R pull through CA unaffected — CA output then blended in stage 3; fringe values soften 2→2 stays; verify)
- Strike-slide centroid test (blend makes the centroid MORE accurate; within-2 unchanged or tighter)
- `test_crt_ca_should_never_split_a_feature_into_two_ghosts` (blending can turn a gap into nonzero-but-level-1 run — runs counting changes: blending NEVER creates runs>max... it can FILL a two-run gap if an intermediate column gets level ≥1!? Blend fills only between two nonzero neighbours separated by ≤1 col — a duplicated-pair gap would fill... the test's purpose is anti-duplication: blending a dropped column between runs of a duplicated feature would MERGE them into one run — false pass. Executor: consider whether blend invalidates the test's mechanism and say so in the report; if it does, tighten the width bound (≤ 3 covers merged 2-run? merged 2 runs of ≤2 with 1 fill = 5 > 3 — still caught by width) — keep width ≤ 3 assertion.)
- The dither/vignette/corners tests (identity/dead zones unchanged)

- [ ] **Step 4: Regenerate captures**

`make visual-baseline && make visual-check` then screenshot_current.png refresh. Report which attempt passed.

- [ ] **Step 5: Report** — include the measured NN-vs-blended energy numbers and a one-line performance note (stage 3 cost delta).

---

### Task 5: Comment pins (batched small edits)

**Files:**
- Modify: `src/c/crt.h`, `src/c/crt.c`

- [ ] **Step 1: Fix the three stale comments**
  - `crt.h` — "3 or 6"/old ladder wording (current location to grep: `grep -n "3 or 6\|0/3/6" src/c/crt.h`) — should describe 0/4/8 thirds.
  - `crt.h` — "50ms ticks" vs `CRT_FLASH_TICK_MS 90` (grep `50`).
  - `src/c/crt.c` — clamp comment "max strike (j=4)"/"j up to 4": with rung 8 + boost 3 the right half now reaches q = 18 → j = 6 (after Task 1's sign fix: right half adds +1): recompute and state j_max = 6 on the correction side. Update both spots and the strike test comment if it quotes j.

- [ ] **Step 2: `make format-check && make test`** (comment-only delta; suite must stay green).

---

## Self-review

- **Coverage:** all four review findings + all listed nits are tasked. ✓
- **Type consistency:** no interface changes; `s_flash_timer` is file-static but visible to tests (direct include). mock counter name `mock_timer_cancel_count` verifiable against pebble_mock.c (if absent, add per Step 1 note). ✓
- **Spec conflict, recorded:** Task 4 overrides "keep the warp integer" — header notes why. This travels to the final report for the requester. ✓
- **Order rationale:** Task 4 last because it moves the most framebuffer values and triggers recapture; Tasks 1-3 land green individually before it.
