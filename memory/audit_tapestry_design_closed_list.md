# Tapestry graphics-phase DESIGN PASS — audit closed list

Cumulative do-not-re-report set for the Tapestry/Halcyon graphics-phase design
(TAPESTRY.md §18 + `specs/tapestry_present.tla`). This is a DESIGN + SPEC review
(no impl exists); findings are design flaws / unsound bindings / underspecs /
model-fidelity gaps, not code bugs. Any later re-audit (incl. the G-2/G-3 impl
audits) treats these as closed at the DESIGN level — re-prosecute only their
IMPL realization.

---

## Round 1 — 2026-07-17 (Fable-max holotype + concurrent self-audit)

Scope: commits e3553f3d (TAPESTRY §18 + companion syncs) + 005591ba
(tapestry_present.tla + 6 cfgs). Prosecutor: holotype-reviewer, MODEL(start)==
MODEL(end)==Fable 5 (claude-fable-5) — no mid-run fallback; findings trusted at
full weight. Self-audit ran concurrently and independently found 8 of the 16
(SA-1..SA-8 == F2/F4/F7/F6/F5/F9/F16-overlap/F11); the holotype added the two
P1s the self-audit missed (F1 DMA-I-5, F3 Weft-lifecycle) + F8/F10/F12-F15.

**Counts: 0 P0 / 4 P1 / 6 P2 / 6 P3.** All closed as binding amendments in
TAPESTRY §18.11 (+ inline corrections to §18.0/§18.1/§18.3 for the load-bearing
ones) + spec edits. NOT a dirty close in the code sense (no runtime surface),
but P1+P2 = 10 ≥ 6 AND load-bearing bindings changed (the DMA delta narrowed;
the map-path re-scoped; a spec action added) → a **round-2 holotype on the
amendment is warranted** while Fable is available (the design-review analog of
the dirty-close re-audit).

Amendment commit: <pending>. Verified SOUND set (do not re-litigate): the 4
buggy cfgs faithfully target their invariants; the clean spec's structural
exactly-once + forward-only displayed switch + GoneClean; the #847 cross-Proc
dual-refcount (Weft-2-audited) for the ANON case; the DMA client PTE attr is
Normal-WB not Device (the attr axis of "no hw authority" holds — F1 is about
region CONTENTS); share_id unforgeable to EL0; compositor-owns-keymap needs no
keymap-update event (clients get pre-resolved runes + layout-independent raw
codes); test-mode/TPRESENT_HOLD build-time-gated; W^X on the weave (RW never X,
enforced by burrow_share_into→vma_alloc); present = LOOM_OP_WRITE zero new
opcodes; the trusted-path medium-binding soundly reconciles ARCH §17.2.

### P1 (4) — all CLOSED

- **F1 [P1] — DMA admission weakened I-5 from structural to behavioral.**
  Fixed: §18.1 now admits only a *device-passive DMA weave subtype* (pixels
  only, no device-interpreted structures), so a device-command DMA ring stays
  structurally unshareable like MMIO — I-5 stays STRUCTURAL. KObj_DMA
  second-refcount + virtio-gpu-resource detach folded into the G-2 T-1
  spec/audit (NOT asserted type-independent). §18.11 F1 + inline §18.1.
- **F2 [P1] — /dev/tapestry cross-client isolation unspecified.** Fixed:
  §18.11 F2 binds per-session surface scoping (netd-`/net`-style qid encoding;
  a client resolves ONLY its own surfaces; `surface/new` mints in-session). The
  V2 fid-capability rests on this; scripture, lands with G-3.
- **F3 [P1] — Weft consume-once can't do arm/re-arm/disarm.** Fixed: (a)
  re-map uses a FRESH weave fid per generation (§18.3 corrected — SYS_WEFT_MAP
  is idempotent-per-fid); (b) retire runs a state-aware weft_share_unregister
  atomically with RETIRING. Corrects §18.0's "not new machinery"; scoped into
  G-2. §18.11 F3 + inline §18.0/§18.3.
