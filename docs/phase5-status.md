# Phase 5 — status and pickup guide

Authoritative pickup guide for **Phase 5: 9P client + Stratum integration** (per `ROADMAP.md §7`; header label "Phase 4" is stale by one — see ROADMAP §2.1 for the reconciliation table).

## TL;DR

**Phase 5 is OPEN.** Entry unblocked at Phase 4 close (tip `4f90e62`, P4-M hash fixup). Stratum v2 is feature-complete and shipping; integration binds to its stable 9P2000.L wire + libstratum-9p ABIs per `stratum/v2/docs/OS-INTEGRATION.md`. No external delivery dependency remains.

Phase 5 lifts the kernel from "drivers expose hardware as composed kobjects" (Phase 4 close) to **"the filesystem is the OS"** — userspace processes interact with persistent storage, the admin surface, and each other entirely through 9P. Major deliverables: the kernel 9P client (`kernel/9p_*`), `specs/9p_client.tla` (the gating spec, written before impl), Unix-socket transport on Spoor, mount path integration, per-Proc 9P connection lifecycle, `.key` sidecar handling via janus, `stratumd` lifecycle inside Thylacine's userspace-driver-as-9P-server pattern, `/srv/stratum-ctl/` consumption, ramfs → Stratum boot pivot. The two §6.2 boxes re-scoped from Phase 4 close as natural side effects: virtio-net `/dev/ether0` 9P surface (P4-Id) + virtio-input `/dev/cons` 9P surface — both depend on the same "userspace driver as 9P server" primitive Phase 5 lands.

Per CLAUDE.md spec-first policy: **`specs/9p_client.tla` is mandatory and lands first.** Invariants in scope: I-10 (per-session tag uniqueness), I-11 (per-session fid identity stable across open lifetime), out-of-order completion correctness, flow control / back-pressure. At least 4 buggy `.cfg` variants demonstrating each invariant's violation under buggy assumptions.

The phase culminates with Thylacine booting from a real Stratum pool: `stratumd` starts in the initramfs, the `.key` sidecar is unwrapped via janus, the kernel mounts the FS socket at `/sysroot`, init pivots root, the system runs entirely on Stratum-backed storage with the `/srv/stratum-ctl/` admin surface live for snapshot / scrub / properties / metrics.

---

## Phase entry pickup (read order)

1. `CLAUDE.md` (root) — operational framework. Refreshed at Phase 5 entry: the "Stratum coordination" section now reflects Stratum v2 reality.
2. `docs/VISION.md §11` — Relationship to Stratum (refreshed for v2).
3. `docs/ARCHITECTURE.md §10.2` + `§14` — 9P dialect + Stratum integration (refreshed for v2).
4. `docs/ROADMAP.md §7` — this phase's binding plan.
5. **`stratum/v2/docs/OS-INTEGRATION.md`** — Stratum's canonical OS-integration manual. Required reading for anyone implementing the 9P client or the boot path. Read §3 (Choosing an integration mode), §4 (Boot lifecycle), §5 (Encryption + key delivery), §6 (POSIX surface inventory), §7 (`/ctl/` admin), §13 (ABI stability), §18 (common pitfalls), §19 (Testing).
6. `stratum/v2/docs/reference/20-9p.md` — as-built 9P wire semantics.
7. `stratum/v2/docs/reference/22-ctl.md` — `/ctl/` admin surface trust boundary.
8. `docs/phase4-status.md` + `docs/handoffs/038-p4-close.md` — what Phase 4 delivered + what was re-scoped to Phase 5.
9. `memory/project_phase4_open_boxes.md` — the 2 re-scoped §6.2 boxes with explicit pickup conditions.

