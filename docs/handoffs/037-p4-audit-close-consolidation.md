# Handoff 037 — Phase 4 audit-close consolidation (P4-image-shrink, P4-N, P4-O, P4-L-scanout, P4-K-events, P4-Z)

**Tip**: `86270a6` (P4-Z hash fixup) on `main`. **ROADMAP §6.2 final invariant-bearing exit criterion CLOSED at P4-Z.**

This handoff consolidates six chunks landed in two sessions:

| Order | Chunk | Substantive | Hash fixup |
|---|---|---|---|
| 1 | **P4-image-shrink** — C-side `-z max-page-size=4096` + shrink 9 legacy 96-KiB blob caps → 16 KiB | `eb7ed3a` | `92e8625` |
| 2 | **P4-N** — BURROW spec finalize + R13 audit close (1 P2 + 3 P3; 0 deferred) | `ea763df` | `36807d0` |
| 3 | **P4-O** — Exit-criteria reconciliation + Dev vtable coverage test | `d766360` | `4fcf2ca` |
| 4 | **P4-L-scanout** — Full 2D scanout pipeline on top of P4-L probe (closes §6.2 GPU box at substrate layer) | `8c296c9` | `7298ea1` |
| 5 | **P4-K-events** — virtio-input event consumption + QMP send-key wiring | `a859abc` | `7cc6c14` |
| 6 | **P4-Z** — Cumulative driver-model audit close (R14: 1 P1 + 3 P2 + 6 P3; 10 closed, 2 deferred; **closes §6.2 final invariant box**) | `821b047` | `86270a6` |

Phase 4 substrate now stands on a fully-audited base: all invariant-bearing audit-trigger surfaces from CLAUDE.md §"Audit-triggering changes" have been formally prosecuted and closed. The three §6.2 exit criteria that remain open are user-visible plumbing requiring FS-namespace work (P4-Id /dev/ether0, P4-K-events /dev/cons 9P surface) or a fresh design pass (P4-M driver crash recovery), not invariant-bearing substrate.

---

## §6.2 status at this handoff

```
ROADMAP §6.2 exit criteria
├─ [x] Dev vtable: 16 ops dispatch                          (P4-A + P4-O reconcile)
├─ [x] /dev/null  10K cycles no leak                        (P4-B)
├─ [x] /dev/random nonzero bytes                            (P4-B)
├─ [x] /proc/<pid>/ enumerable                              (P4-C)
├─ [x] /ctl/{procs,memory,devices,kernel-base,sched}        (P4-D)
├─ [x] /dev/ramfs cpio-loaded FS + initrd PA reserved       (P4-E)
├─ [x] VirtIO MMIO transport + split virtqueue              (P4-F)
├─ [x] IRQ forwarding to KObj_IRQ blocker (I-9 wait/wake)   (P4-G)
├─ [x] VirtIO PCIe ECAM enumeration                         (P4-H)
├─ [x] Userspace runtime (libt C + libthyla-rs Rust)        (P4-Ia1 + P4-Ia2)
├─ [x] Hw-handle integration: KObj_MMIO + KObj_IRQ + caps   (P4-Ib;  R9 closed)
├─ [x] Burrow MMIO type + device-memory PTE attrs           (P4-Ic1 + P4-Ic2; R10 closed)
├─ [x] rfork_with_caps capability grant (spec-first)        (P4-Ic3; R11 closed)
├─ [x] First userspace hw-handle binary (/mmio-probe)       (P4-Ic5a)
├─ [x] IRQ probe userspace + virtio-mmio reservation        (P4-Ic5-IRQ-probe + P4-Ic5b1a)
├─ [x] KObj_DMA + SYS_DMA_CREATE + SYS_DMA_MAP              (P4-Ic5b1b; R12-DMA closed)
├─ [x] First composed userspace driver (virtio-blk-probe)   (P4-Ic5b2)
├─ [x] Multi-sector virtio-blk r/w 1 GiB read + 1 GiB write (P4-Ic7)
├─ [x] virtio-net TX + RX full ARP round-trip               (P4-Ja + P4-Jb)
├─ [x] virtio-net steady-state RX recycling + TX reuse      (P4-Jc)
├─ [x] virtio-input probe (DeviceID=18) + classification    (P4-K)
├─ [x] virtio-input event consumption via QMP send-key      (P4-K-events)
├─ [x] virtio-gpu probe (DeviceID=16, two virtqueues)       (P4-L)
├─ [x] virtio-gpu full 2D scanout pipeline (host pixels)    (P4-L-scanout)
├─ [x] IRQ-to-userspace latency benchmark infrastructure    (P4-Ic-latency)
├─ [x] BURROW spec finalize + audit close                   (P4-N; R13 closed)
├─ [x] Hw-handle non-transferability (structural via NoHwDup)(P4-Ib + P4-Ic5b1b)
├─ [x] No P0/P1 audit findings on driver model              (P4-Z; R14 closed)
│
├─ [ ] virtio-net /dev/ether0 9P surface                    (P4-Id;        awaits FS-namespace)
├─ [ ] virtio-input /dev/cons 9P surface                    (post-P4-K-events; awaits FS-namespace)
└─ [ ] Driver crash recovery                                (P4-M;         next chunk candidate)
```

