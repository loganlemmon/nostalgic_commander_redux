# CRT Radial Curvature (Warp Seam Fix) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the CRT pass's rank-1 warp (a y-only horizontal gain whose quantization steps all outer columns on the same 8 row boundaries — visible 1px horizontal seams) with a radial magnification term, so step boundaries curve in x like a curved surface instead of seaming.

**Architecture:** The destination→source horizontal gain stops being per-row (`mul16` from `crt_warp_inset(y)`) and becomes per-pixel: `M(x,y) = 65536 + CRT_WARP_R2_K * (xq+yterm)`, reusing the same elliptical squared-radius terms the CA stage already computes (`s_ca_xq[x]` cache + row `yterm`). K=13 ≡ ~5px pull at mid-edges (today's `CRT_WARP_MAX_PX` behaviour at top/bottom), growing smoothly to the corners. Everything still samples a single row (`crt_warp_sx` = horizontal-only resampling); strike row jitter unchanged.

**Tech Stack:** same as before — Pebble C SDK code, Unity host suite in `test/test_watchface.c` (includes `../src/c/crt.c` directly), `make test` = format + JS + C, `make visual-baseline`/`visual-check` = emery emulator pixel gate.

**Spec:** docs/superpowers/specs/2026-08-22-crt-subpixel-design.md, "Stage by stage" §3 final paragraph (radial term named as the fix; y-dither explicitly rejected there — do not reintroduce dithering).

## Global Constraints

- **Deliberate new appearance:** side content now warps too (curved tube bows on all edges); vertical window frames bow inward ~3.5-4px around mid-height (dest maps to source further from centre, so content moves toward the middle), and the outermost ~4 columns clip to black at mid-height — those columns are already vignette-black, so nothing visible is lost. This is intended, surfacing for the hardware A/B.
- `crt_warp_inset` is REMOVED (definition src/c/crt.c:29, call src/c/crt.c:217, decl src/c/crt.h:57, test uses at test/test_watchface.c:3922/3930-3932). Verify no other callers remain (grep).
- `CRT_WARP_MAX_PX` is removed with it; its value is folded into the K comment as the calibration anchor.
- Stage 1 (CA thirds sampler), stage 2 (vignette/dither), strike geometry/sound: untouched.
- `crt_strike_offset` row jitter rides on top of `crt_warp_sx` by addition, exactly as today (`sx + row_off`).
- No dithering anywhere in the warp; quantization stays integer.
- VCS: jj repo. Implementers and reviewers run NO jj mutations and NO git. Work stays in the working tree (change `wkvotwyr`); per-task test gates stand in for commits.
- Comment register: crt.c's dense why-with-arithmetic style. Integer math only.
- All tests must keep passing except the three explicitly rewritten here; rewritten fixtures must be recomputed against the new formula before changing assertions.

Useful exact values (Q8 elliptical radius, w=200, h=228):
`xq(x) = (2x-199)²·256/199²`, `yterm(y) = (2y-227)²·256/227²`.
- centre pixel (100,113): xq=0, yterm=0 → M=65536
- top centre (100,20): yterm=173 → M=67785
- mid-edge sides (0,113)/(199,113): xq=256 → M=68864
- corner (0,0)/(199,227): xq+yterm=512 → M=72192
- crt_warp_sx(x,y) = `((w-1)·65536 + (2x-(w-1))·M + 65536) >> 17` (no strike)
- sx(0,113) = −5 (clips black), sx(100,113) = 100 (identity)

---

### Task 1: Radial warp geometry (pure functions + pure-test rewrites)

**Files:**
- Modify: `src/c/crt.h` (replace `crt_warp_inset` decl/comment; add two decls; replace `CRT_WARP_MAX_PX`)
- Modify: `src/c/crt.c` (replace `crt_warp_inset` def with `crt_warp_q16` + `crt_warp_sx`; stage-3 call site compiles via Task 2 — see Interfaces)
- Test: `test/test_watchface.c` (rewrite the two pinned warp test blocks, add the seam-spread test)

**Interfaces:**
- Consumes: nothing new; zone macros unchanged.
- Produces (Task 2 relies on these exact signatures):
  - `int crt_warp_q16(int x, int y, int w, int h)` — radial magnification M in Q16: `65536 + CRT_WARP_R2_K * (xq + yq)`. Costs two small int divisions per call; the pass in Task 2 does NOT call it per pixel (reuses `s_ca_xq[x] + yterm`); the exported pure function is for tests and documentation.
  - `int crt_warp_sx(int x, int y, int w, int h)` — full source column mapping without strike jitter: `((w-1)*65536 + (2*x-(w-1)) * crt_warp_q16(x, y, w, h) + 65536) >> 17`. Task 2's stage-3 loop inlines the same expression (reading M from the cached terms) and adds `row_off`; this exported function is the pin target for the seam-spread test.
  - `#define CRT_WARP_R2_K 13` in src/c/crt.h.

- [x] **Step 1: Rewrite the failing tests**

In `test/test_watchface.c`, in `test_crt_pure_geometry_should_match_the_spec`, replace the warp block (currently three `crt_warp_inset` asserts at ~3930-3932):

```c
  // Warp: identity at the screen centre, calibrated ≈5px pull at mid-edges,
  // growing smoothly toward the corners (radial — was rank-1 in y).
  TEST_ASSERT_EQUAL_INT(65536, crt_warp_q16(100, 113, 200, 228));
  TEST_ASSERT_EQUAL_INT(67785, crt_warp_q16(100, 20, 200, 228));
  TEST_ASSERT_EQUAL_INT(68864, crt_warp_q16(0, 113, 200, 228));
  TEST_ASSERT_EQUAL_INT(68864, crt_warp_q16(199, 113, 200, 228));
  TEST_ASSERT_EQUAL_INT(72192, crt_warp_q16(0, 0, 200, 228));
  TEST_ASSERT_EQUAL_INT(72192, crt_warp_q16(199, 227, 200, 228));
  // Monotone with elliptical radius along both axes; mirror-symmetric.
  TEST_ASSERT_TRUE(crt_warp_q16(190, 113, 200, 228) >= crt_warp_q16(160, 113, 200, 228));
  TEST_ASSERT_TRUE(crt_warp_q16(100, 190, 200, 228) >= crt_warp_q16(100, 160, 200, 228));
  for (int y = 0; y < 228; y++) {
    for (int x = 0; x < 200; x++) {
      int m = crt_warp_q16(x, y, 200, 228);
      TEST_ASSERT_EQUAL_INT(m, crt_warp_q16(199 - x, y, 200, 228));
      TEST_ASSERT_EQUAL_INT(m, crt_warp_q16(x, 227 - y, 200, 228));
      TEST_ASSERT_TRUE(m >= 65536 && m <= 72192);
    }
  }
```

Replace `test_crt_warp_should_pull_the_top_row_inward`'s single `crt_warp_inset` assert (~line 3922) — the G-bar assertions after it stay UNCHANGED (recomputed below):

```c
  // Row 20 mid-column magnification ≈ 3.4% (was a 3px inset).
  TEST_ASSERT_EQUAL_INT(67785, crt_warp_q16(100, 20, 200, 228));
```

Add the seam-spread regression test after `test_crt_pure_geometry_should_match_the_spec`:

```c
void test_crt_warp_should_spread_steps_across_rows(void) {
  // The rank-1 gain quantized the whole outer quarter on the same 8 row
  // boundaries — a visible horizontal seam. With M growing in x, each
  // column's source map crosses its own integer boundaries at its own rows:
  // no boundary may step >16 dest columns at once (spread ≈1 per ~28 rows),
  // and no >3 adjacent columns may step on the same boundary (no comb).
  for (int y = 0; y < 227; y++) {
    int jumps = 0;
    int run = 0, maxrun = 0;
    for (int x = 0; x < 200; x++) {
      if (crt_warp_sx(x, y + 1, 200, 228) != crt_warp_sx(x, y, 200, 228)) {
        jumps++;
        if (++run > maxrun) maxrun = run;
      } else {
        run = 0;
      }
    }
    TEST_ASSERT_TRUE(jumps <= 16);
    TEST_ASSERT_TRUE(maxrun <= 3);
  }
  // Sanity: the pull is still substantial — an identity map would
  // trivially pass the jump-run bounds. Mid-edge sides now push past the
  // edge: sx(195,113): dx=191, xq=235, M=68591 → 99.5+95.5·68591/65536
  // ≈ 199.4 → 199; sx(4,113) mirrors below 0.
  TEST_ASSERT_TRUE(crt_warp_sx(195, 113, 200, 228) >= 199);
  TEST_ASSERT_TRUE(crt_warp_sx(4, 113, 200, 228) <= 0);
}
```

Register in `main()` right after `RUN_TEST(test_crt_pure_geometry_should_match_the_spec);`:

```c
  RUN_TEST(test_crt_warp_should_spread_steps_across_rows);
```

- [x] **Step 2: Run to verify failure**

Run: `make -C test test 2>&1 | tail -15`
Expected: compile error — `crt_warp_q16`/`crt_warp_sx` undeclared, and `crt_warp_inset` still exists until Step 3 (step 1 must NOT delete it yet or other tests break compile — wait, the old asserts referencing crt_warp_inset were already replaced in Step 1, so it compiles only after Step 3; treat the whole RED as the compile failure).

- [x] **Step 3: Implement the pure functions; update header**

In `src/c/crt.c`, replace the `crt_warp_inset` definition (lines 29-32):

```c
// Radial magnification of the curvature stage, Q16. Identity at the centre,
// K per unit of the elliptical squared radius (same xq/yq formulation as the
// CA zone map). K=13 restores the old rank-1 max: ≈CRT_WARP_MAX_PX(5px) pull
// at a mid-edge (r² = 256 Q8) and grows smoothly to 1.10x in the corners —
// a curved surface bows its sides too, so mid-height now warps where the old
// y-only gain was identity. Rank-1 quantized all outer columns on the same
// row boundaries (visible seams); the per-column boundary curve does not.
int crt_warp_q16(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  int dy = 2 * y - (h - 1);
  int xq = (dx * dx * 256) / ((w - 1) * (w - 1));
  int yq = (dy * dy * 256) / ((h - 1) * (h - 1));
  return 65536 + CRT_WARP_R2_K * (xq + yq);
}

// Source column for dest (x,y) under the warp, without strike jitter: the
// pass inlines this with cached xq/yterm and adds the strike offset itself.
int crt_warp_sx(int x, int y, int w, int h) {
  int dx = 2 * x - (w - 1);
  return ((w - 1) * 65536 + dx * crt_warp_q16(x, y, w, h) + 65536) >> 17;
}
```

In `src/c/crt.h`, replace the geometry comment + `CRT_WARP_MAX_PX` define and the `crt_warp_inset` declaration:

```c
// Curvature geometry. The warp magnifies radially — dest (x,y) samples source
// at cx + (x-cx)·M/65536 with M = 65536 + CRT_WARP_R2_K·r² (r² = the
// elliptical Q8 xq+yq the CA zones also use). K=13 ≈ the retired
// CRT_WARP_MAX_PX: 5px of pull at a mid-edge (r²=256), smoothly more toward
// the corners. The vignette darkens within VIGNETTE_PX of the nearest edge
// (counted around the corner arcs) — which is also what hides the warp's
// outermost black-clip columns.
#define CRT_VIGNETTE_PX 20
#define CRT_CORNER_RADIUS 14
#define CRT_WARP_R2_K 13
```
(keep the rest of that comment block about CA zones as-is; just the warp sentence + define swap)

Declaration swap:

```c
// Radial warp magnification in Q16 at (x,y): 65536 = identity, growing with
// the elliptical squared radius. Side content warps too (deliberate — a
// curved tube bows on all edges); the outermost ~4 columns clip to black at
// mid-height, exactly where the vignette is already black.
int crt_warp_q16(int x, int y, int w, int h);
// Source column for dest (x,y) under the warp alone (no strike jitter).
int crt_warp_sx(int x, int y, int w, int h);
```

Stage 3 still references `crt_warp_inset` — do NOT fix it here; leave a deliberate compile break IF AND ONLY IF the header decl removal already breaks it (it will). Accept RED at this step.

- [x] **Step 4: Run**

Run: `make -C test test 2>&1 | tail -15`
Expected: FAIL to compile at the stage-3 call site (`crt_warp_inset` undeclared). This broken tree is handed to Task 2 — Task 1 and Task 2 run back-to-back; do not hand a half state to anyone else.

- [x] **Step 5: No format gate yet** (deferred to Task 2's tail — tree doesn't compile).

---

### Task 2: Stage-3 integration + framebuffer fixtures + captures

**Files:**
- Modify: `src/c/crt.c` stage-3 block (~lines 214-231)
- Test: `test/test_watchface.c` (recompute `test_crt_warp_should_pull_the_top_row_inward` fixture if needed)
- Modify: `test/visual/baseline.png`, `screenshot_current.png` (regenerated)

**Interfaces:**
- Consumes: `crt_warp_q16`/`crt_warp_sx`/`CRT_WARP_R2_K` from Task 1 (signatures above); the existing per-row `yterm` and the `s_ca_xq[]` cache already computed for stage 1.
- Produces: working warp; green suite; green visual-check.

- [x] **Step 1: Replace the stage-3 block**

In `src/c/crt.c`, the current block (~214-231):

```c
      int inset = crt_warp_inset(y, h);
      int mul16 = (w << 16) / (w - 2 * inset);  // Q16 jacobian; ==65536 mid-rows
```
and the stage-3 loop:

```c
      for (int x = 0; x < w; x++) {
        int dx = 2 * x - (w - 1);
        int sn = (w - 1) * 65536 + dx * mul16 + 65536;  // sx*131072 + half
        int sx = (sn >> 17) + row_off;
        row[x] = (sx < 0 || sx >= w) ? GCOLOR8_OPAQUE_BLACK : row_ca[sx];
      }
```

become (drop BOTH lines; the `int ey`...block between them stays exactly as is):

```c
      // 3) Curvature (+ strike jitter): dest (x,y) samples source column
      //    sx = cx + (x - cx)·M(x,y)/65536 with the radial M of crt_warp_q16 —
      //    recomposed here from the CA stage's cached terms — then the strike
      //    slides the whole row sideways. M grows with x too, so integer
      //    quantization boundaries curve instead of aligning into seams.
      //    Rounded to nearest on BOTH signs — a >> on negative deltas floors
      //    away from zero and clipped the left rim a quantum earlier than the
      //    right (visible on hardware as a left-edge shift under each header).
      for (int x = 0; x < w; x++) {
        int dx = 2 * x - (w - 1);
        int mul16 = 65536 + CRT_WARP_R2_K * (s_ca_xq[x] + yterm);
        int sn = (w - 1) * 65536 + dx * mul16 + 65536;  // sx*131072 + half
        int sx = (sn >> 17) + row_off;
        row[x] = (sx < 0 || sx >= w) ? GCOLOR8_OPAQUE_BLACK : row_ca[sx];
      }
```
(Also delete the stale `// 3) Curvature (+ strike jitter): source column sx = ...` comment that preceded the old loop — replaced by the block above. The intervening vignette code between the old inset/mul16 lines and the loop is untouched; note the mul16/inset lines were ABOVE that block — remove both from their original position.)

Verify no references remain: `grep -rn "crt_warp_inset\|CRT_WARP_MAX_PX" src/ test/test_watchface.c` → empty.

- [x] **Step 2: Run the suite; recompute the bar fixture if it moved**

Run: `make -C test test 2>&1 | tail -25`
Expected: likely GREEN as-is — precomputed: with M, `crt_warp_sx(54,20)=52`, `crt_warp_sx(56,20)=54`, `crt_warp_sx(145,20)=147`, so the G-bar fixture's three assertions (centre 56 full, both edges pulled) hold under the new map. If any differ, recompute expected columns from `crt_warp_sx` on the bar's columns and update only those lines — never weaken the assertions.

- [x] **Step 3: Format gate**

Run: `make format-check && make test`
Expected: clean.

- [x] **Step 4: Regenerate both captures**

Appearance changed materially (side bow). Emulator flash was factory-reset earlier; healthy.

Run: `make visual-baseline && make visual-check`
Then: `pebble screenshot --emulator emery --no-open screenshot_current.png`
Expected: `visual-check (attempt N): 0 pixels differ outside the masks`; screenshot saved.
If the emulator hangs again: the fix from earlier today is to move aside `~/.pebble-sdk/4.33.1/emery/qemu_spi_flash.bin` and retry (report that you did).

- [x] **Step 5: Report**

Full report to the path given by dispatch. Surface explicitly: side-bow magnitude observed in the new capture vs old (read both PNGs if you can; otherwise note the geometry), and confirm stage 1/2 and strike were untouched.

---

## Self-Review

**Spec coverage:** §3 curvature paragraph — radial term adopted ✓, geometry dithering not reintroduced ✓, integer sampling kept ✓ ("Keep the warp integer"), seam arithmetic preserved in commit-comment/test rationale ✓. Everything else of the spec was Tasks 1-3 of the previous plan.

**Placeholder scan:** all test bodies and the full stage-3 block are concrete; sx values precomputed (sx(54,20)=52, sx(56,20)=54, sx(145,20)=147; sx(195,113)=199; sx(4,113)≤0; M pins 65536/67785/68864/72192). ✓

**Type consistency:** `crt_warp_q16`/`crt_warp_sx` used identically in tests and the pass; `CRT_WARP_R2_K` defined once in crt.h and used by both crt.c occurrences. yterm/s_ca_xq exist at the stage-3 insertion point (verified against the current crt.c: `yterm` computed near the row top, `s_ca_xq` cached at function top). ✓

**Known intentional mid-state:** after Task 1 the tree does not compile (stage 3 still references the removed function) — tasks run back-to-back under one controller; the plan says never to hand that state onward. ✓
