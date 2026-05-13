# Handoff 038 — Phase 4 close

**Tip**: `4f90e62` (P4-M hash fixup) on `main`. **Phase 4 is CLOSED.**

This handoff marks the Phase 4 boundary. All invariant-bearing and substrate-test §6.2 exit criteria are ticked. The two remaining boxes (P4-Id virtio-net `/dev/ether0` 9P surface + virtio-input `/dev/cons` 9P surface) are user-visible cross-process plumbing that requires the kernel 9P client + "userspace driver as 9P server" primitive — both Phase 5 deliverables. **Re-scoped to Phase 5 with explicit pickup conditions preserved**, not abandoned.

Phase 5 entry is unblocked. Stratum v2 is feature-complete and shipping; the integration target is concrete (its OS-integration manual at `stratum/v2/docs/OS-INTEGRATION.md`).

---

## What Phase 4 delivered

Phase 4 lifted the kernel from "userspace runs" (Phase 3 close) to **"hardware is reachable from userspace via the composed hw-handle SVC surface"**. The thylacine driver model is proven across four device classes.

### Cumulative summary

- **Dev vtable + Spoor lifecycle infrastructure** (P4-A). 16-op vtable per ARCH §9.2; reset/init/shutdown/attach/walk/stat/open/create/close/read/bread/write/bwrite/remove/wstat/power. Plan 9 idiom verbatim.
- **Kernel-internal synthetic Devs** (P4-B through P4-E). `/dev/cons`, `/dev/null`, `/dev/zero`, `/dev/random` (ARM64 RNDR), `/proc/<pid>/`, `/ctl/{procs,memory,devices,kernel-base,sched}`, `/ramfs` (cpio-loaded). 8 devices in the bestiary.
- **VirtIO core** (P4-F + P4-H). In-kernel transport for MMIO + minimal PCIe; split virtqueue (modern dialect, Version=2).
- **IRQ forwarding** (P4-G). `KObj_IRQ` blocker wakeups via Rendez; collapses multiple device IRQs into a count; spec-pinned at I-9 NoMissedWakeup via `scheduler.tla`.
- **C + Rust userspace runtimes** (P4-Ia1 + P4-Ia2). libt (C) + libthyla-rs (Rust); both produce ELFs with identical PT_LOAD layout under the W^X linker script.
- **Hardware handle integration** (P4-Ib through P4-Ic5b1b). `KObj_MMIO` + `KObj_IRQ` + `KObj_DMA` with capability gating (`CAP_HW_CREATE`), `BURROW_TYPE_MMIO` + `BURROW_TYPE_DMA`, SYS_MMIO_MAP + SYS_DMA_MAP for user-VA installation. R9 + R10 + R11 + R12-DMA + R13-burrow all closed clean.
- **`rfork_with_caps`** (P4-Ic3). Kernel-internal capability grant primitive; AND-with-parent enforces I-2 monotonicity; `handles.tla` spec-pinned via `RforkWithCaps` action + `BuggyRforkElevate` counterexample.
- **First composed userspace driver** (P4-Ic5b2). `usr/virtio-blk-probe/` reads sector 0 from QEMU's virtio-blk-device via the full composed hw-handle surface. First binary to compose MMIO + IRQ + DMA + Burrow + rfork-with-caps in one process.
- **Multi-sector virtio-blk r/w** (P4-Ic7). 2 GiB stress; 3 GiB I/O bit-exact. Closes ROADMAP §6.2's virtio-blk exit criterion.
- **virtio-net + virtio-input + virtio-gpu drivers** (P4-Ja → P4-Jc; P4-K + P4-K-events; P4-L + P4-L-scanout). Three more composed drivers; substrate generalizes across the VirtIO device-class space.
- **IRQ-to-userspace latency benchmark** (P4-Ic-latency). Infrastructure for the VISION §4.5 latency budget. QEMU TCG p99 ≈ 7-10 ms (emulation overhead dominates); bare-metal target p99 < 5 µs deferred to Phase 8 hardware bring-up.
- **R12-* deferred audit closures** (R12-bss-2mib + R12-gic-edge + R12-DMA + R12-FP + R12-vaddr + R12-uaccess + R13-burrow). Every audit-trigger surface introduced during Phase 4 formally prosecuted with no P0/P1 findings left open.
- **Cumulative driver-model audit** (P4-Z / R14). 1 P1 (VIRTIO 1.2 §2.7.13.2 LoadLoad barrier) + 3 P2 + 6 P3 surfaced; 10 closed in-commit, 2 deferred. Verdict MET (0 P0/P1/P2 after close).
- **Driver crash recovery runtime verification** (P4-M). `userspace.driver_crash_recovery` test exercises the spawn-A → A-exits → spawn-B-on-same-hardware path; verifies the release path (`proc_exit` → `kobj_*_unref`) empties the MMIO + INTID claim tables.

