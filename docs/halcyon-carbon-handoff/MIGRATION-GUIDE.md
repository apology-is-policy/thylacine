# UI migration guide — Daylight Halcyon to Instrument / Carbon Optics

## 1. Strategy and scope

Use a gated profile migration, not a rewrite of the graphical environment. Reuse Tapestry layout ownership, Halcyon's transcript/selection, the isolated kaua-term process model, glyph atlas and cartoon CPU executor. Keep the legacy profile available until the new mode passes real guest tests. All proposed symbols/paths in this guide must be reconciled with the actual repository; only documentation and mockup sources were attached.

This request authorizes writing this handoff and palettes, not modifying or deploying a native OS. The implementing agent should work on a branch, preserve unrelated changes, and follow the repository's approvals. Security gates and audit obligations in the supplied documents are invariants, not text to remove to get screenshots green.

## 2. Required discovery before the first patch

Record exact repository commit, target triple, build/run lane, enabled features, OS image provenance and current profile. Read AGENTS/CLAUDE or equivalent project instructions. Re-run documented native build and relevant host tests before changing anything; record pre-existing failures separately.

Locate current definitions/callers for Theme, KEYS, Metrics, Sheet, pane recompute, status carve, chrome binding, session declaration, renderer admission, theme wire, theme resolution/export, default-ink comparison, live/alt-screen transition, logical-line soft wrap, GlyphSource and layout cache generation. Confirm landed TY phases use skrifa/zeno and live subset mono rather than the older fontdue sketch. Confirm whether stacked containers already reserve every hidden child's header, where their strips paint, and who owns their hit-test rectangles. Do not assume the old5px tab strip is a32px Acme header.

Capture the existing Daylight/Nightjar screen, single-pane and split, and exercise a real shell, nora alt-screen entry/exit, resize, logout/console return and scale 100/200. Preserve baselines for trusted-console/Aurora. Inventory theme parser keys from code, not this kit's57 count; if it has changed, update conversion and validators deliberately before installing files.

## 3. Ordered implementation slices

Each slice has a separate commit, tests, screenshot where applicable, and rollback boundary. Names are suggested work packages, not assertions that new project issue IDs exist.

### I-0 — Freeze the oracle and ratify the visible delta

Import `reference/`, the exact palette register, fixtures and this contract into a test/reference area. Keep source hashes. Do not edit the reference to resemble the implementation. Add the dated visual/typographic/default amendment from spec§2; preserve safety invariants. Resolve the explicit Plex Mono vs Cornucopia identity change in the repository's policy workflow. Font substitution is not an implementation optimization.

Exit: a reviewer can distinguish normative target, historical policy, and permitted production exceptions. Native source owners and relevant test commands are documented. No runtime behavior changes yet.

### I-1 — Complete theme files, exact color types and new loader

Run stock native lint on all 13 files and add exhaustive tests enumerating current KEYS. Add an independent InstrumentColors type and strict v1 sidecar parser. Compile Carbon as the new profile fallback if the rollout requires it. Do not expose arbitrary CSS to no_std code; resolve aliases at build time. Add loader rejection tests and a generated build-time all 13 registry. Ensure sidecars do not enter the stock theme gallery by glob.

Thread `&ResolvedVisual` through painters without using DAYLIGHT constants or test-only accessors in production. Preserve legacy Theme and Daylight fixture visibility guard. A normal native cargo build is required: workspace tests alone can unify `theme-fixture` and hide a forbidden constant reference, as the attached audit records.

Exit: exact sidecar values roundtrip, no partial applies, legacy behavior unchanged. Current-schema palette previews are explicitly intermediate, not final screenshots.

### I-2 — Shared profile metrics and rail carve

Add shared InstrumentMetrics and pure physical conversion. Top34, bottom 25, outer3, divider7/2, flat frame1, header32. Do not widen the old bevel bounds to0 or overload `gap` with both3 and 7. Tapestry owns both rail reservations; halcyond paints them through properly gated non-hosted surfaces. Version/configure these roles rather than impersonating a full-display app overlay. Retain the old one-status-surface rule where applicable and add equally bounded upper-rail ownership.