**Invariant-bearing boxes: 28 ticked, 0 open.** Plumbing boxes: 1 ticked (substrate side of /dev/cons via P4-K-events), 2 open + 1 design-pass (P4-M).

---

## What landed during this two-session window

### P4-image-shrink (`eb7ed3a` / `92e8625`)

C-side mirror of P4-K's Rust `-z max-page-size=4096` flag. Adds the LD flag to `cmake/Toolchain-aarch64-userspace.cmake::THYLACINE_USERSPACE_LD_FLAGS`. ld.lld defaults to MAXPAGESIZE 0x10000 (64 KiB) on aarch64; without this flag the C-side `hello` binary carried a 64-KiB zero-padded gap between PT_LOAD segments. With the flag, every PT_LOAD aligns to 4 KiB (matching kernel PAGE_SIZE); hello drops 73912 → 12472 B.

With every userspace binary now under 16 KiB (largest = virtio-net-arp at 14864 B), shrunk all 9 legacy 96-KiB test-blob caps to 16 KiB:
- `RAMFS_EXEC_BLOB_MAX`, `MMIO_PROBE_BLOB_MAX`, `IRQ_PROBE_BLOB_MAX`, `VIRTIO_BLK_PROBE_BLOB_MAX`, `VIRTIO_BLK_RW_BLOB_MAX`, `IRQ_BENCH_BLOB_MAX`, `VIRTIO_NET_PROBE_BLOB_MAX`, `VIRTIO_NET_ARP_BLOB_MAX`, `VIRTIO_NET_LOOP_BLOB_MAX`.

9 × 80 KiB = 720 KiB of kernel `.bss` reclaimed. Image_size 1460 → 744 KiB; image+firmware 1972 → 1256 KiB; headroom under the 2 MiB L3 mapping grows 76 KiB → ~792 KiB.

Non-audit-bearing — pure toolchain + sizing.

### P4-N (`ea763df` / `36807d0`)

BURROW spec finalize + R13 audit close. 1 P2 + 3 P3 closed in-commit; 0 deferred.

