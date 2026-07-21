# AURORA-CONFIG.md — the Aurora/Tapestry configuration subsystem + the "make Quake real" arc

**Status**: DESIGN (2026-07-21, aux session, Opus 4.8). Not yet built.
Design conversation held in-session; landed as scripture per the CLAUDE.md
"design conversation → scripture commit FIRST → then code" discipline. Two
of the design decisions need user signoff before the build (§4 forks).

This doc kicks off the aux arc after G-7 (SDL/Quake, merged into main
`5288efb0`). It carries **two tracks** that share the "make the graphics
real, not a demo" spirit:

- **Track A — make Quake truly PLAY** (near-term, concrete, small): the
  interactive-play E2E + mouse-look + a frame limiter + **multiplayer over
  `/net`**. User-chosen 2026-07-21.
- **Track B — the Aurora config subsystem** (design-heavy, scripture-worthy):
  config-as-a-file + runtime chords + the three resolution layers + a native
  Kaua TUI configurator. User-proposed 2026-07-21.

---

## 1. The unifying insight: config-as-a-file (the Plan 9 move)

The compositor's personality is currently frozen at **compile time** — the
Bonfire palette, the 10×22 cell, the `Super+F` chord, the `DEFAULT_DISPLAY_W/H`
(`usr/tapestryd/src/gpu.rs`), the chord `const`s (`usr/tapestryd/src/server.rs`,
`fn is_chord` / the `KEY_F => zoom` match at ~1558). Track B's whole thesis:
**move that personality into a config file in the namespace**, read by
tapestryd + aurora at startup, with a **runtime ctl** to apply changes live.
The precedent is already in the tree: `/dev/tapestry/ctl` has `clock-rate`
(a live-settable global, `server.rs:2417`), so the "edit a file, poke a ctl,
apply live — no restart" path is proven; this generalizes it.

The configurator (§3.5) is then just a *client* that edits the file and pokes
the ctl. Resolution, chords, and niceties all become one schema.

---

## 2. Track A — make Quake truly PLAY

Concrete, self-contained; ends with the user fragging a host bot over `/net`,
mouse-look and all, at a capped framerate. Build order (cheapest first):

### 2.1 The interactive-keyboard-play E2E (do FIRST — proves the input path)

The timedemo never exercised **input reaching the game through the compositor
focus routing** — the one unproven leg. `tyr-quake` is baked at
`/bin/tyr-quake`, `/quake` data is in the pool, `ut` launches it by bare name.

- Launch `tyr-quake -nosound +map start` from `ut`.
- **Super+F** to zoom the pane fullscreen (the chord vocabulary: Super+arrows
  focus / **Super+F zoom** / Super+Shift+arrows move / Super+H,V split /
  Super+T,S tab,stack / Super+Tab cycle / Super+Shift+Q close).
- Inject WASD/arrow keys via QMP `input-send-event` (routes to the console's
  PCI keyboard → tapestryd → the focused surface).
- Assert Quake reacts (the game frame changes in response, or a `+forward`
  console echo). Send the user a mid-game screendump.

This validates the compositor→client keyboard path for an SDL app. Needs a VM
boot (sequence off any concurrent SMP gate to avoid host contention,
[[feedback-no-host-load]]).

### 2.2 G-7c — mouse-look (#47)

The `TEV_PTR_MOVE/BTN/SCROLL` records are ALREADY reserved in the protocol
("kinds arrive with the tablet device"), and the `SDL_thylacine` pump's
`TEV_PTR_*` arms are already stubbed and waiting
(`usr/ports/sdl2/thylacine/SDL_thylacineevents.c`). Work:

- `virtio-tablet-pci` in `tools/run-vm.sh` (the co-page→PCI rule, like the
  keyboard).
- tapestryd `gather` manifest picks up the tablet function (the I-34
  allowance already carries multiple PCI functions — G-3).