Update every consumer of content rectangles, status/menu geometry, surface_target, hit-testing, cursor placement, pts winsize, damage and screenshot region readers. Remove legacy ring/tag deductions ONLY when Instrument is active. Theme/scale/profile changes all fan geometry and color updates even when extents remain unchanged. Single-pane Instrument still reserves rails and draws its frame/header. Keep application fullscreen policy separate.

Exit: rectangle partition and scaling tests pass; paint and reported geometry agree in guest at 100 and 200, including a color-only update. Aurora/prelogin legacy pixels remain unchanged.

### I-3 — Stable stack topology and collapsed-header placement

Introduce InstrumentStack presentation on the existing tree, or prove current Stacked satisfies the contract. Preserve hosted leaf identities and one process per tile. Centralize expanded child, focus and header geometry. Hidden bodies become dormant while their headers remain visible via the stack's chrome ownership. Update introspection/readback and layout serialization only through registered versions.

Migration of old layout: each old hosted leaf becomes a one-tile InstrumentStack wrapper; split directions preserve geometry; existing Stacked containers can become one multi-tile pane only if ordering/ownership are preserved; Tabbed remains a legacy mode or converts explicitly on opt-in, never silently discards children. Keep `halcyon-layout v1` reading intact. If v1 cannot preserve new stack/ratio/view metadata, introduce a versioned v2 writer and explicit importer; do not add unrecognized tokens and hope old readers ignore them. Layout save is not process checkpointing, and restore may spawn commands.

Exit: the 3-pane10-tile fixture matches order and rectangles; opening each tile shows exactly one body and stable headers; live processes survive repeated collapse; hidden bodies never receive input. Save/restore geometry roundtrip does not imply unsaved editor contents persisted.

### I-4 — Header/rail paint and semantic input

Implement complete state matrix: pane focus-neutral, short champagne active gutter, index, ellipsis, right metadata, close visibility and attention priority. Keep success/error out of old full-body hairlines in this profile. Add exact rails, context, transient status, clock, picker and help. Reference WORKSPACE01 is just one workspace label; no new workspace protocol needed.

Route actions through stable semantic IDs to the authoritative model. Preserve session/renderer gates on chrome, menus and rail surfaces; don't turn labels into ordinary hosted surfaces. Add keyboard actions with priority tests. Restore keyboard focus after re-layout/menu closure. Close and Reset acquire production confirmations; confirmation UI uses the same typography/panel tokens.

Exit: header click/cycle/close tests, directional focus, keyboard-only picker/help and no click-away release leakage pass in real guest. Status text cannot contain control characters or inject shell commands.

### I-5 — Typography, rich content and Rust semantics

Vendor exact Plex face assets under the repository's provenance/license workflow and produce a hash/coverage manifest. Configure Instrument weights400/500/600 and mono400/500. Decide and pin synthetic italic behavior for parity captures. Preserve existing raster mechanisms and compare advances/bearings at every supported scale. Retain fallback coverage for math, box drawing, Czech diacritics and lambda; fallback may change glyph shapes but must not corrupt fixed-grid alignment.

Build Instrument Sheet styles with viewport-relative padding/H1 sizing, explicit CSS-like margin collapsing, independent code/terminal grounds and baseline alignment. Add terminal-view versus rich-document presentation to the renderer, without conflating it with normal/alternate screen or Beacon tier. Utopia's real prompt becomes λ in this profile through its documented configuration/semantic mechanism; do not replace arbitrary ⊢ or `$` output bytes.

Implement Rust semantic style mapping with the 9 exact new categories. Reference mode consumes frozen span classifications; real editor uses tokenizer/LSP classifications. Update palette export/cooperative app handling so nora can use lifetime/punctuation as well as old9 roles; don't break the old program-agnostic export ABI. Add metadata producers only with explicit registry amendment if no current channel exists. The workspace proposal's pill/diag marks are not automatically available.

