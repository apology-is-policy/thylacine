# AURORA-CONFIG.md — the Aurora/Tapestry configuration subsystem + the "make Quake real" arc

**Status**: DESIGN. Landed as scripture per the CLAUDE.md "design
conversation → scripture commit FIRST → then code" discipline.

- **2026-07-21** (aux session): the initial pass — Track A (make Quake play)
  + Track B (the config subsystem), with two forks left for signoff.
- **2026-07-23** (this session): **Track B rewritten to the push-on-start,
  per-environment ownership model** (§3), after a design conversation that
  worked through the "Aurora and Halcyon share the compositor but want
  different settings" tension. This SUPERSEDES the earlier §3.5 "launchable
  Kaua configurator" (the config UI is a **built-in overlay**, not a
  spawnable client) and refines the earlier single per-component config file
  into **per-environment ownership**. The §4 forks are resolved.

This doc carries **two tracks** that share the "make the graphics real, not a
demo" spirit:

- **Track A — make Quake truly PLAY** (§2): the interactive-play E2E +
  mouse-look + a frame limiter + multiplayer over `/net`. Largely LANDED
  across the gfx merges (see `project_tapestry_design_pass` memory + the G-7
  close); kept below as the record.
- **Track B — the Aurora config subsystem** (§3): config-as-policy owned by
  the environment and pushed onto the mechanism components at session start,
  with a built-in Turbo-Vision-style OSD (F10) as the front-end.

---

## 1. The unifying insight: config is POLICY the environment pushes (the Plan 9 move)

The compositor's personality is currently frozen at **compile time** — the
Bonfire palette, the 10×22 cell, the `Super+F` chord, the `DEFAULT_DISPLAY_W/H`
(`usr/tapestryd/src/gpu.rs`), the chord `const`s (`usr/tapestryd/src/server.rs`,
`fn is_chord` / the `KEY_F => zoom` match). Track B's thesis: **that personality
is not the component's to own — it is POLICY, owned by the user's ENVIRONMENT
and pushed down onto the components at session start.**

This is pure Plan 9 mechanism/policy separation — the model TAPESTRY.md §14/§15
already espouses for Halcyon ("`halcyon.rc` is a shell script writing files, not
a config DSL; the compositor is a file server"). Track B extends it
**symmetrically to Aurora**, and the payoff is that **Aurora and Halcyon can run
different settings on the SAME shared compositor** (§3.1). The config file
becomes greppable Plan-9 `key value` text; the OSD (§3.6) is the built-in editor
that mutates it and re-pushes live. The precedent that "edit → poke a ctl → apply
live, no restart" works is already in the tree: `/dev/tapestry/ctl` has
`clock-rate` (a live-settable global, `server.rs:2784`).

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
  wall).
- The SDL pump's `TEV_PTR_*` arms → `SDL_SendMouseMotion`/`SDL_SendMouseButton`
  (relative mode computes deltas driver-side from successive positions).

Audit-light (pure userspace + the manifest); the tablet MMIO/DMA safety is
inherited from the audited virtio-input path.

### 2.3 #51 — the frame limiter (the ELEGANT route)

Interactive Quake presents as fast as it can → spins a vCPU. Don't busy-present:
**block on tapestryd's next FRAME event** (the compositor already ticks at
`clock_hz`, `main.rs:frame_tick`), so the app presents exactly at refresh —
zero core-spin, no tearing, and it composes with the maximize model. Realization:
the `SDL_thylacine` present path (or a `host_maxfps` cvar) gates the present on
receiving a FRAME event from the pump before the next `SDL_UpdateWindowSurface`.
Fall back to a torpor-nanosleep budget (patch 0022) if FRAME pacing is
unavailable. This is also the honest close of the G-7d F1 lineage — the FRAME
clock becomes a *deliberate* pacing source.

### 2.4 Multiplayer over `/net` (the UDP soak test)

