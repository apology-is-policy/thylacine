# pouch-stratumd-boot — running stratumd in Thylacine

Phase 6 sub-chunk 16 (`pouch-stratumd-boot`). Brings the
cross-compiled stratumd from sub-chunk 15 into the live boot path:
joey spawns stratumd, stratumd mounts a real pool, the kernel 9P
client connects to stratumd's `/srv` socket, joey mounts `/sysroot`,
ramfs pivots, the stub is retired. Closes Phase 5 + Phase 6.

The chunk is **sub-chunked**; 16b was further sub-chunked in-session
2026-05-25 under the stratumd-as-driver architectural decision (see
"Design decisions" below):

| Sub-chunk | Scope | Audit-bearing? |
|---|---|---|
| **16a** (landed 2026-05-24) | Binary-load probe: joey spawns stratumd with no args; verifies the binary loads + libc init runs + argv parsing executes + clean exit. | no (joey spawn path unchanged) |
| **16b-α** (next) | argv pass-through kernel surface (new SYS_SPAWN_* with argv buffer OR extended SYS_SPAWN_WITH_PERMS); pouch arm; joey constructs richer argv; the probe verifies argv is observable from inside stratumd. | **yes** (new kernel surface) |
| **16b-β** | Stratum-side `src/io/bdev_thylacine.c` (port of the userspace Rust `usr/virtio-blk-rw` driver) + pouch HW-syscall arm + joey spawns stratumd with `CAP_HW_CREATE` + end-to-end pool mount + `/srv` socket bind. | **yes** (block layer + cap surface + boot ordering) |
| **16c** (final) | Kernel 9P client connects to stratumd's `/srv` socket + joey mounts `/sysroot` + ramfs pivot + stub retire. | **yes** (boot ordering + I-1 namespace isolation) |

This file documents 16a; 16b-α + 16b-β + 16c extend it in place.

---

## Design decisions

### The stratumd-as-driver architecture (chosen 2026-05-25)

Sub-chunk 16b's load-bearing claim is "stratumd runs as a Thylacine
native process and mounts a real pool". Surfacing 16b implementation
forced a question that mid-design notes hadn't fully resolved: **what
provides the block-device backing under stratumd?**

Thylacine's block-device model is *userspace-driver-shaped*: virtio-blk
is a userspace Rust process (`usr/virtio-blk-rw`) that drives
virtio-mmio + DMA + IRQ directly via SYS_MMIO_* / SYS_DMA_* / SYS_IRQ_*
handles (P4-Ic5b). There is no kernel synth `/dev/blk/N` path.
Stratumd cannot simply `open("/dev/sda")`.

Four options surfaced:

1. **Writable in-RAM pool (α)** — new kernel synth Dev for a writable
   in-RAM pool, pre-initialized from a ramfs template. Pool resets
   each boot; doesn't yet prove real block backing.
2. **Separate `virtio-blkd` userspace daemon (β)** — new Rust daemon
   fronting virtio-blk via 9P. Real persistent backing. Multiple
   audit-bearing sub-chunks; effectively a small phase.
3. **Defer real backing (γ)** — close 16 minimally; real pool mounting
   moves to a post-Phase-6 bootloader/installer phase.
4. **Stratumd-as-driver (δ)** — stratumd holds `CAP_HW_CREATE` and
   drives the virtio-blk hardware directly. New Stratum-side
   `src/io/bdev_thylacine.c` arm; no separate daemon; no 9P-block
   protocol seam.

**Choice: (δ).** Rationale:

- **Plan-9-canonical**: "The filesystem owns its disk." Plan 9's disk
  servers historically drove their hardware directly. The decision is
  not architecturally novel; it's faithful to the lineage Thylacine is
  in.
- **Stratum already has platform arms**: `peer_creds.c` already carries
  Darwin/Linux/Thylacine arms; the `stm_bdev` abstraction is already
  the right seam for a Thylacine arm. We are on the `thylacine-pouch-arm`
  branch of Stratum precisely so this kind of code has a home.
- **Honest capability allocation**: `CAP_HW_CREATE` for stratumd
  reflects what stratumd is — the storage daemon. Granting it the
  storage hardware is not privilege creep; it is the correct authority
  for the role. I-2 (caps monotonically reduce post-grant) and I-5
  (HW handles cannot transfer) continue to hold; the cap-broadening
  attack surface is bounded by stratumd's known scope.
- **Persistent backing**: writes go to the QEMU disk.img (a second
  virtio-blk-device). Pool survives reboots. This is what the original
  16b scope wanted to prove.