---

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| *(pending)* | **P5-wire: hash fixup** | (doc-only) |
| *(pending)* | **P5-wire: `kernel/9p_wire.c` — 9P2000.L wire codec (bring-up subset).** The lowest layer of the kernel 9P client stack; pure byte-level marshal/unmarshal with no kernel state, no allocation, no I/O. **Subset covered**: header peek; pack/unpack primitives (u8 / u16 / u32 / u64 / 9P-string / qid); Tversion/Rversion (handshake); Tattach/Rattach (root fid bind); Twalk/Rwalk (fid navigation + clone); Tclunk/Rclunk (fid release); Rlerror parse (server error response). **Out of scope for this chunk** (deferred to P5-wire-io / P5-wire-meta / P5-wire-mutation / P5-wire-lock / P5-wire-xattr / P5-wire-stratum-ext): Tlopen / Tread / Twrite / Tlcreate; Tgetattr / Tsetattr / Treaddir / Tstatfs / Tfsync; mutation family (Tunlinkat / Trename / Trenameat / Tsymlink / Tmknod / Tlink / Treadlink / Tmkdir); Tlock / Tgetlock; xattr family; Stratum extensions (Tsync / Treflink / Tbind / Tunbind / Tfallocate / Tfadvise). **Discipline**: every parser enforces `header.size == frame_length` AND `body offset == frame_length` at the end (strict body-length equality per Stratum's R111 P3 F-10 doctrine); R111 caller-cap-bound applied at `p9_parse_rwalk` (`nwqid > qid_cap` rejected BEFORE any qid is unpacked into the caller buffer). **Compile-time invariants**: 8 `_Static_assert` lines pin opcode constants + header/qid lengths + sentinel values + struct-shape minimum size. **Error convention**: pack/unpack/build return byte count or negative; parse returns 0 or negative. Coarse-grained errors (buffer-too-small / malformed / wrong-type); no errno-style channel. **Audit posture**: foundation layer; the `kernel/9p_*` surface joins CLAUDE.md trigger list when P5-session lands. Bugs here can break I-10 (tag mis-decoded) and I-11 (newfid corrupted) — guarded by the unit-test matrix. **15 new unit tests** in `kernel/test/test_9p_wire.c`: 7 round-trips (primitives × 4, string, qid, header, each Tmsg+Rmsg pair) + 8 rejection paths (overflow, underflow, size-mismatch, wrong-type, oversize-string, caller-cap-bound violation). Files: `kernel/include/thylacine/9p_wire.h` (~225 LOC), `kernel/9p_wire.c` (~370 LOC), `kernel/test/test_9p_wire.c` (~325 LOC), `kernel/test/test.c` (forward decls + 15 registry entries), `kernel/CMakeLists.txt` (kernel-source list + test-source list), `docs/reference/44-9p-wire.md` (new; ~200 LOC). Test count 244 → 259. 259/259 PASS × default + UBSan. Non-audit-bearing at this layer (pure codec, no state); audit-bearing when composed with P5-session in the next chunk. Predecessor: P5-spec. | 259/259 PASS × default + UBSan. |
| `f1aadfc` | **P5-spec: hash fixup** | (doc-only) |
| `ce4fa31` | **P5-spec: `specs/9p_client.tla` — Phase 5 entry, spec-first per CLAUDE.md.** Pins ARCH §28 I-10 (per-session tag uniqueness) + I-11 (fid identity stable across open lifetime) + two composition-layer properties from ROADMAP §7 / `stratum/v2/docs/OS-INTEGRATION.md` §3: OutOfOrderCorrectness (Rmessages match Tmessages by tag, not by arrival order) + FlowControl (outstanding-request count bounded; back-pressure surfaces as Send-side blocking, never as silent drop). **4 invariants** (`TypeOk`, `RootFidImmutable`, `TagAndOpAccounting`, `FidStability`, `BoundedOutstanding`). **4 buggy cfg variants** exercise each invariant's violation surface: `tag_collision` (alloc_tag returns in-use tag), `fid_after_clunk` (IO on unbound fid), `ooo_match` (Rmsg paired with wrong tag's op), `unbounded` (Send past MaxWindow). **TLC posture**: correct cfg clean (462 generated / 197 distinct / depth 9); each buggy cfg produces expected counterexample in ≤ 6 steps (50-68 generated / 42-52 distinct / depth 5-6). **Modeling decisions**: one session modeled (cross-session is Stratum's `fid.tla` + `namespace.tla` server-side concern); op kinds collapsed to `{walk, clunk, io}` (full 9P2000.L baseline + Stratum extensions all collapse into `io` for spec purposes); `SendClunk` requires no other in-flight op on the same fid (canonical client discipline); per-op `op_id` (monotonic) lets invariants distinguish ops across tag-slot reuse. Bounded model: `TagIds = {t1, t2, t3}`, `FidIds = {root, f1}`, `MaxWindow = 2`, `MaxOps = 3` — small state space, every bug class reaches violation in ≤ 6 steps. **State machine cross-references** in `specs/SPEC-TO-CODE.md` map each spec action to its planned Phase 5 impl symbol (`kernel/9p_attach.c::session_attach`, `kernel/9p_session.c::session_send_io` / `_walk` / `_clunk` / `dispatch_rmsg`). **No impl yet** — Phase 5 entry chunk, gates P5-wire (codec) + P5-session (state machine) + P5-transport (Spoor-over-Unix-socket) + P5-attach (mount syscall) per CLAUDE.md spec-first policy. Files: `specs/9p_client.tla` (~395 LOC), `specs/9p_client.cfg` + 4 buggy cfgs (~15 LOC each), `specs/SPEC-TO-CODE.md` (new 9p_client section ~85 LOC; replaces the prior "Phase 4 (planned)" stub). 244/244 PASS × default + UBSan unchanged (spec-only commit). Audit posture: spec-bearing per CLAUDE.md §"Spec-first policy"; the 9p_client surface joins the audit-trigger surface list once P5-wire/P5-session impl lands. | 244/244 PASS × default + UBSan (spec-only). 5 TLC verdicts: 1 clean (correct) + 4 expected-violation (buggy). |

---

## Planned chunk sequence (proposed)

This is a working plan, refined as chunks land. The spec-first chunks come first; everything else builds on them.

### P5-spec — `specs/9p_client.tla` (mandatory; spec-first per CLAUDE.md)

State model: per-session tag pool, fid table, outstanding-request set, transport queue. Actions: `Tversion` / `Rversion`, `Tattach` / `Rattach`, `Twalk` / `Rwalk`, `Tlopen` / `Rlopen`, `Tread` / `Rread` (with offset + count), `Tclunk` / `Rclunk`, plus the Stratum extensions per ARCH §10.2. Invariants:

- **I-10 TagUniqueness**: at all times, no two outstanding `Tmessage`s on the same session share a tag.
- **I-11 FidStability**: a fid's identity (qid binding) is stable from `Tattach` / `Twalk` until `Tclunk`.
- **OutOfOrderCorrectness**: an `Rmessage` arriving after a later-issued `Rmessage` (out-of-order completion) is matched by tag, not by arrival order. The pipelined client must not assume FIFO.
- **FlowControl**: outstanding-request count is bounded by the negotiated `msize` / per-session window; back-pressure surfaces as `Tsubmit` blocking, not as silent drops.

At least 4 buggy `.cfg` variants: `tag_collision`, `fid_after_clunk`, `out_of_order_wrong_match`, `unbounded_outstanding`.

### P5-wire — `kernel/9p_wire.c` (codec)

9P2000.L baseline message framing + marshal/unmarshal. Each message gets a `_Static_assert` on layout. Round-trip tests against a Stratum reference message corpus (Stratum's own test suite includes message fixtures we can consume).

### P5-session — `kernel/9p_session.c` (state machine)

Tag pool (per-session, monotonic generation per CLAUDE.md handle-discipline pattern). Fid table (per-session, with magic clobber on clunk mirroring KObj_* discipline). Outstanding-request table. Send / receive loops. Spec cross-references at each state transition.

### P5-transport — `kernel/9p_transport.c` (Spoor → Unix-socket)

Thylacine's "Unix socket" inside the guest VM is a Spoor that the kernel routes between the client process's address space and `stratumd`'s. Message framing at the byte level; `msize` negotiation; the standard 8-MiB ceiling per OS-INTEGRATION.md §14.

### P5-attach — `kernel/9p_attach.c` (mount syscall integration)

`mount(fd, path, "subvol_name")` → `Tattach` with `subvol_name` as `aname`. Per-Proc connection setup at `rfork`. Cleanup at `exits`.

### P5-key — janus integration

`.key` sidecar handling in libthyla-rs (or a kernel-side janus client; design pass needed). `mlock(2)` + `MADV_DONTDUMP` + `explicit_bzero` discipline. Argon2id is the v1.0 default; TPM-sealed at v1.x.

### P5-stratumd — `stratumd` lifecycle in initramfs

Initramfs holds `stratumd`, `janus`, `init`, and the wrapped `.key`. init prompts → janus unwraps → init forks `stratumd` → init waits for FS socket bind → init mounts `/sysroot` over 9P → init pivots root. Post-pivot: `/srv/stratum-ctl/` mounted from the `/ctl/` socket.

### P5-ctl — `/srv/stratum-ctl/` consumption

Userspace tools dial the `/ctl/` socket. The first integration is `cat /srv/stratum-ctl/version` (sanity); the second is `echo start > /srv/stratum-ctl/pools/<uuid>/scrub-trigger` + monitoring via `/srv/stratum-ctl/pools/<uuid>/scrub`.

### P5-id — virtio-net `/dev/ether0` 9P surface (re-scoped from P4-Id)

Once the "userspace driver as 9P server" primitive is in place (likely as a side effect of P5-attach + P5-transport), virtio-net-loop's RX-drain feeds into a 9P file-tree backing `/dev/ether0`. Other processes `open` / `read` / `write` it for raw Ethernet frames.

### P5-cons — virtio-input `/dev/cons` 9P surface (re-scoped from P4-K-events tail)

Same shape as P5-id but for the keyboard event stream. virtio-input's eventq drain feeds into a 9P file at `/dev/cons` that other processes (a shell, a textual editor) `read` to consume input events.

### P5-snapshot-upgrade — atomic-snapshot upgrade smoke test

Per OS-INTEGRATION.md §8: snapshot root dataset → break userspace → reboot → rollback verb → reboot succeeds. This is the strongest exit signal for Phase 5: a real atomic-system-upgrade demonstration.

### P5-audit — cumulative 9P client audit (R-series prosecutor)

The 9P client is an audit-bearing surface per CLAUDE.md trigger surfaces. After all P5-* chunks land, spawn the prosecutor scoped to `kernel/9p_*` + `kernel/9p_session.c`'s tag / fid management + boot-pivot ordering. Adversarial categories: wire-protocol malformations, tag collision under retry, fid lifetime UAF, out-of-order completion races, msize boundary, flow-control deadlocks, key-handling lifetime.

---

## §7.3 Exit criteria status

(All boxes ticked from the ROADMAP §7.3 list at Phase 5 close. Each gets a citation when ticked.)

- [ ] `specs/9p_client.tla` clean under TLC. ≥ 4 buggy `.cfg` variants exist.
- [ ] `stratumd` boots from initramfs against a real Stratum pool on virtio-blk.
- [ ] Kernel mounts the FS socket at `/`. Basic FS ops (`ls`, `cat`, `mkdir`, `echo > / cat <`) all work.
- [ ] janus unwraps a passphrase-protected pool key.
- [ ] **Reboot test**: data written before reboot persists; Stratum's three-phase sync verifies.
- [ ] **Pull-the-plug test**: SIGKILL stratumd mid-write; next mount succeeds + state consistent.
- [ ] 9P session: 10,000 open/read/write/close cycles without leak.
- [ ] Pipelined throughput: 32 concurrent reads achieve ≥ 90% session bandwidth.
- [ ] 9P round-trip latency (loopback Stratum): p99 < 500µs (VISION §4.5).
- [ ] 100 procs × 1 connection each works without leaks.
- [ ] Stratum extensions verified: `Tbind`, `Tsync`, `Treflink`, xattr round-trip.
- [ ] No P0/P1 audit findings on the 9P client.
- [ ] `/srv/stratum-ctl/` mounted; `version` + `scrub-trigger` + `events` all functional.
- [ ] Atomic-snapshot upgrade smoke test passes.
- [ ] Prometheus exposition reachable; `stratum_pool_*` metrics readable.
- [ ] POSIX surface from Stratum v2 (per OS-INTEGRATION.md §6) exercises correctly through Thylacine's 9P client.

---

## Trip hazards carried from Phase 4

(per `docs/handoffs/038-p4-close.md`):

1. F225 (QueueReady-before-populate ordering) — composition-layer cosmetic; ride alongside any P5 chunk that touches `usr/virtio-*` if convenient.
2. R12-bss-2mib (kernel.ld assert tightening) — 792 KiB headroom today; not urgent.
3. F202 + F203 (GIC ICFGR SMP-RMW + stale-ICPENDR on re-claim) — both v1.0-not-reachable; revisit when SMP-driver-restart paths land.
4. F149 + F150 (per-CPU SGI/PPI semantics + ReduceCaps drop-precondition) — both forward-looking; revisit when SMP-aware drivers + cap-drop syscall land.

---

## Build + verify commands

Once `kernel/9p_*` lands, the test runner needs a Stratum pool to test against. Recommended test fixture: a tiny pre-formatted Stratum pool image (`build/stratum-test-pool.img`) generated by `tools/mkstratumpool.sh` calling Stratum's `mkfs` CLI. `tools/run-vm.sh` attaches it as a second virtio-blk device.

```bash
# Build everything
tools/build.sh all

# Generate the Stratum test pool (one-time per Stratum version)
tools/mkstratumpool.sh

# Run tests (default)
tools/test.sh

# Run tests with the Stratum pool attached + boot pivot active
THYLACINE_STRATUM_POOL=build/stratum-test-pool.img tools/test.sh

# All specs (including the new 9p_client.tla)
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && for s in *.tla; do
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
    -config "${s%.tla}.cfg" "$s" 2>&1 | tail -3
done
```

---

## References

- `CLAUDE.md` — operational framework + Stratum coordination
- `docs/VISION.md §11` — Relationship to Stratum
- `docs/ARCHITECTURE.md §10.2 + §14` — 9P dialect + integration
- `docs/ROADMAP.md §7` — binding plan
- `docs/handoffs/038-p4-close.md` — Phase 4 close handoff
- `memory/project_phase4_open_boxes.md` — re-scoped §6.2 boxes
- `stratum/v2/docs/OS-INTEGRATION.md` — Stratum's OS-integration manual
- `stratum/v2/docs/reference/20-9p.md` — 9P wire semantics
- `stratum/v2/docs/reference/22-ctl.md` — admin surface