`tyr-quake` has `net_udp.c` compiled in (NQ UDP netcode); the pouch
`AF_INET → /net/udp` path is live; DHCP hands the guest the slirp gateway
(10.0.2.2). The **guest side already works**. New work is HOST-side: a Quake
server on the Mac (`quakespasm -dedicated` or a `-DSERVERONLY` tyrquake build),
a slirp UDP route in `tools/run-vm.sh`, and the guest `tyr-quake +connect`.
Then real game traffic pounds netd's dataplane — `go build`-is-the-oracle
applied to the network. **Note**: netd `/net` is main-track's; the CLIENT side
+ the host recipe are pure aux; any netd change coordinates with main.
Candidate NET-arc stress fixture (#52).

---

## 3. Track B — the Aurora config subsystem (push-on-start, per-environment)

### 3.1 The ownership model — environment = policy, components = mechanism

The tension the design conversation worked through: **Aurora and Halcyon want
different settings, but they share the compositor.** The presentation stack is
three roles:

- **tapestryd** — the compositor: owns the GPU/scanout, the keyboard, the chord
  plane, resolution, panes. **SHARED** by both environments (TAPESTRY.md §14:
  Halcyon is "the compositor's first client").
- **the renderer** — Aurora's cell grid → pixels; Halcyon's TTF surfaces.
  **Per-environment.**
- **the environment** — the session leader `login` hands the session to (A-5).

The factoring that resolves the tension:

- **Components are mechanism.** tapestryd and the renderer HOLD whatever they
  are told; they own no persisted personality.
- **The environment is policy.** Each environment (Aurora, Halcyon) owns its
  OWN complete config and, as the session leader, **PUSHES** it down on session
  start — `mode W H` / `chord ...` / `gaps ...` to the compositor ctl, palette /
  font to its own renderer. Switch environments → the new session leader
  re-pushes ITS config, overriding.

Different settings per environment falls straight out, and the compositor is
stateless-of-personality between sessions. This is exactly the Plan 9
mechanism/policy discipline already applied to Halcyon (`halcyon.rc` is policy;
the compositor is a file server), made symmetric across both environments.

### 3.2 The config files — per-environment, with a pre-login system default

- **Per-user, per-environment**: `$home/lib/aurora` and `$home/lib/halcyon` —
  each a COMPLETE `key value` config (one setting per line, `#` comments) its
  environment pushes. This **refines** the 2026-07-21 single `$home/lib/tapestry`:
  config is owned by the ENVIRONMENT, not keyed to the shared component.
- **System defaults for pre-login**: tapestryd + aurora come up for the LOGIN
  SCREEN before any user session exists, so there is no environment to push. A
  system default (`/lib/aurora/config`, `/lib/tapestry/config`) seeds the
  pre-login state; the per-user file pushes AFTER login. The Plan 9 `/lib`
  default + `$home/lib` personal idiom (per-user files are Stratum-home-backed
  → persistent + per-user-encrypted for free).
- **The writer defines the tier (the cfg-2 refinement, 2026-07-23 — forced by
  an isolation fact the original pass did not confront):** aurora is a
  PRE-LOGIN SYSTEM process (spawned by joey post-pivot, `PRINCIPAL_SYSTEM`,
  the full system namespace); the user's home is per-user-encrypted and
  mounted in the SESSION's namespace — so aurora can never read or write
  `$home/lib/aurora`, and must not. Ownership therefore splits by writer:
  - `/lib/aurora/config` is **the device's memory** (the monitor-OSD
    semantic — a monitor persists its own OSD settings): aurora READS it at
    startup (the pre-login theme) and the F10 OSD WRITES THROUGH to it
    (write-tmp + fsync + rename, the A-1.6 swap discipline). Baked with
    defaults by the host populate (the `/lib/ndb/local` pattern).
  - `$home/lib/aurora` is **the session's**: read AND written session-side
    only, pushed to the components at session start. The renderer push
    channel is **in-band private OSC over the console wire** (the
    terminal-native mechanism — xterm's dynamic-colors OSC 10/11 shape; the
    drain already carries every console byte to aurora, so the channel adds
    ZERO new kernel/9P surface); a tiny session tool emits it at login.
    A session push applies for the session; it does NOT rewrite the system
    file. An OSD "save to my profile" needs a session-side agent — a
    recorded seam, not v1.0. **The session-scoped lifetime is realized by
    reset-first (cfg-2b as-built)**: aurora is a boot-long process, so
    `aurora-push` ALWAYS emits a `reset system` verb (re-seed from the
    system file) before the user's lines — every session start is
    deterministically *system defaults ⊕ user overrides*, and a stale
    prior-session push dies at the next login. The channel's trust posture
    is the xterm dynamic-colors one (any console writer can emit it):
    cosmetic, session-scoped, non-persisting, aurora-local ONLY — it must
    NEVER gain a persisting or authority-bearing key (the write-through
    stays OSD-only; the compositor tier rides the gated ctl, §3.3, never
    OSC).