- **No new daemon protocol**: (β) would require designing a
  9P-block-protocol between virtio-blkd and stratumd. Here, stratumd
  speaks to itself; no protocol seam to design and verify.
- **Reversible**: if process isolation later proves desirable (e.g., we
  decide block-driver bugs should not poison the FS server), the
  driver can be factored back out into a daemon. (δ) is not a one-way
  door.

**Honest concerns documented** (worth surfacing, not blockers):

- Stratumd LOC grows by ~500-800 lines (the C port of `virtio-blk-rw`
  into `bdev_thylacine.c`). The "stratumd is a fs server" framing gets
  stretched. Mitigation: partition the driver code clearly under
  `src/io/`; the FS-server core remains separable.
- Coupling stratumd to virtio-mmio specifics. Future block hardware
  (NVMe, real ATA, ...) needs new arms. This is true of any block
  driver in any OS; nothing about (δ) makes it worse than (β).
- A virtio-blk-driver bug crashes stratumd. In (β) the same bug
  crashes the daemon, and stratumd loses its filesystem anyway. The
  fault-isolation benefit of process separation is marginal when the
  data dependency is total.

### Sub-chunking 16b under (δ)

The choice of (δ) makes 16b genuinely two stages, not one:

- **16b-α**: argv pass-through. Orthogonal to the stratumd-as-driver
  decision (any non-trivial spawn wants argv pass-through). Smallest
  blast-radius new kernel surface; audit-bearing for the surface; lands
  independently.
- **16b-β**: the integration chunk. Stratum-side `bdev_thylacine.c` +
  pouch HW-syscall arm + joey spawn with `CAP_HW_CREATE` + end-to-end
  mount. Larger but coherent.

Sub-chunking further (e.g., split β into stratum-side-only and
joey-side-only) was considered but rejected: the Stratum-side change
is unobservable from Thylacine until joey wires it up, so testing
gates naturally at the integration point.

---

## 16a — stratumd binary-load probe

**Deliverable**: `/stratumd-probe` runs in joey; stratumd's binary
loads + libc init + argv parsing + clean exit verified. Surfaces any
runtime gaps in pouch's lowest layers (constructor failures,
`__pouch_note_handler` registration from sub-chunk 13b, TLS init,
musl's startup chain).

### What landed

| Component | Path |
|---|---|
| Ramfs wiring | `tools/build.sh::build_ramfs` — new `pouch_daemon_bins=( "stratumd" )` array. Copies stratumd from `$BUILD_DIR/pouch/progs/stratumd` into `build/ramfs-src/stratumd`. |
| Build-chain wiring | `tools/build.sh::build_kernel` — calls `build_stratumd` after `build_pouch_progs` so `tools/build.sh kernel` produces a complete ramfs including stratumd in one shot. CMake/ninja incremental on warm rebuilds (~<5s when no Stratum-side source changed; ~2-3 min cold). |
| build_pouch_progs preservation | Changed `rm -rf "$progs_out"` to a targeted `rm -f` of pouch-hello-* binaries only, so a build_pouch_progs run doesn't clobber the stratumd binary installed by build_stratumd in the same directory. |
| build_stratumd idempotency | Removed `rm -rf "$stratumd_build"` so CMake's cache + ninja's dep tracking can short-circuit on warm rebuilds. |
| joey probe | `usr/joey/joey.c` — new probe block before the stub bringup. `t_spawn("stratumd", ...)` + `t_wait_pid(&status)` + log "binary loaded, argv parsed, usage exit". |

### Why this probe is the right scope for 16a

Stratumd's full lifecycle needs a pool + a keyfile + a Unix socket
bind + a 9P client to consume the socket. Each of those is an
integration risk. **Before any of that**, we want to know:

- Does stratumd's ET_EXEC load into a Thylacine process? (kernel
  ELF loader accepts; W^X intact; TLS init works)
- Does pouch's musl `_start` chain run? (auxv handoff from
  exec_setup; CRT objects; .init_array constructors including
  the sub-chunk 13b notes handler register `__pouch_note_handler`)
- Does argv parsing run? (musl's argv exposure to main; stratumd's
  `argc < 2` gate)
- Does process exit work? (musl's `SYS_exit_group`)

If ANY of these are broken, every subsequent integration attempt
(16b, 16c) starts from a broken base. By isolating the probe to
"just runs argv parsing and exits", a failure is locatable.

### Why `argc < 2` works as the probe trigger

Stratumd's main is `stm_cmd_stratumd_main(argc, argv)` (in
`v2/src/cmd/stratumd/run.c`). The first executable line:

```c
int stm_cmd_stratumd_main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    ...
}
```

Joey spawns with `t_spawn` — which translates to
`SYS_SPAWN(name, name_len)` with no argv pass-through (the v1.0
spawn surface inherits exactly `argv[0]` = the binary's name).
So stratumd sees `argc == 1`, hits the gate, calls `usage()` (which
writes to stderr; fd 2 isn't pre-installed by the bare `t_spawn`,
so the writes drop silently), and returns 1.

Joey reads the exit status via `t_wait_pid(&status)`; any non-zero
status is success for THIS probe (the binary ran; the specific
status==1 is stratumd-specific but we don't depend on it for the
probe's contract).

### Observed boot output

```
joey: /stratumd-probe reaped status=1 (binary loaded, argv parsed, usage exit)
```

**Posture**: 586/586 PASS × default + UBSan, 0 UBSan runtime errors.
Boot green.

### What the probe did NOT exercise (and what 16b + 16c will)

The bare-argv-parsing path hits very little of stratumd's code:
- No `mlock` / `madvise` / `mprotect` calls (those live in the
  mount + key-handling paths, not in argv parsing).
- No socket bind (`bind` / `listen` on the FS Unix socket).
- No pthread creation (the acceptor + worker threads).
- No 9P codec, no AEAD, no Argon2id.
- No block device I/O.

So 16a's clean exit does NOT prove the deeper layers work. It
proves the toolchain + libc init + the argv path. Each of the
deeper layers gets exercised by 16b + 16c, and gaps surface there.

**Anticipated 16b/16c gaps** (from inspection of stratumd's source):

- `mlock(addr, len)` for the corvus session-token buffer
  (`src/corvus_client/corvus_client.c`) — pouch's musl maps this to
  the `0xFFFF` ENOSYS sentinel; corvus_client treats `mlock`
  failure as best-effort (per memory of the R144 close).
- `madvise(addr, len, MADV_DONTDUMP)` same path; best-effort.
- `mprotect(addr, len, prot)` may surface in mallocng's
  `madvise_collapse` path; v1.0 ENOSYS via sentinel.
- `statfs` / `fstatfs` — checks pool device free space; needs a
  pouch arm (currently 0xFFFF sentinel).
- `pwrite` / `pread` on a block device — works (sub-chunk 4
  syscall seam handled these).
- `bind(AF_UNIX, /srv/<name>)` — sub-chunk 12 patched musl to call
  `SYS_post_service_byte`; needs the calling Proc to have
  `PROC_FLAG_MAY_POST_SERVICE` stamped (joey grants it via
  `t_spawn_with_perms`).
- `getsockopt(SO_PEERCRED)` on a server-accepted fd — sub-chunk 12
  marshals to `SYS_srv_peer`.
- Argon2id passphrase derivation — needs heap (mallocng works
  per sub-chunk 7b).
- AEGIS-256 / XChaCha20-Poly1305 / X25519 / ML-KEM-768 — libsodium
  ships in the sysroot per sub-chunk 14; STM_ENABLE_PQ is OFF in
  the cross-build so ML-KEM-768 is excluded (corvus handles PKE).

### Audit posture (16a)

**NOT audit-bearing.**

- No new kernel surface (joey calls existing `t_spawn` +
  `t_wait_pid`).
- No new spawn path (the spawn flow is exercised by every other
  pouch-hello-* probe in joey today).
- No new invariant surface (stratumd is just one more binary).

The audit-bearing work lives in 16b (block device + pool layer +
boot ordering) and 16c (the 9P client connect + the mount + the
chroot — the latter is the boot-ordering invariant from
POUCH-DESIGN.md §14 row 16).

### Cross-references

- `docs/POUCH-DESIGN.md §14 row 16` — scope.
- `docs/reference/85-pouch-stratumd-build.md` — sub-chunk 15
  (the cross-build).
- `docs/reference/78-pouch.md` — the pouch boundary line + the
  syscall seam.
- `~/projects/stratum/v2/src/cmd/stratumd/run.c` — stratumd's
  argv-parsing entry.
- `~/projects/stratum/v2/docs/OS-INTEGRATION.md §4` — the
  Linux-shape boot lifecycle (for reference; Thylacine's variant
  follows the same shape but with joey + corvus + the kernel 9P
  client substituted for systemd + initramfs).