- `usr/tapestryd/src/input.rs` consumes the tablet eventq; the compositor
  scales the absolute axes to display px, hit-tests the pane under the
  pointer, and emits `TEV_PTR_*` with **surface-relative** coords (the
  TAPESTRY.md §18.4 wire semantics — never absolute screen coords, the D5
  wall; this doc's earlier "absolute coords" note was imprecise).
- The SDL pump's `TEV_PTR_*` arms → `SDL_SendMouseMotion`/`SDL_SendMouseButton`
  (relative mode computes deltas driver-side from successive positions —
  SDL's warp emulation needs a warpable host cursor the backend lacks).

Audit-light (pure userspace + the manifest); the tablet MMIO/DMA safety is
inherited from the audited virtio-input path.

### 2.3 #51 — the frame limiter (the ELEGANT route)

Interactive Quake presents as fast as it can → spins a vCPU (the timedemo's
600 fps + the fps variance are the symptom). Don't busy-present: **block on
tapestryd's next FRAME event** (the compositor already ticks at `clock_hz`,
`main.rs:frame_tick`), so the app presents exactly at refresh — zero core-spin,
no tearing, and it composes with the maximize model (a fullscreen pane paces
the same as a side pane). Realization: the `SDL_thylacine` present path (or a
`host_maxfps` cvar) gates the present on receiving a FRAME event from the pump
before the next `SDL_UpdateWindowSurface`. Fall back to a torpor-nanosleep
budget (patch 0022) if FRAME pacing is unavailable. **This is also the honest
close of the G-7d F1 lineage** — the FRAME clock becomes a *deliberate* pacing
source, not just an incidental teardown wake.

### 2.4 Multiplayer over `/net` (the UDP soak test)

`tyr-quake` has `net_udp.c` compiled in (NQ UDP netcode); the pouch
`AF_INET → /net/udp` path is live (`net-3b UDP round-trip OK` in the boot
log); DHCP hands the guest the slirp gateway (10.0.2.2). So the **guest side
already works** — a `connect <host>!26000` flows netd → NIC → slirp. The new
work is HOST-side:

- A Quake server on the Mac: `quakespasm -dedicated` or tyrquake's own server
  build (a second `build_tyrquake` target with `-DSERVERONLY`), or simply an
  existing host Quake binary.
- A slirp UDP route in `tools/run-vm.sh` so the guest reaches the host server
  (the `10.0.2.2` gateway, or a `hostfwd`/`guestfwd` UDP mapping).
- The guest `tyr-quake +connect <host-ip>` client.

Then real game traffic — 60 Hz position deltas, entity deltas, the
reliable/unreliable channel split — pounds netd's dataplane: `go build`-is-the-
oracle applied to the network. **Note**: netd `/net` is main-track's domain,
but the CLIENT side (`tyr-quake` over `/net/udp`) + the host recipe are pure
aux. If a netd change is needed, that's a coordinate-with-main item, NOT an aux
change. Candidate NET-arc stress fixture (#52).

---

## 3. Track B — the Aurora config subsystem (the design)

### 3.1 The config file

- **Location** (§4 fork 1): recommend **per-user** `$home/lib/tapestry`
  (A-5 gives every user a home; chords/theme/resolution are personal), with a
  system default at `/lib/tapestry/config` that a per-user file overrides.
- **Format**: plain Plan-9 `key value` text (one setting per line; `#`
  comments), e.g.:
  ```
  mode        1920 1080      # or: mode auto (largest the GPU reports)
  font-scale  1.25           # aurora cell size multiplier
  theme       bonfire        # aurora palette
  gaps        4              # inter-pane gap px
  zoom-policy scale-fill     # or: letterbox | center  (§4 fork 2)
  chord       super+f zoom
  chord       super+t tab
  chord       super+Return   spawn ut     # (future: launch bindings)
  ```
  Rationale: greppable, hand-editable, ndb-adjacent (the tree already parses
  ndb in netd) — no new binary format, no ABI.
- Read at tapestryd startup + aurora startup; re-read on the `reload` ctl.

### 3.2 The three resolution layers (do NOT conflate them)

1. **Display / scanout resolution** — what the whole screen runs at. The
   machinery EXISTS: `gpu.set_scanout(resource_id, w, h)` + `self.width/height`
   update (`gpu.rs:552,620`). New work: a `/dev/tapestry/ctl` verb `mode W H`
   (or `mode auto`) that re-sets the scanout AND fans a CONFIGURE out to every
   surface + aurora (the reweave protocol already handles per-surface resize;
   this drives it from a mode change). Validate `W H` against the GPU's
   reported modes. This is "what resolution the shell runs in."
2. **Aurora cell/font size** — the shell's *effective* resolution in text.
   `font-scale` changes the cell dims → more/fewer cells at the same display
   px. Aurora-side (the renderer's cell metrics + Cornucopia scale), NOT a GPU
   mode change. This is what most users mean by "bigger text."
3. **What a program requests** (Quake) — you cannot *force* an app's requested
   size (it asks SDL/its CLI for what it wants: TyrQuake `-width/-height`, or
   fullscreen = display size). What the compositor controls is the **zoom /
   placement policy** (§4 fork 2): on `Super+F`, does a fixed-size surface
   **scale-to-fill** (a 640×480 Quake stretched to the display — best for
   games, cheap on the GPU blit), **letterbox** (preserve aspect), or stay
   at native size? This is the doc-142 "fixed-size-app-in-a-tiling-
   compositor" seam, now decided. (Ground truth from the 2.1 E2E dump:
   today's behavior is **top-left anchor** with black fill — surface (0,0)
   maps to the pane content origin, `blit_composed_pixels` — not centered
   as sketched earlier.)

### 3.3 Runtime chords (the one real tapestryd change)

Today the chord dispatch is a compile-time `match` on key codes. For
remapping, it must consult a **runtime map** (`HashMap<Chord, Action>` or a
fixed table) built from the config's `chord` lines at startup + on `reload`.
The chord PLANE reservation (Super-held swallows everything — G-6c) is
unchanged; only the code→action binding becomes data. Bounded, on the audited
G-6c surface — prosecute the swallow-set/press-release pairing stays intact
when the map changes at runtime (a rebind mid-chord).

### 3.4 Graphical niceties

`theme` (the aurora palette — Bonfire today; a named-palette table), cursor
style, `gaps`, maybe default new-surface placement (tile vs float). All
aurora-side or tapestryd-side settings the config exposes. Additive; each is a
small renderer/compositor read of a config value instead of a `const`.

### 3.5 The Kaua TUI configurator

A native libthyla-rs app on **Kaua** (the console TUI substrate; nora's
sibling — `usr/lib/kaua`). It:

- Reads the config file, presents **Display / Chords / Appearance** sections
  (a form/menu; Kaua's cell-diff renderer).
- On **Chords**: a "press the key to rebind" capture UI (the app reads raw
  keys via consctl raw-mode — the same LS-8b dance nora uses).
- On **save**: writes `$home/lib/tapestry` AND pokes `/dev/tapestry/ctl reload`
  so the change applies live (mode change, chord rebind, theme all
  live-apply).
- Persistent by construction (the file is in the Stratum-backed home).

Lives at `usr/aurora-config/` (or `usr/act/` — a thematic name TBD). Kernel
byte-unchanged; pure aux.

---

## 4. The two forks needing user signoff (before the Track-B build)

1. **Config scope**: per-user `$home/lib/tapestry` (recommend) vs system-wide
   `/lib/tapestry/config` vs both (system default + per-user override —
   recommend as the full answer).
2. **Zoom policy for fixed-size apps** (§3.2 layer 3): ~~scale-to-fill vs
   letterbox vs centered-native~~ — **RESOLVED: LETTERBOX (user, 2026-07-21,
   from live play: "stretch to the full size of the display, center and keep
   aspect ratio")**. Built compositor-side: `Comp::letterbox()` is the ONE
   geometry authority (the blit's forward map and `ptr_hit`'s inverse both
   derive from it — the G-7c audit-F3 lesson made structural);
   nearest-neighbor scale; the bars are pane background. The SDL backend
   honors `SDL_WINDOW_RESIZABLE`: a fixed-size window DECLINES the
   compositor's size offer (unacked CONFIGUREs are protocol-legal standing
   offers), so the dims mismatch persists and the compositor letterboxes —
   acking would reweave to the pane size while the app renders its fixed
   frame into the corner (the pre-fix zoomed-Quake top-left artifact).
   Track B's config file later exposes `zoom-policy` with letterbox as the
   default value. **Refinement (same day, from live play)**: letterbox is
   sound only for the whole-frame-presenter class — a full-slot scaled
   redraw is unsound for an accumulator client (aurora's cell-diff over
   rotating patchwork slots → the "utopia pane flipping" bug the first
   cut shipped). The interim size discriminator (fit-inside letterboxes,
   overflow crops) then clipped a 2px-overflowing split Quake, so the
   final form (**#56, the patchwork latch**) discriminates by PRESENT
   STYLE, which is protocol-observable: `Surface.patchwork` latches
   one-way on the first partial-damage present; unlatched surfaces
   letterbox any mismatch (up or down), latched ones take the
   damage-clipped CROP. Aurora's CONFIGURE-tracking resize (re-grid +
   winsize propagation) is the real close of the oversized-console case
   — tracked as task #55.

---

## 5. Sequencing

- **Track A first** (small, delivers the play experience the user is excited
  about): 2.1 (play E2E) → 2.2 (mouse) → 2.3 (frame limiter) → 2.4
  (multiplayer). Each is a self-contained sub-chunk with its own gate.
- **Track B after** (the design-heavy arc): land the §4 fork decisions, then
  the config-file + resolution ctls + runtime chords + the Kaua configurator.
  This is a multi-sub-chunk arc; the `mode W H` ctl + reconfigure-all is the
  load-bearing piece (it composes with G-6b's generation-fence reweave).

## 6. Seams + coordinate-with-main

- Multiplayer touches netd `/net` (main-track) — the client + host recipe are
  aux; any netd change coordinates with main.
- The `mode W H` reconfigure-all rides the G-6b CONFIGURE/reweave protocol
  (already audited) — no new invariant, but re-run the `tapestry_present.tla`
  buggy cfgs if the retire/reweave ordering is touched.
- The runtime chord table is on the G-6c surface — the swallow-set discipline
  (256-bit plane, press/release pairing) must survive a live rebind.

## Cross-references

- `docs/TAPESTRY.md` §14 (Halcyon / the anti-window environment), §17
  (tiers), §18.3 (the reweave protocol), §18.5 (`/dev/tapestry` tree).
- `docs/reference/142-sdl-port.md` (the "fixed-size app" seam), 143-tyrquake.md.
- `docs/AURORA.md` (the environment half), `docs/KAUA.md` (the TUI substrate).
- Tasks: #47 (mouse), #51 (frame limiter), #52 (multiplayer /net).
