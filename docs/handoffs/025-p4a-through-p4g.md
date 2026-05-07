# Handoff 025 — Phase 4 sub-chunks A through G (driver substrate complete)

**Tip**: `c8430be` (P4-G hash fixup). Predecessor: `17b4845` (handoff 024 at the Phase 4 entry cliff).

## TL;DR

Phase 4 sub-chunks A through G all landed in one session. The kernel-side **driver substrate** is now complete — every dependency for the upcoming userspace drivers (P4-I/J/K/L) is in place: Dev vtable + Spoor lifecycle + bestiary registry, the trivial leaf Devs (cons / null / zero / random), the directory Devs (proc / ctl / ramfs), VirtIO MMIO transport + split virtqueue, and IRQ forwarding to KObj_IRQ blockers.

**Bestiary count: 1 (devnone) → 8** (cons + null + zero + random + proc + ctl + ramfs + the original devnone). **Test count: 96 → 169**.

The next step is either **P4-H** (virtio_pci, minimal PCIe enumeration; #157-independent) or — more important — **investigating trip-hazard #157** (the second-userspace-iteration hang) which gates **P4-I** (first userspace driver). The session-window milestone: every kernel-side prerequisite for userspace drivers is in. The remaining Phase 4 work is mostly **userspace driver implementation**.

---

## What landed in this session window (after handoff 024)

### P4-A: Dev vtable + Spoor lifecycle + bestiary + devnone (`9e2b348` / `370eb74`)

- New `<thylacine/dev.h>`: ARCH §9.2 verbatim 16-op vtable with C99 const-correct typing on read-only inputs. `bestiary[]` registry (sentinel-terminated, max 32 entries) + `dev_register / dev_lookup_by_dc / dev_lookup_by_name / dev_count`.
- New `<thylacine/spoor.h>`: Plan 9 `Chan` equivalent with `SPOOR_MAGIC` (offset 0; SLUB freelist write clobbers on UAF) + `struct Qid` (16 B, pinned by `_Static_assert`) + `struct Walkqid` (flexible-array tail) + counter discipline (`spoor_total_allocated/freed`).
- New `kernel/spoor.c` (~150 LOC): `alloc/ref/unref/clone/clunk` lifecycle mirroring `burrow.c`'s magic + counter pattern.
- New `kernel/dev.c` (~120 LOC): bestiary state + `dev_init()` walks the bestiary calling each `->init()` once (watermark loop tolerates devs registering more devs from inside their own init).
- New `kernel/devnone.c` (~110 LOC): no-op stub Dev (dc='-', name="none"). All ops safe no-ops or graceful failures (-1 / NULL / void). Sentinel anchor + audit guard.
- Boot wire-up: `dev_init()` after `sched_init` in `boot_main`. Banner adds `dev:  N registered (none, ...)` line.
- 9 tests; reference doc `docs/reference/30-dev-spoor.md`.

### P4-B: Trivial Devs — cons / null / zero / random (`6f417e9` / `a016169`)

- 4 new C files (~500 LOC total): `kernel/cons.c` (UART forward via `uart_putc`), `kernel/null.c` (bit bucket), `kernel/zero.c` (zero source), `kernel/random.c` (ARM64 RNDR-backed CSPRNG).
- `dev_simple_{attach,open,close}` helpers in `kernel/dev.c` factor the common leaf-file plumbing.
- **Bug fix in-chunk**: initial RNDR success-bit check used PSTATE.C (bit 29). Per ARM ARM (FEAT_RNG), success is **Z=0** (bit 30); C is always 0. Fixed via the `cset Wd, ne` idiom with `"cc"` clobber.
- Bestiary 1 → 5; banner adds `random: RNDR available (FEAT_RNG)`.
- 12 tests; reference doc `docs/reference/31-trivial-devs.md`.
- **ROADMAP §6.2 exit criteria closed**: "Spoor lifecycle: 10K cycles on /dev/null without leak"; "cat /dev/random produces non-zero bytes". chacha20 stir per ROADMAP §6.1 held to a future hardening sub-chunk (defense-in-depth; API unchanged).

### P4-C: dev/proc — synthetic /proc/<pid>/ Dev (`c716f32` / `e618ace`)

- New `kernel/devproc.c` (~360 LOC): first directory-typed Dev (dc='p'). Walks `/proc/<pid>/{status, cmdline, ctl, ns}`. Qid-encoded namespace: `path = 0` for root, `path = (pid << 32) | subkind` for per-pid objects. Multi-step walk supported.
- Reads dispatch by qid kind into tiny inline formatters (no libc; ~30 LOC of byte-level `fmt_udec/fmt_sdec/fmt_str` into a 256-byte stack buffer; offset-aware copy). Status reports pid/state/threads/exit; cmdline reports argv[0] placeholder ("kproc" / "<unnamed>"); ns reports `binds: N`; ctl reads return 0 (write-only at v1.0).
- Writes: ctl returns n (commands consumed; verb-set + dispatch held to Phase 5+ when `specs/notes.tla` lands); other qid writes return -1.
- Reads on directory qids return -1 (readdir held until 9P readdir or in-kernel readdir helper lands).
- Adds `proc_find_by_pid` + `proc_for_each` to `kernel/proc.c` (DFS through `kproc.children->sibling` under `g_proc_table_lock`).
- New `walkqid_alloc` + `walkqid_free` helpers in `kernel/spoor.c` (kmalloc-backed Walkqid lifecycle for directory-typed Devs).
- Bestiary 5 → 6.
- 13 tests; reference doc `docs/reference/32-devproc.md`.

### P4-D: dev/ctl — kernel admin Dev (`f38c0c2` / `da6348e`)

- New `kernel/devctl.c` (~280 LOC): single-level directory Dev (dc='C'). Walks `/ctl/{procs, memory, devices, kernel-base, sched}`. Per-leaf format generators: procs uses `proc_for_each` (callback-based formatter under `g_proc_table_lock`); memory calls phys accessors; devices iterates `bestiary[]`; kernel-base reads KASLR diagnostics; sched reports runnable count.
- Writes rejected at v1.0 (admin commands held to Phase 5+ syscall surface).
- Tiny formatters duplicated from devproc.c at v1.0; future shared `<thylacine/fmt.h>` when third caller emerges.
- Bestiary 6 → 7.
- 11 tests; reference doc `docs/reference/33-devctl.md`.

### P4-E: dev/ramfs — cpio-loaded in-memory FS + initrd reservation fix (`5a90b77` / `b15b702`)

- Full pipeline per ROADMAP §6.1 "cpio-loaded at boot":
  - `tools/mkcpio.py` (~80 LOC host script).
  - `tools/build.sh::build_ramfs` (chained off `build_kernel`; auto-generates `welcome` + `version` text fixtures).
  - `tools/run-vm.sh -initrd build/ramfs.cpio` flag.
  - New `kernel/cpio.{h,c}` (~200 LOC; newc parser per cpio(5)): 110-byte ASCII-hex header parser + 4-byte alignment + "TRAILER!!!" terminator.
  - New `dtb_get_chosen_initrd` in `lib/dtb.c` (probes `/chosen/linux,initrd-{start,end}`; supports 4- or 8-byte big-endian cells).
  - New `kernel/devramfs.c` (~270 LOC, dc='m'): static file table populated at init from initrd PA via `pa_to_kva`.
- **Bug fix in-chunk**: `mm/phys.c::phys_init` now reserves the initrd PA range (5th reservation entry). Pre-P4-E the buddy could reuse initrd memory + clobber the cpio bytes that `g_ramfs_files[].name` and `.data` point at. Exposed deterministically under UBSan (kernel image ~9% bigger; allocations push past the threshold).
- Bestiary 7 → 8; banner adds `ramfs: 2 files loaded from initrd (412 bytes)`.
- 17 tests (7 cpio unit against synthetic byte-by-byte archives + 10 devramfs integration); reference doc `docs/reference/34-devramfs.md`.

### P4-F: VirtIO core — MMIO transport + split virtqueue (`e6e5507` / `87f72f5`)

- New `<thylacine/virtio.h>` (~190 LOC): VIRTIO 1.2 register offsets + device IDs + status bits + vring types. `_Static_assert` pins on `vring_desc` (16 B) + `vring_used_elem` (8 B).
- New `kernel/virtio.c` (~270 LOC): DTB probe via the new `dtb_for_each_compat_reg` multi-match enumeration helper (added in `lib/dtb.c`). Per-slot probe maps the page-aligned MMIO + reads MagicValue / Version / DeviceID / VendorID. Status state machine + feature negotiation per VIRTIO 1.2 §3.1.1 steps 1-5. Virtqueue allocation with 3 separate page-aligned rings per modern transport's QUEUE_DESC/DRIVER/DEVICE-low/high split.
- **Bug fix in-chunk**: virtio-mmio slots in QEMU virt are 0x200 (512) bytes apart — only every 8th slot starts a 4 KiB page. Initial probe called `mmu_map_mmio(slot_pa, slot_size)` which extincted on the second slot ("pa not page-aligned"). Fix: probe page-aligns the PA, rounds up the size, computes the slot's KVA as `page_kva + (slot_pa - page_pa)`.
- `tools/run-vm.sh` adds `-device virtio-rng-device,id=rng0` for tests (gives at least one slot with DeviceID=4 RNG).
- Boot banner adds `virtio: 32 MMIO slots probed (1 with attached devices, 0 skipped)`.
- 7 tests; reference doc `docs/reference/35-virtio.md`.

### P4-G: irqfwd — IRQ forwarding to KObj_IRQ blocker (`ce380f9` / `c8430be`)

- New `<thylacine/irqfwd.h>` + `kernel/irqfwd.c` (~140 LOC). KObj_IRQ struct (magic + intid + refcount + Rendez + edge-triggered pending_count).
- `kobj_irq_create(intid)` registers the GIC handler + enables the IRQ. `kobj_irq_dispatch` (GIC hook) increments `pending_count` under `r->lock` + drops the lock + wakeup (drop-before-wakeup avoids self-deadlock since wakeup re-takes the lock). `kobj_irq_wait(k)` blocks on the Rendez via `sleep(r, cond=pending_count>0, k)`, atomically reads + zeroes the counter on return.
- Wait/wake atomicity matches `scheduler.tla::NoMissedWakeup` (I-9). Three orderings (early-fire / late-fire / concurrent-during-sleep-transition) all yield count >= 1 on return.
- SGI 1 (`IPI_IRQFWD_TEST`) reserved for tests; SGI 0 stays IPI_RESCHED.
- 4 tests; reference doc `docs/reference/36-irqfwd.md`.

---

## Current state (post-P4-G, 2026-05-07)

- **Phase 0 complete.** All scripture committed and binding.
- **Phase 1 CLOSED** at `ceecb26`.
- **Phase 2 CLOSED** at `5914230`.
- **Phase 3 CLOSED** at `c2d7886` / `6446715` (P3-H).
- **Phase 4 OPEN; A through G landed.** Phase 4 entry: `0d1917a`. Handoff 024: `17b4845`. P4 sub-chunk commits:
  - P4-A: `9e2b348` / `370eb74`
  - P4-B: `6f417e9` / `a016169`
  - P4-C: `c716f32` / `e618ace`
  - P4-D: `f38c0c2` / `da6348e`
  - P4-E: `5a90b77` / `b15b702`
  - P4-F: `e6e5507` / `87f72f5`
  - P4-G: `ce380f9` / `c8430be`
- **Tip is `c8430be`**.

**Tests**: **169/169 in-kernel tests PASS** × default + UBSan. ~393 ms boot (default); ~436 ms UBSan; fault matrix 4/4; KASLR 5/5 distinct.

**Specs**: 4 specs + **13 cfg variants**. All correct configs PASS; all buggy configs produce expected counterexamples. (scheduler 25416, liveness 23, liveness_wfi 5760, territory 625, burrow 100, handles 6.05M.) Phase 4 has been impl-only — no new TLA+ modules added.

**Bestiary** (post-P4-G): 8 Devs registered.

```
dev:  8 registered (none, cons, null, zero, random, proc, ctl, ramfs)
```

Plus the kernel-internal substrate (not bestiary entries):
- `kernel/virtio.c` — VirtIO MMIO transport + virtqueue (P4-F).
- `kernel/irqfwd.c` — KObj_IRQ blocker + GIC dispatch hook (P4-G).
- `kernel/cpio.c` — newc parser (P4-E support module).

**Open audit findings**: 0 unfixed P0/P1/P2. Cumulative deferrals (forward-looking, all Phase 5+): F108/F109/F110 (R6-A); F113/F115/F116/F119 (R6-B); F130/F132/F137 (R7); ~21 P3 + 5 P2 from R5-F/G/H. None blocking.

**Open trip-hazards**: **#157 — second-userspace-iteration hang. Forward-looking P0 (Phase 4 blocker).** Reproduced + bisected at P3-H; root cause beyond TLB/cache/recycle (suspected QEMU-virt simulation state with `-cpu max`). v1.0 mitigation: F136 one-call guard. Reproducer at `kernel/test/test_userspace2.c` (orphaned in tree). MUST be root-caused before **P4-I** (first userspace driver — virtio-blk) or **P4-M** (driver supervision; respawns drivers on crash).

**Boot output (key lines)**:
```
random: RNDR available (FEAT_RNG)
ramfs: 2 files loaded from initrd (412 bytes)
dev:  8 registered (none, cons, null, zero, random, proc, ctl, ramfs)
virtio: 32 MMIO slots probed (1 with attached devices, 0 skipped due to magic mismatch or cap)
joey: rforking child for /joey (9-instr hello blob)
hello
joey: /joey pid=N exited cleanly (status=0)
Thylacine boot OK
```

---

## Verify on session pickup

```bash
git log --oneline -15
# Expect:
#   c8430be P4-G: hash fixup
#   ce380f9 P4-G: irqfwd — IRQ forwarding to KObj_IRQ blocker
#   87f72f5 P4-F: hash fixup
#   e6e5507 P4-F: VirtIO core — MMIO transport + split virtqueue
#   b15b702 P4-E: hash fixup
#   5a90b77 P4-E: dev/ramfs — cpio-loaded in-memory FS + initrd reservation fix
#   da6348e P4-D: hash fixup
#   f38c0c2 P4-D: dev/ctl — kernel admin Dev
#   e618ace P4-C: hash fixup
#   c716f32 P4-C: dev/proc — synthetic /proc/<pid>/ Dev
#   a016169 P4-B: hash fixup
#   6f417e9 P4-B: kernel-internal trivial Devs (cons / null / zero / random)
#   370eb74 P4-A: hash fixup
#   9e2b348 P4-A: Dev vtable + Spoor lifecycle + bestiary + devnone stub
#   17b4845 Phase 4 entry: handoff 024 — Phase 3 close + Phase 4 entry rename

git status
# Expect: clean (untracked: docs/estimate.md, kernel/test/test_userspace2.c, loc.sh).

tools/build.sh kernel
tools/test.sh
# Expect: 169/169 PASS, ~393 ms boot. Boot output as above.

tools/test.sh --sanitize=undefined
# Expect: 169/169 PASS, ~436 ms boot.

tools/test-fault.sh
# Expect: 4/4 PASS.

tools/verify-kaslr.sh -n 5
# Expect: 5/5 distinct.

# Specs (no posture change in this session):
[ -f /tmp/tla2tools.jar ] || curl -sL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs
for cfg in scheduler.cfg scheduler_liveness.cfg scheduler_liveness_wfi.cfg \
           territory.cfg burrow.cfg handles.cfg; do
  spec="${cfg%.cfg}"
  case "$spec" in scheduler_*) spec="scheduler" ;; esac
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
       -config "$cfg" "$spec.tla" 2>&1 | tail -2
done
# Expect: each finishes "in <time>" with no error / no counterexample.
```

If any fails on a clean checkout, something regressed since handoff — investigate before proceeding.

---

## What's NEXT — Phase 4 sub-chunks remaining

Two parallel tracks:

### Track 1: P4-H (virtio_pci) — #157-independent

**P4-H: minimal PCIe enumeration for VirtIO-PCI devices.** New `kernel/virtio_pci.c`. QEMU virt has a PCIe root complex at /pcie node in DTB. Minimal scope: parse the PCIe ECAM (Enhanced Configuration Access Mechanism) range, enumerate devices, find virtio-pci (vendor 0x1AF4 + device IDs 0x1040..0x107F for modern, 0x1000..0x103F for legacy), expose them via `virtio_mmio_dev_get`-equivalent.

Mostly required for VirtIO-GPU at v1.0 (per ARCH §13: GPU is PCI-only on QEMU virt). Block / net / input use MMIO transport. Could land any time before P4-Z; doesn't block on #157.

### Track 2: Trip-hazard #157 root-cause investigation — gates P4-I

**Trip-hazard #157** is the forward-looking P0 from R7. Required before:
- **P4-I**: virtio-blk userspace driver (first chunk that exec's a fresh userspace process).
- **P4-M**: driver supervision (respawns drivers on crash; multiple userspace exec invocations).

Surgical-fix candidates from R7 audit:
- Run with `-cpu cortex-a76` (or `cortex-a72,+lse`) to disable MTE/BTI/PAC; check if MTE-related.
- Add `dc civac` on freed pgtable pages before they reach buddy.
- Set `TCR_EL1.TCMA0=1` to disable MTE checks in user half.
- Run under `qemu-system-aarch64 -d in_asm,exec,int,mmu` for instruction trace post-eret on iter 2.
- Reproducer at `kernel/test/test_userspace2.c` (orphaned in tree).

### After P4-H / #157

1. **P4-I**: userspace virtio-blk driver (Rust). First chunk that exec's a Rust userspace process. Trip-hazard #157 gate.
2. **P4-J/K/L**: virtio-net / virtio-input / virtio-gpu drivers.
3. **P4-M**: driver supervision (init/driver-supervisor). Watches driver process exit; restarts on crash.
4. **P4-N**: BURROW finalize — `specs/burrow.tla` reconciliation + impl audit.
5. **P4-Z**: Phase 4 closing audit.

---

## Important commitments (cumulative)

- **C99 kernel** with Rust-port-friendly discipline.
- **Userspace drivers from Phase 4** — no in-kernel virtio-{blk,net,input,gpu} shortcuts. Driver substrate (P4-A through P4-G) exists in kernel; the drivers themselves are userspace Rust processes.
- **9P2000.L + Stratum extensions** as the universal protocol (Phase 5 ROADMAP §7).
- **Spec-first BINDING from Phase 2** — held through Phase 3 close + Phase 4 P4-A through P4-G (impl-only chunks; no new TLA+ modules). Phase 4 N requires `specs/burrow.tla` reconciliation; Phase 5+ adds `specs/9p_client.tla`, `specs/poll.tla`, `specs/futex.tla`, `specs/notes.tla`, `specs/pty.tla`.
- **SOTA hardening from Phase 1** — KASLR, W^X, canaries, PAC, BTI, LSE all live.
- **Halcyon held to Phase 8** — risk isolation; v1.0-rc.1 from Phase 7 is the shippable fallback.
- **Stratum dependency**: Stratum is feature-complete on Phases 1-7; Phase 9 (9P server + extensions) is Thylacine Phase 5's integration target.

---

## Naming conventions (post-rename, unchanged from handoff 024)

| Concept | Plan 9 / generic | Thylacine |
|---|---|---|
| File/resource handle | Chan | Spoor |
| Process namespace | Pgrp + namespace | Territory |
| Memory object | Vmo | Burrow |
| Device registry | devtab | bestiary |
| WFI halt loop | _hang | _torpor |
| First userspace process | /init | /joey |
| Kernel panic | panic | extinction |

**Kept (Plan 9 portability)**: `Proc`, `Thread`, `Rendez`, `sleep`, `wakeup`, `rfork`, `exits`, `wait_pid`, `bind`, `mount`, `unmount`, `RFNAMEG`, `sched`, `ready`, `Dev` vtable, `Walkqid`, `qid`.

**Kept (Stratum continuity)**: audit-prosecutor agent name.

**Kept (industry-spec surface)**: `virtio`, `vring`, `mmio`, `cpio newc`, GIC names per ARM IHI 0069, PSCI per Arm DEN 0022D.

---

## Reference maintenance discipline

Per CLAUDE.md: maintain BOTH technical reference (`docs/REFERENCE.md` + `docs/reference/NN-*.md`) AND user reference (`docs/USER-MANUAL.md` + `docs/manual/`) per-chunk, deep, binding.

Phase 4 reference docs added in this session window:
- `30-dev-spoor.md` (P4-A)
- `31-trivial-devs.md` (P4-B)
- `32-devproc.md` (P4-C)
- `33-devctl.md` (P4-D)
- `34-devramfs.md` (P4-E)
- `35-virtio.md` (P4-F)
- `36-irqfwd.md` (P4-G)

Total reference doc growth: ~2200 lines covering the full driver substrate.

User reference (`docs/USER-MANUAL.md`) has not been touched in this session — Phase 4 work is internal scaffolding without user-visible behavior change beyond boot banner additions. Phase 5+ syscall surface + the first userspace shell will trigger user-manual updates per CLAUDE.md "Maintenance discipline (per-chunk; non-negotiable)".

---

## Phase numbering (ongoing)

Local impl phase numbering still diverges from ROADMAP scripture by one. Local Phase 4 = ROADMAP §6 (Device model + userspace drivers). The P4-* sub-chunk naming is local. Status docs use local numbering; ROADMAP references use scripture numbering with explicit cross-reference.

Held for explicit signoff (per CLAUDE.md scripture-binding rules): renumber ROADMAP scripture to match local impl OR retroactively rename local phases. Either decision needs user signoff.

---

## Open follow-ups

- **Trip-hazard #157**: Phase 4 blocker (P0 forward-looking). Required before P4-I (virtio-blk userspace driver) and P4-M (driver supervision). Investigation candidates listed in `What's NEXT` above + `audit_r7_closed_list.md`.
- **P3-H deferred audit findings** (R7): F130 (defense-in-depth W^X), F132 (proc_free TLB-flush ordering), F137 (proc_alloc rollback symmetry) — all forward-looking; revisited at Phase 5+.
- **chacha20 stir** for /dev/random per ROADMAP §6.1 — defense-in-depth on top of RNDR; future hardening sub-chunk; API unchanged.
- **/proc/<pid>/ctl verb parser + note dispatch** — Phase 5+ when `specs/notes.tla` lands.
- **/ctl/ admin command writes** — Phase 5+ syscall surface.
- **/ctl/ nested directories** per ARCH §9.4 (`kernel/`, `sched/`, `9p/`, `irq/`, `mm/`, `security/`, `log/`) — Phase 4+ chunk that extends qid encoding for multi-level paths.
- **/joey blob via ramfs** — joey.c builds inline; future chunk swaps to ramfs lookup.
- **Driver binaries in ramfs** — P4-I+ Rust userspace drivers.
- **Initrd freeing on Stratum mount** — Phase 5+ Stratum integration.
- **Shared `<thylacine/fmt.h>`** — formatters duplicated in devproc.c + devctl.c at v1.0; refactor when third caller emerges (likely P4-H virtio_pci diagnostics).
- **Handle-table integration for KOBJ_IRQ** — held to P4-I+ alongside first userspace driver.
- **`specs/burrow.tla` reconciliation** + impl audit — P4-N.
- **Phase 4 closing audit** (P4-Z) — covers all driver-model surfaces; verifies VISION §4.5 IRQ→userspace latency p99 < 5µs (needs userspace drivers to measure).

---

## Build + verify commands (full matrix)

```bash
# Default build + tests (canonical posture check):
tools/build.sh kernel
tools/test.sh

# UBSan trapping build:
tools/build.sh kernel --sanitize=undefined
tools/test.sh --sanitize=undefined

# Deliberate-fault matrix (canary / W^X / BTI / kstack_overflow):
tools/test-fault.sh

# Multi-boot KASLR variability:
tools/verify-kaslr.sh -n 5

# Specs (download tla2tools if /tmp was cleared):
[ -f /tmp/tla2tools.jar ] || curl -sL -o /tmp/tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"
cd specs && for cfg in *.cfg; do
  spec="${cfg%.cfg}"
  case "$spec" in scheduler_*) spec="scheduler" ;; esac
  java -cp /tmp/tla2tools.jar tlc2.TLC -workers auto -deadlock \
       -config "$cfg" "$spec.tla" 2>&1 | tail -2
done

# ramfs cpio rebuild (chained off build_kernel; standalone target also exposed):
tools/build.sh ramfs
```

---

## File inventory — what's new in this session window

**New kernel source files** (10 source + 4 internal headers + 6 reference docs + 2 host-tooling files):

- `kernel/include/thylacine/dev.h`
- `kernel/include/thylacine/spoor.h`
- `kernel/dev.c`, `kernel/spoor.c`, `kernel/devnone.c`
- `kernel/cons.c`, `kernel/null.c`, `kernel/zero.c`, `kernel/random.c`
- `kernel/devproc.c`, `kernel/devctl.c`
- `kernel/devramfs.c`, `kernel/cpio.c`
- `kernel/include/thylacine/cpio.h`
- `kernel/virtio.c`, `kernel/include/thylacine/virtio.h`
- `kernel/irqfwd.c`, `kernel/include/thylacine/irqfwd.h`
- `kernel/test/test_dev.c`, `test_trivial_devs.c`, `test_devproc.c`, `test_devctl.c`, `test_cpio.c`, `test_devramfs.c`, `test_virtio.c`, `test_irqfwd.c`
- `tools/mkcpio.py`
- `docs/reference/30-dev-spoor.md` through `36-irqfwd.md`

**Modified existing files**:

- `kernel/main.c` (boot wire-up: `dev_init`, `virtio_init`)
- `kernel/CMakeLists.txt` (source list growth)
- `kernel/test/test.c` (registry growth — 173 entries)
- `kernel/proc.c` (`proc_find_by_pid` + `proc_for_each` added in P4-C)
- `kernel/include/thylacine/proc.h` (API extension)
- `kernel/include/thylacine/dtb.h` (dtb_get_chosen_initrd + dtb_for_each_compat_reg)
- `lib/dtb.c` (initrd probe + multi-match enumeration helper)
- `mm/phys.c` (initrd PA reservation — closes the buddy-clobbers-cpio bug)
- `tools/build.sh` (build_ramfs target chained off build_kernel)
- `tools/run-vm.sh` (`-initrd` + `-device virtio-rng-device` flags)
- `docs/REFERENCE.md` (snapshot bumped per chunk)
- `docs/phase4-status.md` (per-chunk landed table)

**Total LOC added this session window**: ~6500 LOC kernel/source + ~2400 LOC reference docs + ~400 LOC host tooling ≈ **~9300 LOC**.

---

## Bug-hunting / lessons learned

Three in-chunk bugs surfaced + were fixed:

1. **P4-B**: RNDR success-bit on PSTATE.C (bit 29) instead of Z=0 (bit 30) per ARM ARM. Fixed via `cset Wd, ne` capture pattern with `"cc"` clobber.

2. **P4-E**: `mm/phys.c::phys_init` didn't reserve the initrd PA range — buddy could reuse those pages and clobber cpio bytes. Exposed deterministically under UBSan; pre-fix the `walk_to_welcome` test failed because the cpio name field had been overwritten between init and test runtime. Fixed via 5th conditional reservation entry.

3. **P4-F**: virtio-mmio slots in QEMU virt are 0x200 (512) bytes apart — only every 8th slot starts a 4 KiB page. Probe initially called `mmu_map_mmio(slot_pa, slot_size)` which extincted on the second slot ("pa not page-aligned"). Fixed by page-aligning the PA, rounding up the size, and computing per-slot KVA as `page_kva + (slot_pa - page_pa)`.

All three were caught by existing test infrastructure within the same chunk. None required an audit roundtrip.

---

## References

- `docs/handoffs/024-phase3-close-phase4-rename.md` — predecessor handoff at Phase 4 entry cliff.
- `docs/phase4-status.md` — Phase 4 sub-chunk landing table (P4-A through P4-G filled in; P4-H+ remaining).
- `docs/REFERENCE.md` — snapshot section + Contents table (entries 30-36 added).
- `docs/ARCHITECTURE.md` §9 (territory + Dev vtable + driver model) + §13 (MMIO + VirtIO).
- `docs/ROADMAP.md` §6 (Phase 4 deliverables + exit criteria).
- `memory/audit_r7_closed_list.md` — R7 closing audit + trip-hazard #157 deferral.
