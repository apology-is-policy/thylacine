# Phase 4 — status and pickup guide

Authoritative pickup guide for **Phase 4: Device model + userspace drivers** (per `ROADMAP.md §6`).

## TL;DR

Phase 4 lifts the kernel from "userspace runs" (Phase 3 close) to **"hardware is reachable from userspace via Plan 9 9P"**. Major deliverables: Dev vtable (Plan 9 idiom verbatim) + Spoor lifecycle infrastructure; kernel-internal synthetic Devs (`/dev/cons`, `/dev/null`, `/dev/zero`, `/dev/random`, `/proc`, `/ctl`, `/ramfs`); VirtIO core (in-kernel transport, MMIO + minimal PCIe); IRQ forwarding to userspace via `KObj_IRQ` blocker wakeups; userspace Rust drivers for virtio-blk / virtio-net / virtio-input / virtio-gpu; driver crash supervision.

The phase culminates with userspace drivers exposing their devices as 9P servers at `/dev/<name>/`. Phase 5 (= ROADMAP §7 by local numbering) then layers 9P client + Stratum integration on top.

Per CLAUDE.md spec-first policy: VirtIO core is impl-only (transport correctness checked by integration tests + adversarial audit; no new TLA+). BURROW refcount + mapping lifecycle (`specs/burrow.tla`) is finalized this phase. IRQ-handle wakeup is covered by `scheduler.tla`'s wait/wake protocol (already proved I-9 NoMissedWakeup).

**Local phase numbering note**: this is "Phase 4" by local convention; ROADMAP §6 is the canonical scripture text (titled "Phase 3" in the ROADMAP but representing the device-model phase that comes after our local Phase 3 close). The local renumbering stems from splitting ROADMAP Phase 2's deferred address-space deliverables across local Phase 2 + Phase 3. Working name: **P4-* sub-chunks**.

---

## Landed chunks

| Commit SHA | What | Tests |
|---|---|---|
| *(none yet — Phase 4 entry)* | | |

---

## Remaining work

Sub-chunk plan (refined as Phase 4 progresses):

1. **P4-A: Dev vtable + Spoor infrastructure**. NEXT. New `kernel/include/thylacine/dev.h` + `kernel/include/thylacine/spoor.h` + `kernel/dev.c` + `kernel/spoor.c`. Plan 9 17-op vtable (verbatim per ARCH §9.2). Spoor refcount + per-Spoor lock + spoor_alloc/free/clone/walk/clunk. bestiary[] sentinel-terminated registry + dev_register + dev_lookup_by_dc/by_name. devnone stub Dev (all ops return `-ENOSYS`) anchors unconfigured Spoors + audit guard. Boot wire-up via `dev_init()` in main.c.
2. **P4-B: kernel-internal trivial Devs** (cons, null, zero, random). New `dev/` subdirectory. UART → /dev/cons; /dev/null absorbs writes; /dev/zero produces zeroes; /dev/random uses ARM64 `RNDR` + chacha20 stir.
3. **P4-C: dev/proc**. Synthetic `/proc/<pid>/` exposing process state (status, cmdline, fd list, mem map). Walk-driven; no caching.
4. **P4-D: dev/ctl**. `/ctl/` for kernel admin: scheduler stats, IRQ counters, territory dump, `/ctl/proc-events/exit` for driver supervision.
5. **P4-E: dev/ramfs**. cpio-loaded in-memory FS at boot. Holds /init's blob (currently embedded in kernel image; ramfs decouples). Freed once persistent FS mounts at Phase 5.
6. **P4-F: VirtIO core (kernel/virtio.c)**. Split virtqueue. MMIO transport (VIRTIO_MMIO_*). Feature negotiation + virtqueue setup. Audit-trigger surface.
7. **P4-G: kernel/irqfwd.c**. Hardware IRQ → KObj_IRQ blocker wakeup. p99 < 5µs latency budget per VISION §4.5. Audit-trigger surface.
8. **P4-H: kernel/virtio_pci.c**. Minimal PCIe enumeration for VirtIO-PCI devices (one root complex, linear BAR allocation).
9. **P4-I: userspace virtio-blk driver** (Rust). First userspace driver. Validates the Dev/Spoor/9P/handle plumbing end-to-end.
10. **P4-J: userspace virtio-net driver**.
11. **P4-K: userspace virtio-input driver**.
12. **P4-L: userspace virtio-gpu driver**.
13. **P4-M: driver supervision** (init/driver-supervisor). Watches driver process exit; restarts on crash. Hooks into `/ctl/proc-events/exit`.
14. **P4-N: BURROW finalize** (`specs/burrow.tla` reconciliation + impl audit).
15. **P4-Z: Phase 4 closing audit**. Cumulative review covering Dev/Spoor/VirtIO/IRQ-fwd/drivers/supervision; ROADMAP §6 exit-criteria checklist; trip-hazard #157 root-cause investigation (Phase 4 blocker for any reading that requires multiple userspace exec — this phase will trigger it).

