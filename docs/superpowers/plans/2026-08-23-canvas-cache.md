# Canvas bitmap cache Implementation Plan

> **For agentic workers:** one worker task + one review. Steps use `- [ ]`.

**Goal:** Replace per-render scene redraw (fonts, frames, text) in `canvas_update_proc` with a cached GBitmap blit, redrawn only when content changes. Motivation: strike ticks repaint the whole window each frame (~38 ms of font/frame painting on top of the 55 ms CRT pass) and the stock audio ring starves at ~93 ms blocked/tick. Cache cuts the painting cost to a blit every render — strike ticks and minute ticks alike.

**Architecture:** drawing.c owns a full-screen GColor8 GBitmap cache built over a static buffer. `canvas_update_proc`: cache valid → `graphics_draw_bitmap_in_rect` of the cache over the canvas frame; invalid → draw the scene exactly as today, then capture the framebuffer and memcpy the canvas region back into the cache. Every existing `layer_mark_dirty(s_canvas_layer)` site that signals content change gets a cache-invalidate call alongside.

**Spec:** none formal — rationale and measurements live in the session (2026-08-23: 93 ms blocked/tick ≈ 55 pass + 38 non-pass; ring 128 ms; frozen-strike probe proved renders starve audio).

## Global Constraints

- Format must be framebuffer-native (PBL_COLOR GColor8 per the crt.c comment: "the captured bitmap IS the framebuffer on a native-format platform"); verify against the platform header comment before choosing the GBitmap format and stride. Blit and snapshot use whatever the mock/platform offer; the snapshot memcpy uses `gbitmap_get_data(fb)` row layout — on emery stride == width == 200 but confirm.
- RAM: static 200×228 = 45,600 B. Today's image reports Total footprint 30,982 B / 128 KB — verify the `pebble build` memory report after; state the new number.
- The CRT overlay composits AFTER canvas (existing z-order) — the cached blit must land before the overlay pass, content byte-identical to a live redraw. The strike must look unchanged.
- Idle path must not get MORE expensive: valid cache → exactly one blit, no extra work.
- Every content-change path must invalidate (clock minute, settings/theme push, weather/data arrival, startup). Missing one = stale display — this is THE hazard; list the sites in the report (grep `layer_mark_dirty(s_canvas_layer)` across src/).
- Do not touch crt.c.
- VCS: jj — no mutations, no git, no commits. Working tree only.
- Mock (`test/pebble_mock.c`/`pebble.h`): add `graphics_draw_bitmap_in_rect` (count + clip rect) if absent; framebuffer capture/release mocks exist (CRT tests use them). Add a test that a second render after content change uses the blit path (draw-call counts drop to blit-only, and the framebuffer after blit equals the first render's).

## Task 1: the cache

**Files:**
- Modify: `src/c/drawing.c` (cache + two-path update proc; an exported `canvas_content_changed()` invalidator if the site list needs one)
- Modify: callers in `src/c/main.c` / `src/c/messaging.c` (only to add invalidation at mark-dirty sites)
- Modify: `test/pebble.h`, `test/pebble_mock.c` (mock blit + capture tracking as needed)
- Test: `test/test_watchface.c` (fresh-render-vs-cached-render equality test)

**Steps:**

- [ ] **Step 1:** grep the invalidate-site list first; write it in the report.
- [ ] **Step 2:** implement + unit test (scene equality between live and cached render on the mock framebuffer).
- [ ] **Step 3:** `make format-check && make test` green; `pebble build` memory report recorded.
- [ ] **Step 4:** `make visual-baseline && make visual-check` — idle frame MUST be unchanged (if it moves, the cache broke a draw path; investigate before accepting a new baseline).
- [ ] **Step 5:** report: site list, memory delta, test/gate evidence.