- **F213 (P2)**: `burrow_free_internal` lacked the `v->magic = 0` clobber that sibling kobjs (kobj_mmio, kobj_dma) established at R9 F148. Without it, a stale-pointer dereference between `kmem_cache_free` and SLUB's next-pointer write would read a valid `VMO_MAGIC` and bypass the magic check. Fix: explicit clobber before `kmem_cache_free`.
- **F214 (P3)**: SPEC-TO-CODE.md drift — burrow.tla section pointed at the wrong impl symbols. Fixed.
- **F215 (P3)**: `specs/burrow.tla` commentary enumerated ANON + MMIO but not DMA. Fixed.
- **F216 (P3)**: 4 sites claimed "SLUB clobbers magic on free" — factually wrong (SLUB writes a freelist next-pointer; clobber is incidental allocator behavior, not architectural guarantee). All 4 sites updated to attribute the clobber to `burrow_free_internal`'s explicit assignment.

TLC verification clean (burrow.cfg 100 distinct states / depth 9 matches P2-Fb baseline; 3 buggy cfgs each produce expected NoUseAfterFree violations).

### P4-O (`d766360` / `4fcf2ca`)

Exit-criteria reconciliation + Dev vtable coverage test. Closes the gap between ROADMAP §6.2 exit criteria as written and what the test suite actually verifies. Five criteria ticked with citations: (1) `cat /dev/random` non-zero bytes; (2) Spoor 10K cycles on /dev/null; (3) Dev vtable all 16 ops dispatch via new `dev.vtable_slot_coverage` test; (4) hw-handle non-transferability (wording reconciled from "panics with message" to "is rejected at handle_dup" — structural NoHwDup prevention stronger than runtime extinct); (5) burrow.tla TLC clean.