- **Format**: plain `key value` text — greppable, hand-editable, ndb-adjacent
  (netd already parses ndb); no binary format, no ABI. Example (Aurora):
  ```
  mode        1280 1024     # or: mode auto (largest the GPU reports)
  font-scale  1.25          # aurora cell size multiplier
  theme       bonfire       # aurora palette
  gaps        4             # inter-pane gap px
  zoom-policy  letterbox    # fixed-size-app placement (§3.5)
  chord       super+f zoom
  chord       super+t tab
  ```

### 3.3 The apply-authority gate (the leak-closer — LOAD-BEARING new work)

**The whole design's soundness rests here.** GROUND TRUTH
(`usr/tapestryd/src/server.rs::global_ctl`, verified 2026-07-23): the global ctl
is currently **UN-gated for authorization** — `clock-rate <hz>` is a real global
mutation any connected client that can reach `/dev/tapestry/ctl` may write.
Access is purely namespace-shaped (I-1/I-28: the mount must be in your
territory). *Surface* ctls ARE ownership-gated (`surf_owned`, server.rs:2765);
the GLOBAL ctl is not.

The push-on-start model adds the first AUTHORITY-BEARING global verbs — `mode` /
`chord` / `gaps` reconfigure the SHARED hardware. These MUST be gated to the
SESSION LEADER (the environment), or the privilege leak reopens: a
hosted-in-Halcyon aurora terminal, or any app in a territory that includes
`/dev/tapestry`, could drive the system resolution. **This gate is what lets the
OSD be renderer-owned (§3.6) without being a privilege hole** — the trust lives
in *who may apply* config to the shared compositor, not in *who draws or owns* it.

The gate mechanism — **RESOLVED at cfg-3 (2026-07-23; ground-truth-driven)**:

- **Peer-identity, realized as the kernel renderer-role stamp.** Every
  libtapestry client opens `/srv/tapestry` itself (`Surface::fullscreen`/`open`
  → open=connect → a FRESH SrvConn per client), so tapestryd holds a
  kernel-stamped per-conn identity (`t_srv_peer` on the accepted handle). The
  discriminator the identity record lacked — "is this peer THE bound console
  renderer?" — is added as **`SRV_PEER_FLAG_CONSOLE_RENDERER`**, bit 0 of
  `srv_peer_info.flags@32` (previously reserved-0 → APPEND-ONLY ABI), stamped
  inside the same alive-gated `g_proc_table_lock` walk as `caps` against the
  G-4 single-holder `g_console_renderer`; a dead/reaped peer fail-closes to 0
  exactly like caps. tapestryd checks it **per authority write** (live →
  revocation-correct when the role moves or its holder dies). This is the
  "kernel-side session-leader signal" §6 anticipated, landed with full kernel
  treatment (kernel test + ARCH §25.4 addendum + the SMP gate re-run).
