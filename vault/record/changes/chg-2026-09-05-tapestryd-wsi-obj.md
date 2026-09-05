---
id: chg-2026-09-05-tapestryd-wsi-obj
type: chg
title: "sub-tapestryd brought current: the WSI presentable + present-source (W-3c/W-4) and the obj surfaces (H-3c menu, fid_clunk, H-3d status)"
date: 2026-09-05
arc: arc-vault
commits: ["704cc652"]
touched:
  - sub-tapestryd
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-09-05
---
The sub-tapestryd deltas main handed off across yip 0030/0031/0034 (Warp-WSI
W-3c-1/W-3c-2 + W-4) and 0038/0040/0041 (H-3c menu + its audit close, H-3c-2
fid_clunk, H-3d status bar). The dossier had ZERO of these -- main's own KT-1
append ("The declared seat") assumed the surrounding compositor but the six
feature arcs (2026-08-26 .. 2026-09-02) predate every dossier section, so this
is a real de-stale. Six dated sections appended in the dossier's established
pattern (core sections + Provenance + dated feature appends).

## Every load-bearing symbol re-verified against the code, not copied

Read `usr/tapestryd/src/{server.rs,gpu.rs,pane.rs}` for each; the handoff
notes were the SOURCE for what to look for, the code was the authority on what
is there (the "re-verify any figure" discipline). Confirmed present:
`MAX_WARP_IMGS_PER_CTX`=16 + `WARP_IMG`=1<<45 + `struct WarpImg` +
`wimg_teardown`/`wimg_destroy` + `warp_img_selftest` + `create_presentable`/
`set_scanout_blob`; `enum PresentSrc`/`AdoptSrc` + `GlAdopt` +
`direct_bind_adopted` + `gl_adoption` + `comp_import_bo`; `Cost::PokeBind/
PokeFlush` + `poke_hist_bind/flush[8]` + `release_displaced_gen` +
`ctx_guest_backing`/`ctx_hostmem_backing` + `img_poke_complete` +
`text_snaps` + `warp_stall_watch` (conn=/surf=); `Role::Menu` + `MenuState` +
`menu_place`/`menu_dismiss`/`menu_heal`/`menu_reassert`/`prefill_from_shown` +
`key_owner`/`btn_owner` arrays + `OWNER_SWALLOWED`; `fid_clunk` + the "minted,
never created" retire; `Role::Status` + `StatusState` + `status_rect` +
`P_STATUSBAR`=6 + the `statusbar` file + `status_bg`; pane.rs `Role::Menu`/
`Role::Status`.

## The sections

- **W-3c-1 the PRESENTABLE** -- the img/ object class; the AMENDED mint
  (USE_MAPPABLE never mapped, not USE_SHAREABLE -- virglrenderer refuses
  SHAREABLE on HOST3D, measured at round 2); the ORDERED teardown (unbind via
  gl_evict_res BEFORE unref = the spec's PUnbound, unconditional because
  gl_evict_res self-guards on the authoritative bound_res); the I-7 hazard
  running the OTHER way (the display holds the ref).
- **W-3c-2 the present source Bo|Img** -- pub-keyed consent (a freed handle's
  tenant cannot inherit it); direct_bind_adopted as one family dispatch; the
  composed machinery HARD-GATED to Bo as MEMORY SAFETY (an img va is 0);
  wimg_destroy consent-clear; the JOURNAL run-6 fork resolution (the run-5
  measurement was of the untypeable stand-in class, no scripture narrowed).
  The W-3d compose arm's PDrained pinflight-drain is recorded as the owed seam
  (buggy_pdrain_skipped is its counterexample).
- **W-4 the present windows** -- text_snaps (the per-fid gen pin + its three
  clear sites); the double-paint fix (PokeBind = whole rotated paint,
  PokeFlush = same-image re-poke; direct_bind_adopted flushes internally);
  the poke latency histograms (the instrument that measured the ~10ms host
  quantization); warp_stall_watch's identity fields; the ctx_guest/ctx_hostmem
  budget split.
- **H-3c the menu** -- Role::Menu (never hosted); the one Comp.menu; the gated
  place/dismiss verbs; THE GRAB via key_owner/btn_owner arrays (the audit-close
  restructure that replaced menu_swallow_btn + widened chord_down + fixed the
  ptr_btn ordering leak); compositor-owned dismiss; menu_heal/menu_reassert/
  prefill_from_shown.
- **H-3c-2 fid_clunk** -- the minted-never-created surface reaped on its ctl
  clunk (the compositor backstop for a client that clunks without libtapestry's
  fail_created destroy).
- **H-3d the status bar** -- Role::Status; the create gating (E_INVAL before
  the weave alloc); status_rect; the reconcile layout_h carve passed to
  recompute ONLY (shadowing dh bound the scanout -- the found bug); the
  statusbar file; the status_bg strip fill.

## Owed, tracked (not silently dropped)

- The #56-latch discriminator paragraph (the fullscreen-zoom fix): main is
  writing the chg+fnd for it now (yip 0052); I fold the FIXED-latch
  discriminator into the dossier from those records AFTER main's fix lands, so
  the dossier documents the fixed latch (partial-damage AND >=2 slots), not the
  buggy coverage-only one.
- The W-3d compose arm's PDrained drain (the W-3c-2 seam above) is unbuilt.