## Trip-hazard #157 — Phase 4 blocker (forward-looking P0)

Per audit R7 (P3-H closing audit), trip-hazard #157 is a forward-looking P0 that **must be root-caused before any Phase 4 chunk that triggers multiple userspace exec invocations** (specifically: P4-I virtio-blk driver, which exec's a Rust userspace process; P4-M supervision, which respawns drivers on crash; potentially P4-B if /dev/cons is exposed via a userspace driver later).

P4-A through P4-H are kernel-only and safe. The first P4-I chunk that exec's a userspace driver process becomes the gate. Investigation candidates from R7:
- Run with `-cpu cortex-a76` (or `-cpu cortex-a72,+lse`) to disable MTE/BTI/PAC; check if MTE-related.
- Add `dc civac` on freed pgtable pages before they reach buddy.
- Set `TCR_EL1.TCMA0=1` to disable MTE checks in user half.
- Run under `qemu-system-aarch64 -d in_asm,exec,int,mmu` for instruction trace post-eret on iter 2.

Reproducer at `kernel/test/test_userspace2.c` (orphaned in tree).

## Exit criteria status

Per `ROADMAP.md §6.2`, post-Phase-4-entry:

- [ ] `cat /dev/random` produces non-zero bytes.
- [ ] **Userspace virtio-blk**: read 1 GiB from VirtIO block device; write 1 GiB and read it back, verify bit-exact.
- [ ] **Userspace virtio-net**: send and receive raw Ethernet frames via `/dev/ether0`; checksum verified.
- [ ] **Userspace virtio-input**: keyboard input from VirtIO console reaches user processes via `/dev/cons`.
- [ ] **Userspace virtio-gpu**: write pixels to framebuffer via BURROW handle; visible on QEMU display.
- [ ] Spoor lifecycle: 10,000 open/read/close cycles on `/dev/null` without leak.
- [ ] Dev vtable: all 17 ops dispatch correctly for cons, null, zero, random, proc, ctl, ramfs.
- [ ] **Driver crash recovery**: kill the virtio-blk driver process mid-I/O; supervisor restarts; subsequent I/O resumes.
- [ ] **Hardware handle non-transferability**: attempt to transfer `KObj_MMIO` panics with explicit "non-transferable type" message.
- [ ] IRQ-to-userspace handler latency p99 < 5µs.
- [ ] `specs/burrow.tla` clean under TLC; `SPEC-TO-CODE.md` maintained.
- [ ] No P0/P1 audit findings on driver model.

## Build + verify commands

```bash
tools/build.sh kernel
tools/test.sh
tools/test.sh --sanitize=undefined
tools/test-fault.sh
tools/verify-kaslr.sh -n 5
```

## Trip hazards

(Cumulative from Phase 1+2+3 plus new Phase 4 entries.)

### NEW at Phase 4 entry

180. **Phase numbering vs ROADMAP scripture**. Local Phase 4 == ROADMAP §6 (titled "Phase 3"). The numbering shift stems from splitting ROADMAP Phase 2's deferred address-space deliverables across local Phase 2 + Phase 3. Either renumber ROADMAP scripture to match (user signoff required per CLAUDE.md scripture-binding) OR keep the offset and document. Status docs use local numbering; ROADMAP references use scripture numbering with explicit cross-reference.

(Future P4-* entries will append here.)

## References

- `docs/ROADMAP.md` §6 — Phase 4 binding scripture.
- `docs/ARCHITECTURE.md` §9 (territory + Dev vtable + driver model) + §13 (MMIO + VirtIO).
- `docs/phase3-status.md` — Phase 3 close.
- `memory/audit_r7_closed_list.md` — Phase 3 closing audit + trip-hazard #157 deferral.