- **F4 [P1] — tapestryd crash/restart contract absent.** Fixed: §18.11 F4 binds
  the "compositor gone" fid error + client re-attach/re-create (netd reconnect
  shape); #847 keeps client pages alive (no UAF) with the resource dead. SPEC:
  added the `ServerDeath` action (all live/woven gens → retiring, armed cleared,
  mapping stays) — clean invariants hold across it + EventuallyRetired verified
  (the #847-holds-across-crash machine-check).

### P2 (6) — all CLOSED (§18.11 F5–F10)

- F5 event never-drop set (CONFIGURE/CLOSE/FOCUS/key never dropped; FRAME +
  ptr-motion coalesce) + bounded per-surface buffer + present-CQE CQ headroom
  (or split rings).
- F6 ≤2 live weave generations — one reweave in flight per surface; a burst
  queues (inline §18.3 step 4 + spec header note; makes Gens={g1,g2} faithful).
- F7 graphical login on virtio-gpu media is NOT a trusted episode (passphrase
  transits untrusted tapestryd; corvus auth on serial) — TRUSTED-PATH-§8-style.
- F8 the cons drain/feed grant gate named (warden-bound renderer via a
  SPAWN_PERM_CONSOLE_OWNER-class perm; single-holder; 2nd opener refused).
- F9 per-client surface-count + weave-dimension caps at `create`; DMA-pool
  aggregate = the compositor's I-32/#65 composition point.
- F10 the framebuffer map path is its own branch (skip weft_binding_alloc's
  pages!=NULL + the Tweftio ring-view; report geometry from KObj_DMA).

### P3 (6) — all CLOSED (§18.11 F11–F16)

- F11 full-32-bit CONFIGURE serial rides tevent `value`; dims via `geometry`.
- F12 non-I/O-coherent-backend cache-clean obligation (stated now, Lazarus).
- F13 TPRESENT_HOLD `release` = a ctl verb; most-recent-held-per-surface.
- F14 G-0 screendump fallback (egl-headless/vnc, or pull in-band snapshot fwd).
- F15 G-arc edges (crash-probe synthetic re-home; virtio-input handoff; the
  Loom-5+6 precondition edge before G-3; test-mode drains in-flight FRAME).
- F16 spec fidelity (switch-at-Complete reconciled with §18.3; lifetime-vs-
  content header note; G-2 SPEC-TO-CODE binds Map's guard to the claim-vs-retire
  gate).

### Deferred to impl (no design change owed)

None dropped. F1's KObj_DMA/resource-detach lifetime + F3's unregister +
F10's map branch are IMPL work SCOPED into G-2 (named, not dropped). The G-2
SPEC-TO-CODE map owes: Map's guard → the atomic claim-vs-retire site (F3/F16).

---

## Round 2 — 2026-07-17 (Fable-max holotype re-audit of the round-1 fixes)

The dirty-close re-audit (round-1 was dirty: 4 P1 + 6 P2 ≥ 6). Prosecutor:
holotype-reviewer, MODEL(start)==MODEL(end)==Fable 5 — no fallback. Scope: the
round-1 amendment commit b7322841 (the fixes THEMSELVES). Self-audit ran in
parallel (R2-SA-a..f); a/b/c/d matched the reviewer's R2-F1/F5/F2/F4.

**Counts: 0 P0 / 0 P1 / 4 P2 / 2 P3.** NOT a clean round-2 (4 P2), but NOT a
dirty close (P1+P2 = 4 < 6, no P0/P1, no load-bearing-mechanism restructure — the
spec change is an ADDITION). All closed as binding amendments (§18.12 + inline
§18.1/§18.3 + the spec dual-refcount). **No round-3 mandated.** Amendment
commit: <pending>. Convergence: round-1 4 P1+6 P2 → round-2 0 P1+4 P2 → these
close without re-opening a P1 (design is converging; the residue is impl-scoped).

- **R2-F1 [P2]** — the device-passive DMA subtype was still creator-asserted
  (not kernel-verifiable) + is an ABI add. Corrected §18.1: structural ONLY via
  a kernel-minted+bound weave-backing type (handed only to RESOURCE_ATTACH_BACKING),
  ELSE it rests on tapestryd's existing sole-GPU-owner trust (not MMIO-parity).
  **The t_dma_create type/flag is an ABI addition → USER SIGNOFF at G-2**
  (escalation-worthy; flagged, not silently bound).
- **R2-F2 [P2]** — the round-1 ServerDeath spec action was EMPIRICALLY VACUOUS
  (reviewer ran TLC both ways: 2840==2840; no invariant read `armed`; the #847
  dual refcount was collapsed into one `backed` bool). FIXED: modeled the dual
  refcount explicitly — `serverRef` (handle_count) distinct from `mapped`
  (mapping_count); `ServerRelease` (graceful, post-quiesce) vs `ServerDeath`
  (crash, at-once even mid-transfer); new `RefImpliesBacked` invariant (either
  ref ⇒ backed) = the #847-across-crash no-UAF check. TLC: clean GREEN **5413
  distinct (up from 2840 — the jump IS the non-vacuity proof)**; liveness GREEN
  incl. EventuallyRetired across the crash; 4 buggy cfgs still fire.
- **R2-F3 [P2]** — the crash contract designed-in an uncharged pinned-DMA leak
  (burrow_share_into bumps mapping_count only, never #65 page_count). Bound: a
  kernel force-reclaim of orphaned weave mappings after a bounded grace (the
  client that ignores "compositor gone" takes a snare:segv) + charge to a
  per-client shared-mapping budget (the I-32/#65 axis F9 opens). G-2/G-3 impl.
- **R2-F4 [P2]** — the never-drop set + bounded buffer had no overflow
  disposition (a stalled client → compositor DoS). Bound: separate present/event
  rings (resolves the §18.11-F5 A/B) + a WEDGED-client force-retire when the
  non-coalescible class fills the buffer (never-drop holds for a live client,
  terminates a dead one).
- **R2-F5 [P3]** — F3's "atomic with RETIRING" mislocated the ordering + the
  reweave-open was unordered. Sharpened: the real obligations are
  registry-removal-before-page-free + claim-join-re-checks-registry (the G-2
  SPEC-TO-CODE binding of Map's guard); §18.3 step 2 now orders the resize-ack
  Rwrite after g2 alloc.
- **R2-F6 [P3]** — reusing SPAWN_PERM_CONSOLE_OWNER's shape muddied I-27.
  Sharpened: a distinct SPAWN_PERM_CONSOLE_RENDERER, orthogonal to BOTH
  console-ATTACH and console-OWNER (a three-role clarity).

**Round-2 verified-sound**: fresh-fid + separate-map-branch compose; F2 scoping
composes with F3/F4 (re-attach = new session); ≤2-gen bound faithful; no
resolution contradicts; inline edits consistent with their §18.11/§18.12
entries; the 4 buggy cfgs survive the dual-refcount addition.

### Round-2 escalation flag (owed to the user)

**R2-F1's t_dma_create ABI addition needs signoff at G-2** — a new BURROW_TYPE /
create flag / allocation syscall for the kernel-minted weave-backing type. Not
decided here; recorded as a G-2 gate. Until it lands, the DMA-share guarantee is
tapestryd-trust-based (honest fallback stated in §18.1).