- **The admitted set at v1.0 = {the bound console renderer}.** Aurora IS both
  the renderer and the environment (AURORA.md §2), so the environment-that-
  exists is exactly the role the kernel already tracks single-holder. The
  *session-leader* admission (a session-side environment pushing compositor
  config) is the Halcyon-era extension, landing with the task-#42
  multi-untrusted-client model — the gate grows an admitted set, it does not
  change shape.
- **Namespace-only confirmed insufficient — and the shared mount is denied by
  construction**: the `/dev/tapestry` mount is joey's own `/srv/tapestry` conn
  MREPL-mounted, so any session write through it arrives with JOEY as the
  conn peer — never the renderer → E_PERM. Authority requires your OWN conn
  AND the live role; the mount conveys visibility only.
- **`clock-rate`: FOLDED under the gate.** Ground truth: no writer exists
  anywhere in the tree (only the server arm + the ctl read format + comments),
  so gating costs nothing — and grandfathering an ungated global mutation
  would contradict the chunk's point. Supersedes the G-6d F3
  "same-session-trust" doc'd posture.
- **The determinism verbs (`test-mode`/`tick`/`release`) stay OUTSIDE the
  gate.** Their production posture is already the #880 cargo-feature strip
  (E_OPNOTSUPP in a `--no-default-features` build); in test builds the whole
  guest is a harness and the in-guest battery — a non-renderer client — must
  keep driving them. `release` keeps its F2 per-surface ownership gate.
  Global-ctl READS stay ungated (the geometry query every client boots on).

Because this touches the compositor's trust boundary, the gate lands with a
**focused audit pass** (unlike the rest of the OSD, which is pure aux).

### 3.4 The three resolution layers (do NOT conflate them)

Reframed as values the environment PUSHES (they were always three distinct
things):

1. **Display / scanout resolution** — what the whole screen runs at. Machinery
   EXISTS: `gpu.set_scanout(resource_id, w, h)` + `self.width/height`
   (`gpu.rs`). New work: a gated `/dev/tapestry/ctl` verb `mode W H` (or
   `mode auto`) that re-sets the scanout AND fans a CONFIGURE out to every
   surface + aurora (the G-6b reweave protocol already drives per-surface
   resize). Validation (refined at cfg-3): base virtio-gpu reports ONE
   preferred rect (GET_DISPLAY_INFO), not a mode list — so `W H` validates
   against sane bounds (320×200..3840×2160; the screen buffer stays under
   the 64-MiB weave cap) and `mode auto` re-probes GET_DISPLAY_INFO
   (non-mutating query) and adopts the reported rect. The scanout swap
   mints a FRESH per-generation screen resource (bind-then-flush per #57;
   the old screen freed only after the rebind); held presents drop (stale
   geometry); the layout recompute + the fan ride the audited
   `reconcile()` unchanged.
2. **Aurora cell/font size** — the shell's *effective* text resolution.
   `font-scale` changes the cell dims → more/fewer cells at the same display px.
   Renderer-side (the cell metrics + Cornucopia scale), NOT a GPU mode change.
   What most users mean by "bigger text."