### Test count at close

244/244 PASS × default + UBSan. Boot time ~9 s (P4-M's two sequential virtio-blk-probe spawns dominate; BOOT_TIMEOUT bumped 10 → 15 s).

### Spec count at close

4 / 9 of the mandatory specs landed: `scheduler.tla`, `territory.tla`, `handles.tla`, `burrow.tla`. Remaining 5 are Phase 5+ work: `9p_client.tla` (Phase 5 gating spec), `poll.tla`, `futex.tla`, `notes.tla`, `pty.tla`.

### Reference docs at close

`docs/reference/00-overview.md` through `docs/reference/43-virtio-gpu.md`. 44 reference docs. Every audit-bearing surface is documented with file:line citations + the "Known caveats / footguns" enumeration.

---

## What Phase 4 deferred to Phase 5

### Re-scoped §6.2 boxes (not abandoned)

| Box | Why re-scoped | Phase 5 dependency |
|---|---|---|
| **P4-Id (virtio-net `/dev/ether0` 9P surface)** | The userspace driver consumes ARP packets end-to-end (P4-Jc). The `/dev/ether0` 9P surface requires bridging the driver's RX-drain to OTHER processes via a 9P file-tree. That's "userspace driver as 9P server" — the symmetric primitive to mounting a 9P FS. | Lands as a side effect of Phase 5's kernel 9P client + attach path. |
| **virtio-input `/dev/cons` 9P surface** | Same shape. Driver consumes events (P4-K-events); needs the same primitive to expose them. | Same. |

Tracker: `memory/project_phase4_open_boxes.md`.

### Trip hazards carried forward

1. **F225 (QueueReady-before-populate ordering)** — virtio-net-loop + virtio-input set `QUEUE_READY=1` BEFORE populating descriptors. VIRTIO 1.2 §3.1.1 step 8 says the device defers until DRIVER_OK regardless, so QEMU honors it. A future port to a stricter device would benefit from inverting. Deferred; cosmetic.
2. **R12-bss-2mib (kernel.ld assert tightening)** — current `_image_size < 0x200000` assert doesn't account for firmware offset. 792 KiB headroom today (P4-image-shrink reclaimed it); not urgent. Phase 5+ cleanup.
3. **F202 + F203 (GIC ICFGR SMP-RMW + stale-ICPENDR on re-claim)** — both v1.0-not-reachable per current discipline; revisit when SMP-driver-restart paths land in Phase 5+.
4. **F149 + F150 (per-CPU SGI/PPI semantics + ReduceCaps drop-precondition)** — both forward-looking. Revisit when SMP-aware drivers + cap-drop syscall land in Phase 5+ or Phase 6.

### Auto-restart supervision (P4-M tail)

Driver auto-restart on crash (the `/ctl/proc-events/exit` hook + supervisor process originally hinted by ROADMAP §6.2's "supervisor restarts" wording) is a Phase 5+ POLICY concern. Phase 4 proved the SUBSTRATE: the release path empties the kernel state; a new driver can re-claim. Designing the policy (auto-restart vs. escalate vs. panic) is a separate architectural decision.

---

## Stratum v2 readiness — what changed

The single largest contextual update at Phase 4 close: **Stratum v2 is feature-complete and shipping** (as of 2026 Q2). The scripture amendment landing alongside this handoff updates ARCH + ROADMAP + VISION + NOVEL + CLAUDE.md to reflect this.

Concretely:
- Stratum exposes three concurrent ABIs (9P2000.L wire — recommended; `libstratum-9p` C ABI; `libstm_fs` in-process UNSTABLE). Thylacine binds to the 9P wire surface.
- `stratumd` is a userspace daemon, one process per pool, bound to a Unix socket. Two sockets per daemon: FS (kernel mounts) + `/ctl/` (admin synthetic 9P FS).
- POSIX surface is comprehensive: inodes, dirents, xattrs, file seals, advisory locks, statx, name_to_handle_at, copy_file_range, single-dataset reflink, rename family, fallocate family, symlinks + hard links, O_TMPFILE, posix_fadvise, inline-data optimization, snapshots + atomic rollback.
- PQ encryption mandatory: ML-KEM-768 + XChaCha20-Poly1305 hybrid wrap keys. `.key` sidecar is the separate-factor security boundary.
- 9P extensions: `Tsync`, `Treflink` (single-dataset only at v2.x), `Tbind` / `Tunbind`, xattr family.

The canonical reference is `stratum/v2/docs/OS-INTEGRATION.md` (724 lines). Phase 5 entry reads it cover-to-cover.

---

## Phase 5 plan summary

(Full plan in `docs/phase5-status.md`; binding plan in `ROADMAP.md §7`.)

1. **P5-spec** — write `specs/9p_client.tla` first. Invariants: I-10 tag uniqueness, I-11 fid lifetime, out-of-order completion, flow control. 4+ buggy `.cfg` variants.
2. **P5-wire** — `kernel/9p_wire.c` codec for 9P2000.L baseline + Stratum extensions.
3. **P5-session** — `kernel/9p_session.c` state machine (tag pool, fid table, outstanding-requests).
4. **P5-transport** — `kernel/9p_transport.c` (Spoor → Unix socket).
5. **P5-attach** — `kernel/9p_attach.c` (mount syscall integration; per-Proc connection lifecycle).
6. **P5-key** — janus integration; `.key` sidecar handling.
7. **P5-stratumd** — `stratumd` lifecycle in initramfs; ramfs → Stratum pivot.
8. **P5-ctl** — `/srv/stratum-ctl/` consumption.
9. **P5-id** — virtio-net `/dev/ether0` 9P surface (= re-scoped P4-Id).
10. **P5-cons** — virtio-input `/dev/cons` 9P surface (= re-scoped P4-K-events tail).
11. **P5-snapshot-upgrade** — atomic-snapshot upgrade smoke test (the strongest exit signal).
12. **P5-audit** — cumulative 9P-client audit (R-series prosecutor).

---

## Sanity snapshot at this handoff

- **Tip**: `4f90e62` on `main`.
- **Tests**: 244/244 PASS × default (~9 s boot) + UBSan.
- **Spec**: 4 of 9 landed (scheduler / territory / handles / burrow).
- **Audit closed lists**: R1..R14 cumulative; latest at `memory/audit_r14_p4z_closed_list.md`.
- **Phase 4 chunks**: A → H, Fix157, Ia1, Ia2, Ib, Ic1, Ic2, Ic3, Ic4, Ic5a, Ic5-IRQ-probe, Ic5b1a, Ic5-FP, Ic5b1b, Ic5b2, Ic6-{spec,cfg,impl}, Ic7, Ja, Jb, Jc, Ic-latency, K, K-events, L, L-scanout, image-shrink, N, O, Z, M. Plus the R12-* and R13 deferred-audit closures.
- **Working tree**: clean; 110 commits ahead of `origin/main` (will grow by the scripture-amendment commit landing alongside this handoff).
- **Scripture posture**: CLAUDE.md + VISION + NOVEL + ROADMAP + ARCH all refreshed for Stratum v2 readiness as of this handoff's substantive commit.

---

## What the next session needs to know

1. **Read first**: `CLAUDE.md` → this handoff → `docs/phase5-status.md` → `docs/ROADMAP.md §7` → `stratum/v2/docs/OS-INTEGRATION.md`.
2. **Phase 5 is OPEN; entry chunk is P5-spec** (`specs/9p_client.tla`). Spec-first is non-negotiable per CLAUDE.md.
3. **Stratum is ready**. No external dependency. The integration manual is the canonical guide.
4. **The phase-numbering reconciliation** in ROADMAP §2.1 explains why ROADMAP's "Phase 4" section is actually our Phase 5; the renumber is a future doc churn pass.
5. **Re-scoped §6.2 boxes** are tracked in `memory/project_phase4_open_boxes.md`; they close as natural side effects of Phase 5 deliverables.
6. **Composition-layer discipline** from R14 (VIRTIO 1.2 §2.7.13.2 LoadLoad barrier via `libthyla_rs::virtio_rmb()`) applies to any future VirtIO-class driver. Documented at `docs/reference/39-hw-handles.md` caveat #11.
7. **The thylacine substrate**: this Phase has produced a real production-grade ARM64 OS substrate — userspace drivers running on top of a hw-handle SVC surface with audited release paths, formally specified concurrency, and composition-layer memory-ordering discipline. The next phase makes the storage layer real.

---

**The thylacine runs again** — at Phase 5 entry, on real persistent storage.