Exit: the renderer fixture matches line breaks and syntax, copy returns exact code, terminal input/caret remain aligned, `pre` remains mono and tables/objects remain interactive. Enter/exit nora repeatedly with no merged grids, lost Esc or stale background.

### I-6 — Divider interaction and layout safety

Implement7px tracks,2px rules and joints. Pointer capture belongs to one split and is cancelled safely on surface retirement/modal transition/logout. Match source ratio normalization and double-click behavior; keyboard step.025. Add recursive native usable minima and bounded tree depth using existing system caps. Refuse an impossible split atomically with status feedback, not overflow, negative geometry or process starvation.

During drag, resize real visible content only through the existing Configure/reweave/pts Resize path. Apply frame coalescing and backpressure; send the final size. Preserve scroll anchors and selection after reflow. Do not stop hidden process output from draining: bound retained output instead of deadlocking a child when its tile collapses. Escape ends drag at current ratio in reference parity mode; changing it to rollback is a separate product decision.

Exit: mouse/keyboard/end-outside/cancel tests, zero-sized-display resilience, minima refusal, stack header budget and terminal SIGWINCH verified.

### I-7 — Atomic live theme switching

Implement the versioned visual transaction in THEME-CONVERSION§5. Distinguish startup fallback from runtime rejection. Session, compositor, header/rail surfaces, content paints and future spawns use one visual generation. Existing app palettes need cooperative repaint; inherited `/env` does not update them. Add explicit partial-app-adoption reporting if an uncooperative truecolor application remains in old colors.

Persist user choice durably only after success; preserve existing choices; fresh selectionCarbon. Color-only change triggers visible redraw without relocating content; scale or font/weight change regenerates caches and pts metrics. Device/system theme and user overrides never cause unauthorized retint across principals.

Exit: all 13 switch without mixed Daylight frames or process resets; runtime malformed candidate leaves current theme intact; Busy retries bounded, E_PERM final; logout restores console's own theme.

### I-8 — Parity gate, security review and rollout

Run ACCEPTANCE-TESTS in full. Store artifacts in the repository's accepted evidence location with exact image/commit/font identities. Audit new parser/wire fields, geometry bounds, seat operations, popup grabs, dormancy and process lifecycle. Use the actual repository review policy; no specific model or subagent workflow is required by this handoff.

Rollout order: opt-in development profile → reference fixture gates → real-session trials → all 13 gallery and switch gates → default change for fresh users → optional operator-controlled migration for existing installations. Retain one explicit legacy switch until the new profile has passed the normal release period. No profile can bypass SAK or identity restrictions.

## 4. Per-slice evidence template

Record goal, touched owners, pre-change behavior, chosen source-backed design, tests run with literal commands and exit status, before/after captures, exact color/rect assertions, positive/negative controls, known incomplete conditions, rollback commit/config and whether a schema/protocol changed. A skipped guest leg is SKIPPED, not PASS. Host-only tests cannot prove a compositor-painted pixel or a kernel authorization check.

## 5. Rollback and data durability

Keep the legacy profile and parsers compiled. Before installation, retain the current explicit user/system theme files and layout definitions under unique backup names using the OS's supported durable-write path. Do not overwrite them with the bundled example. Switch profile to legacy, restart only if the current runtime requires it, and restore the user's prior preference—not the author's guessed Daylight values.

A palette rollback does not require killing terminal processes. A profile/protocol rollback may require a controlled session restart; warn that running processes and unsaved edits are not preserved by layout serialization. Prefer rolling back before production adoption rather than implementing fake process restoration. If a new layout format was written, retain the old reader and an explicit loss-aware exporter; do not let the old binary parse v2 as v1.

## 6. Non-goals that prevent scope drift

No kernel multi-console rewrite; no new VT parser; no replacement with a webview desktop; no demand for a new GPU backend; no force-recoloring third-party truecolor pixels; no9-workspace feature inferred from a label; no arbitrary user shaders; no visual redesign of the 13 themes; no permanent minimize/maximize controls or rounded floating windows. New safe native behavior is limited to the explicit production exceptions and mechanisms needed to implement this UI truthfully.