3. **What a program requests** (Quake) — you cannot *force* an app's requested
   size. What the compositor controls is the **zoom / placement policy** on
   `Super+F`: **letterbox** (RESOLVED 2026-07-21 — `Comp::letterbox()`, the one
   geometry authority; nearest-neighbor scale, bars are pane background;
   #56 latches by present style so an accumulator client crops instead). The
   config exposes `zoom-policy` with letterbox as the default.

### 3.5 Runtime chords (the one tapestryd behavior the environment pushes)

Today the chord dispatch is a compile-time `match` on key codes. For remapping
it consults a **runtime map** built from the config's `chord` lines — pushed by
the environment at start + on the OSD's re-push. The chord PLANE reservation
(Super-held swallows everything — G-6c) is unchanged; only the code→action
binding becomes data. On the audited G-6c surface — prosecute that the
swallow-set / press-release pairing stays intact across a live rebind (a rebind
mid-chord). The rebind arrives via the gated global ctl (§3.3), so only the
session leader can remap.

### 3.6 The OSD — the built-in overlay (SUPERSEDES the old §3.5 Kaua app)

The config UI is a **native built-in overlay in the renderer**, NOT a spawnable
client. For Aurora: **aurora draws it** — it already has the Cornucopia atlas +
cell grid + `render_rows`, and it already RECEIVES F10 as a `TEV_KEY` and drops
it (`main.rs::key_bytes` has no arm for code 68), so intercepting it is nearly
free.

- **Trigger**: plain **F10** — aurora-local grab (aurora eats it from its own
  event stream when the console pane is focused; no global key-shadow, no
  compositor change for the trigger). `Super+F10` is the alternative if a
  global, always-available grab is ever wanted (that would move the grab to the
  compositor chord plane).
- **Style**: deliberately **Turbo-Vision raw** — double-line box borders
  (Cornucopia box-drawing block), a DISTINCT high-contrast palette (the classic
  TV blue field / gray dialog / yellow highlight / red accent) so it reads as
  "system dialog, not your Bonfire session," a drop shadow, a block cursor.
  Contrasty vs Kaua's fine style, by design — this is what signifies "an Aurora
  built-in," and "leave the pretty configuration to Halcyon."
- **Behavior**: live-applies immediately (the renderer's own state; the
  compositor tier later via the gated ctl §3.3) AND persists by WRITING
  THROUGH to `/lib/aurora/config` — the system tier the OSD, as part of the
  pre-login system renderer, legitimately owns (§3.2 "the writer defines the
  tier"; the per-user `$home/lib/aurora` is the session's, pushed via OSC).
  The reweave/resize fan-out rides the audited G-6b CONFIGURE protocol.
- **Scope (v1)**: Display (mode/resolution — LIVE since cfg-3: the Mode row
  cycles a pending preset, Enter applies through the gated ctl, persists on
  acceptance; zoom-policy info) + Appearance (palette, cursor; font-scale a
  later row); Chords + Session as later tabs. The system/display tier.
- **Trust**: "Halcyon can never invoke it" holds by construction — it is not a
  program, it is part of the Aurora renderer; Halcyon is a different environment
  with its OWN settings UI over its OWN config file. The apply-authority gate
  (§3.3) is what stops a hosted aurora terminal from driving the shared
  compositor even though the *Aurora environment* legitimately can.

Kernel byte-unchanged for the OSD itself (pure aux). The one component that
touches a trust boundary is the §3.3 apply-authority gate.

---

## 4. Resolved design decisions

1. **Ownership model** — RESOLVED (2026-07-23): **environment owns config,
   pushes to mechanism components on session start** (§3.1). Alternatives
   considered + rejected: (a) *compositor owns all config* — cannot give Aurora
   and Halcyon different settings on the shared compositor; (b) *renderer /
   client owns config and pushes it ungated* — reopens the privilege leak (any
   client drives the shared hardware). The push model + the §3.3 apply-authority
   gate gets both properties (per-environment settings AND no leak).
2. **Config file** — RESOLVED: per-environment `$home/lib/{aurora,halcyon}` +
   system `/lib/...` defaults for pre-login (§3.2). Refines the 2026-07-21
   per-component `$home/lib/tapestry`.
3. **The OSD** — RESOLVED: built-in aurora-drawn Turbo-Vision-raw overlay,
   F10, edits + live-pushes (§3.6). SUPERSEDES the 2026-07-21 launchable-Kaua-app
   design.
4. **Apply-authority gate** — RESOLVED as REQUIRED (§3.3); the mechanism
   PINNED at cfg-3 (2026-07-23): peer-identity via the kernel
   `SRV_PEER_FLAG_CONSOLE_RENDERER` stamp (append-only `srv_peer_info.flags`
   bit), checked live per authority write; admitted set at v1.0 = {the bound
   console renderer}; `clock-rate` folded under the gate; the determinism
   verbs stay feature-stripped outside it. The leak-closer.
5. **Config scope** (both `/lib` defaults + `$home/lib` personal) and **zoom
   policy** (letterbox) — carried from the 2026-07-21 resolved forks, unchanged.

---

## 5. Sequencing

- **Track A** (largely landed across the gfx merges) — the record is §2.
- **Track B** (this arc). Build-order refinement (2026-07-23, at chunk 1):
  the trust boundary touches ONLY the compositor tier (`mode`/chords/gaps →
  the gated ctl); the **aurora-local settings (theme, cursor) apply with no
  gate at all** — so the visible artifact front-loads and the single
  audit-bearing chunk lands last, cleanly scoped, with a working UI to
  drive it:
  1. **The OSD shell + Appearance** (§3.6; pure aux, gate-free) — F10, the
     Turbo-Vision panel, Display info-only + Appearance live-applying the
     aurora-local settings (session-lived). CHUNK 1 — BUILT.
  2. **Config-file persistence + push-on-start** (§3.2), split by tier:
     **cfg-2a** — the SYSTEM tier: bake `/lib/aurora/config`, aurora reads it
     at startup (pre-login apply), the OSD writes through (the persist loop
     closes; the monitor-OSD semantic). **cfg-2b** — the PER-USER tier: the
     private-OSC push channel + the session push tool + the login hook.
  3. **The compositor tier + the apply-authority gate** (§3.3/§3.4) — the
     `mode W H` verb + the CONFIGURE fan-out (composes G-6b's
     generation-fence reweave), landing WITH the gate + its focused audit
     pass (do not ship an ungated `mode`); the OSD's Display section goes
     live here. CHUNK cfg-3 — BUILT (the kernel renderer-role stamp + the
     gated `mode`/`clock-rate` + the OSD Display live + push-on-start +
     the OSC allowlist; `ls-gfx-mode.exp` + the battery gate leg +
     `devsrv.srv_peer_renderer_flag`).
  4. **Runtime chords** (§3.5) + the niceties (gaps etc.) — additive, each
     a small component read of a pushed value.

## 6. Seams + coordinate-with-main

- The `mode W H` reconfigure-all rides the G-6b CONFIGURE/reweave protocol
  (already audited) — no new invariant, but re-run the `tapestry_present.tla`
  buggy cfgs if the retire/reweave ordering is touched.
- The apply-authority gate is the ONE trust boundary — a focused audit pass, and
  if the chosen mechanism needs a kernel-side session-leader signal (vs a
  tapestryd-side peer-identity check) that is a coordinate-with-main item.
- The runtime chord table is on the G-6c surface — the swallow-set discipline
  (256-bit plane, press/release pairing) must survive a live rebind.
- Halcyon's own config + settings UI (its `$home/lib/halcyon` + pretty settings)
  is Phase-8 work; Track B builds only the Aurora half + the shared-compositor
  gate the Halcyon half will reuse.

## Cross-references

- `docs/TAPESTRY.md` §14 (Halcyon / the anti-window environment), §15
  (layout-as-9P / the compositor is a file server), §18.3 (the reweave
  protocol), §18.5 (`/dev/tapestry` tree).
- `docs/AURORA.md` §2 (Aurora names the renderer AND the environment), §4 (the
  `/dev/cons` backend + the winsize contract), §7 (the Halcyon relationship).
- `docs/KAUA.md` (the app-side TUI substrate — the "fine" style the OSD
  deliberately contrasts).
- `usr/tapestryd/src/server.rs::global_ctl` (the ctl the gate lands on),
  `usr/aurora/src/main.rs` (the renderer the OSD lives in).
- Tasks: #47 (mouse), #51 (frame limiter), #52 (multiplayer /net).