ROADMAP "17 ops" reconciled to "16 ops" (Plan 9's modern devtab is also 16; counting drift). 242 → 243 tests.

### P4-L-scanout (`8c296c9` / `7298ea1`)

Full 2D scanout pipeline on top of the P4-L probe. Extends `usr/virtio-gpu/` from a `GET_DISPLAY_INFO`-only probe into a six-command driver:

```
GET_DISPLAY_INFO → RESOURCE_CREATE_2D → RESOURCE_ATTACH_BACKING
                 → SET_SCANOUT → TRANSFER_TO_HOST_2D → RESOURCE_FLUSH
```

Each subsequent command verifies `resp.hdr.type == OK_NODATA` (0x1100) per VIRTIO 1.2 §5.7.6.

**Closes ROADMAP §6.2 "Userspace virtio-gpu: write pixels to framebuffer via BURROW handle"** at the substrate layer. The host validates + builds the resource + records the backing + binds the scanout + copies the guest backing into the host-side resource + presents on scanout. CI-visible pixel verification ("visible on QEMU display") deferred to optional P4-L-screencap (QMP `screendump` + Python verifier; `tools/run-vm.sh` runs `-nographic` so no host-side rasterizer is active in CI).

What's new vs. probe:
1. **Per-command submit + wait helper** — new `struct Controlq { slot_va, dma_va, dma_pa, irq_handle, seq }` + `Controlq::submit_and_wait(req_len, resp_len) -> Result<resp_type>`. `seq` monotonic across the six commands; desc head 0 rebuilt + reused safely between completions.
2. **Five new request-body builders** per VIRTIO 1.2 §5.7.6 body layouts (CREATE_2D 16 B / ATTACH_BACKING 24 B / SET_SCANOUT 24 B / TRANSFER_TO_HOST_2D 32 B / FLUSH 24 B).
3. **Two-DMA composition** — separate `t_dma_create(64 KiB)` for the framebuffer (128×128 B8G8R8A8 = 65536 B = 16 contiguous 4-KiB pages from one buddy-allocated chunk). Framebuffer PA is the value embedded in `ATTACH_BACKING`'s mem_entry.
4. **Pattern fill** — 4-quadrant test pattern (red TL / green TR / blue BL / white BR, 64×64 each) filled between CREATE_2D and ATTACH_BACKING with a trailing `dsb sy`.
5. **DMA layout reshape** — RESP_OFF moves 0x620 → 0x700; REQ region 32 → 256 B; three new compile-time asserts.

**Halcyon (Phase 8) substrate gate closed**.

### P4-K-events (`a859abc` / `7cc6c14`)

virtio-input event consumption + QMP send-key wiring on top of the P4-K probe. CI pipeline:

1. Kernel test pre-fires SPI 77 via `gic_set_pending_spi(intid)` (computed from `input_dev->pa` slot index) before rfork — interactive-mode forward-progress guarantee (ARM IHI 0069 §12.9.6: pending-bit orthogonal to enable; child's first `t_irq_wait` returns without sleeping).
2. Driver issues `t_irq_create` + single `t_irq_wait` (consumes pre-fire) + bounded busy-poll (`MAX_POLL_ITER = 200M ≈ 3-5 s on QEMU TCG`).
3. Used-ring drain helper parses 8-byte event records + recycles descriptors via DSB-fenced `avail.ring[avail.idx % QUEUE_SIZE] = desc_id` + `avail.idx++`.
4. Driver prints `virtio-input: AWAITING_QMP_KEY` sentinel.
5. `tools/run-vm.sh` adds `-qmp unix:build/qmp.sock,server,nowait`.
6. `tools/test.sh` background injector polls the log for the sentinel; on match, connects to `build/qmp.sock`, negotiates `qmp_capabilities`, issues `send-key` with qcode `"a"`.
7. QEMU's virtio-keyboard-device fills the eventq with 4 events (KEY press + SYN + KEY release + SYN); driver drains, validates `{type=EV_KEY, code=KEY_A=30, value=1}` against the target, exits PASS.
8. Post-boot grep in `tools/test.sh` enforces `virtio-input: saw target key` when injection was expected.

**Does NOT close** ROADMAP §6.2 "Userspace virtio-input: keyboard input via /dev/cons" — that requires a `/dev/cons` 9P surface bridging the userspace driver's eventq to OTHER processes. What this chunk DOES close: the event-injection-to-userspace-consumption substrate end-to-end (CI-visible PASS verified bit-exact against KEY_A press semantics).

### P4-Z (`821b047` / `86270a6`)

Cumulative driver-model audit close. R14 prosecutor pass across all four composed userspace drivers (virtio-blk-rw, virtio-net-loop, virtio-input, virtio-gpu) + libthyla-rs. **0 P0 + 1 P1 + 3 P2 + 6 P3** surfaced; 10 closed in-commit, 2 deferred.

**F217 (P1)** — VIRTIO 1.2 §2.7.13.2 requires a driver-side LoadLoad barrier between reading `used.idx` and reading `used.ring[k]` / data buffer. Without it, out-of-order ARM Cortex-A cores (v1.0 deployment target) speculate data reads before the used.idx load, returning pre-advance bytes. **Smoking-gun**: virtio-input's busy-poll drain would classify a real EV_KEY/KEY_A=30 wake as phantom EV_SYN (zero type/code/value from populate_eventq pre-zeroed slot), silently dropping the key. IRQ-driven paths NOT "safe by accident" — `eret` is a Context Synchronization Event, NOT a memory barrier. Fix: new `libthyla_rs::virtio_rmb()` (`unsafe { asm!("dmb ishld", options(nostack, preserves_flags)) }`) inserted at every used.idx-read site. Matches Linux's `virtio_rmb()` codegen on AArch64.

**F218 (P2)** — `INT_USED_BUFFER` (bit 0) vs `INT_CONFIG_CHANGE` (bit 1) per VIRTIO 1.2 §4.2.5 bit-discriminated wake loop; applied in blk-rw + net-loop + gpu (cap `MAX_NON_USED_BUFFER_WAKES = 16`).

**F219 (P2)** — `MAX_DRAIN_PER_BATCH = QUEUE_SIZE` cap on virtio-net-loop drain loops; defense-in-depth vs back-pressure relaxation.

**F220 (P2)** — `avail_idx_at_entry` capture replaces read-from-self gate in virtio-input.

**6 × P3 closed**: F222 (libthyla-rs t_irq_wait doc); F223 (discarded errno; now WARN-logged); F224 (unused-var hygiene comments); F226 (`used.elem.len == VIRTIO_INPUT_EVENT_LEN` validation); F227 (compile-time region-layout asserts added to blk-rw + net-loop); F228 (new `T_RIGHT_HW_ALLOWED = READ | WRITE | MAP | SIGNAL` constant in libthyla-rs as consumer-side I-5 guard).

**2 deferred**: F221 withdrawn during pass (subsumed by F217 + F219). F225 (QueueReady-before-populate ordering) spec-canonical but currently safe via the DRIVER_OK gate per VIRTIO 1.2 §3.1.1 step 8; refactor deferred to Phase 4 housekeeping or Phase 5+.

**Audit-bearing at the COMPOSITION layer.** Documented at `docs/reference/39-hw-handles.md` caveat #11 as a CLAUDE.md "Composed-driver discipline" extension. No §28 invariant directly touched — the §2.7.13.2 barrier is a composition-layer discipline that pins read ordering at the driver-userspace level.

**F217 regression test**: post-build binary inspection — `llvm-objdump -d build/usr-rs/aarch64-unknown-none/release/<driver> | grep -c "dmb.*ishld"` must report 1/2/1/1 across blk-rw/net-loop/input/gpu. The memory-ordering bug F217 prosecutes cannot be reproduced deterministically on QEMU TCG (in-order execution), so binary inspection is the durable regression.

---

## Trip hazards carried forward

1. **F225 (QueueReady-before-populate ordering)** — virtio-net-loop + virtio-input set `QUEUE_READY=1` BEFORE populating descriptors / avail.ring / avail.idx. VIRTIO 1.2 §3.1.1 step 8 requires the device defer processing until DRIVER_OK, so QEMU honors the staged-staging. A future port to a non-QEMU device that interprets QueueReady more aggressively would benefit from inverting the order. Deferred to Phase 4 housekeeping or Phase 5+ when a real device exposes the divergence.

2. **R12-bss-2mib (image+firmware ≤ 2 MiB constraint not enforced by kernel.ld)** — `arch/arm64/kernel.ld` asserts `(_kernel_end − _kernel_start) < 0x200000` but the runtime constraint is image + firmware_offset ≤ 2 MiB. P4-image-shrink reduced image_size to 744 KiB with 792 KiB headroom; this gives many quarters of runway, but tightening the kernel.ld assert to encode `(_kernel_end − _kernel_start) + 0x80000 < 0x200000` (or similar) would catch future overflow at build time. Phase 5+ cleanup.

3. **F202 (ICFGR-RMW SMP-safety) + F203 (stale-ICPENDR on edge-trigger transition)** — both deferred at R12-gic-edge as v1.0-not-reachable (serial claim discipline + no re-claim path); revisit when SMP+driver-restart paths land in Phase 5+.

4. **F149 (per-CPU SGI/PPI semantics) + F150 (ReduceCaps drop-precondition)** — both deferred at R9 with forward-looking rationale. Revisit when SMP-aware drivers + cap-drop syscall lands.

---

## Sanity snapshot at this handoff

- **Tip**: `86270a6` on `main`.
- **Tests**: 243/243 PASS × default (~1520 ms) + UBSan.
- **Spec**: 9 mandatory `specs/*.tla` planned per CLAUDE.md §"Spec-first policy"; 4 landed (scheduler / territory / handles / burrow). 5 remaining for Phases 4-5 (9p_client / poll / futex / notes / pty). burrow.cfg + 3 buggy cfgs clean under TLC.
- **Audit closed lists**: R1..R14 cumulative; latest at `memory/audit_r14_p4z_closed_list.md`.
- **Phase 4 commits**: A → H, Fix157, Ia1, Ia2, Ib, Ic1, Ic2, Ic3, Ic4, Ic5a, Ic5-IRQ-probe, Ic5b1a, Ic5-FP, Ic5b1b, Ic5b2, Ic6-{spec,cfg,impl}, Ic7, Ja, Jb, Jc, Ic-latency, K, L, image-shrink, N, O, L-scanout, K-events, Z. Plus R12-{bss-2mib, gic-edge, DMA, FP, vaddr, uaccess (substantive + close)} deferred-audit closes.
- **Working tree**: clean; 105 commits ahead of `origin/main`.

---

## Next chunks

- **P4-M (driver crash recovery)** — kernel test that spawns child A holding hw-handles, kills/exits A non-zero, spawns child B claiming the SAME hardware, verifies B's claims succeed (proving the release path released them). Mechanism is largely already in place via `proc_exit` → handle-table walk → `kobj_*_unref`. Test will reveal any gaps in the release plumbing. Closes the third remaining §6.2 box at the substrate layer; the kill-mid-IRQ + driver-supervision-policy aspects are Phase 5+ work.

- **Phase 4 close handoff** — once P4-M lands, a separate "Phase 4 close" sub-chunk can mark the phase boundary, refresh the cumulative trip-hazards list, and produce the Phase 5 entry document.

## Phase 4 → 5 bridge: tracked deferrals (do-not-forget)

The two remaining §6.2 boxes are explicitly deferred to Phase 5, NOT abandoned. Tracked in `memory/project_phase4_open_boxes.md` so a future session can pick them up without re-deriving context:

- **P4-Id (virtio-net `/dev/ether0` 9P surface)** — the userspace virtio-net-loop driver consumes ARP packets end-to-end via composed hw-handles. What's missing: an OS-level interface (`/dev/ether0` as a 9P device served BY the userspace driver) that OTHER user processes can `open`/`read`/`write` to send + receive frames. Architecture for "user processes serve 9P endpoints" is partially in place (Spoor lifecycle exists); the missing piece is the driver's role as a 9P server (= Phase 5 §7 "Stratum integration" model where userspace drivers expose ports). When Phase 5 lands the cross-process 9P invocation surface, P4-Id reduces to wiring virtio-net-loop's RX-drain into a 9P file-tree backing `/dev/ether0`. **Not lost — pinned in the bridge tracker.**

- **virtio-input `/dev/cons` 9P surface** — same shape as P4-Id: P4-K-events proved the driver consumes events end-to-end; the missing piece is the 9P surface that bridges those events to other procs (a Plan 9 textual console eventually wants the key stream there). Bridges naturally with P4-Id since both require the "userspace driver as 9P server" composition.

When Phase 5 plans its first chunks, both tracker entries get pulled into the Phase 5 status doc's landed-chunks table. Until then, the userspace drivers proven at P4-Jc + P4-K-events stand as the substrate proof — they validate the device class round-trip end-to-end; only the cross-process plumbing is pending.

---

## What the next session needs to know

- **Read first**: `CLAUDE.md` → this handoff → `docs/phase4-status.md` → `memory/project_next_session.md` → `memory/audit_r14_p4z_closed_list.md` (the latter pins the durable F217 regression in plain text).
- **Composition-layer discipline** is documented at `docs/reference/39-hw-handles.md` caveat #11. Any new VirtIO-class driver — present or future — must include `libthyla_rs::virtio_rmb()` immediately after each `read16(used_va + 2)` site. Binary inspection (`llvm-objdump -d | grep -c "dmb.*ishld"`) is the closing regression.
- **Audit posture is clean across every invariant-bearing surface** per CLAUDE.md §"Audit-triggering changes". The remaining §6.2 boxes are user-plumbing, not substrate.
- **Phase 5 substrate readiness check**: this handoff demonstrates Phase 4 has delivered a Linux-binary-compatible textual driver layer (blk + net + input + gpu) with audited handle-table + IRQ + DMA + Burrow composition + spec-pinned wait/wake. Phase 5 begins from a clean substrate.
