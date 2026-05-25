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

## 16b-α — argv pass-through kernel surface

**Deliverable**: a new struct-based `SYS_SPAWN_FULL_ARGV = 49` kernel
syscall that delivers argv to spawned children, replacing the legacy
`argv = [name]` default. Pouch's musl `_start` parses the System V
startup frame the kernel built (in the new "Shape B" layout) and
exposes `argc` + `argv` to `main()`. Joey constructs argv =
`["pouch-hello-argv", "alpha", "beta", "gamma"]`; the binary echoes
each back; joey content-checks the round-trip.

### What landed

| Component | Path |
|---|---|
| Kernel ABI: constants | `kernel/include/thylacine/syscall.h::SYS_SPAWN_ARGV_MAX` (16) + `SYS_SPAWN_ARGV_DATA_MAX` (4096). |
| Kernel ABI: struct | `struct sys_spawn_args` — 56 bytes; offset + total-size `_Static_assert`s pin every wire field (`name_va`, `argv_data_va`, `fd_list_va`, `name_len`, `argv_data_len`, `argc`, `fd_count`, `perm_flags`, `_pad_envp`, `cap_mask`). `_pad_envp` is reserved for forward-compat envp pass-through. |
| Kernel ABI: syscall # | `SYS_SPAWN_FULL_ARGV = 49`. |
| Kernel handler | `kernel/syscall.c::sys_spawn_full_argv_handler` — uaccess-copies the struct + sub-buffers; validates every field; routes to internal body. |
| Kernel body | `kernel/syscall.c::sys_spawn_full_argv_with_perms_for_proc` + exported `sys_spawn_full_argv_for_proc` (latter adds console-attached gate for perm_flags). |
| Kernel thunk | `kernel/syscall.c::sys_spawn_full_argv_thunk` — installs fds + applies perms + calls `exec_setup_with_argv` + kfree's argv buffer at the end of the kernel-side path (lifetime ends here). |
| exec layout | `kernel/exec.c::exec_build_init_stack` extended with `argv_data` + `argv_data_len` + `argc` params; supports two shapes (legacy Shape A == argc=0; Shape B == argv-bearing). `exec_setup_with_argv` exported; `exec_setup` wraps with `(NULL, 0, 0)`. |
| exec frame doc | `kernel/include/thylacine/exec.h` — layout block documents both shapes; `EXEC_INIT_STACK_MAX_SIZE` constant bounds Shape B. |
| libt wrapper | `usr/lib/libt/include/thyla/syscall.h::t_spawn_full_argv(&req)` — takes a pointer to `struct t_sys_spawn_args` (mirrors the kernel's `struct sys_spawn_args` 56-byte ABI). |
| Pouch probe | `usr/pouch-hello/pouch-hello-argv.c` — POSIX C `main(argc, argv)` printing each argv string back through stdio. |
| Joey hook | `usr/joey/joey.c::pouch_smoke_one_argv` + invocation in `do_pouch_hello_smoke` with argv `["pouch-hello-argv", "alpha", "beta", "gamma"]`. Content-checks for `pouch-hello-argv: argv[3]=gamma` marker (last argv string, which proves every prior argv pointer was placed correctly). |
| Kernel tests | `kernel/test/test_sys_spawn_full_argv.c` — 8 tests: golden + 7 negative paths (oversize argc, oversize argv_data_len, missing trailing NUL, NUL-count mismatch in both directions, argc>0 with len=0, argc=0 with len>0, Shape-A no-argv parity with SYS_SPAWN_WITH_PERMS). Registered in `kernel/test/test.c`. |
| Test count | 586/586 → **594/594** PASS × default + UBSan, 0 UBSan errors. |

### exec_build_init_stack — Shape A vs Shape B

The kernel-side frame layout is now two-mode:

| | Shape A (no argv, legacy) | Shape B (argv-bearing) |
|---|---|---|
| Selected by | `argc == 0` | `argc > 0` (caller's invariant; defense-in-depth in the function) |
| Frame size | Fixed 144 bytes (`EXEC_INIT_STACK_SIZE`) | Variable, bounded by `EXEC_INIT_STACK_MAX_SIZE` |
| `argc` word | 0 | `argc` |
| argv[] array | One u64 NULL terminator | `argc` u64 ptrs into strings region + NULL terminator |
| envp NULL | u64 = 0 | u64 = 0 |
| auxv | 6 entries × 16 bytes = 96 bytes | Same 6 entries; `AT_RANDOM` user-VA pointer adjusted for variable frame size |
| AT_RANDOM block | At `EXEC_INIT_RANDOM_OFFSET` (sp + 128) | At `random_offset_from_sp` = `(structured_top + 15) & ~15` |
| Strings region | (absent) | argv_data_len bytes immediately after AT_RANDOM; argv[i] pointers into this region |

The legacy Shape A path is preserved bit-identical: existing `exec_setup`
callers continue to land on the exact 144-byte frame they did before
this sub-chunk, with no behavioral change.

### Lifetime + safety invariants

1. **argv buffer copy** — the syscall handler copies argv_data into a
   kernel stack buffer (`SYS_SPAWN_ARGV_DATA_MAX = 4096`) for in-handler
   validation; the body re-copies into a kmalloc'd region BEFORE rfork
   so the user-side buffer is never observed post-rfork. The kmalloc'd
   region is owned by `struct spawn_full_argv_args` and kfree'd by the
   thunk after `exec_setup_with_argv` returns. No path leaks; every
   error path kfree's everything in reverse order.

2. **NUL-termination + count match** — the body verifies the last byte
   of argv_data is `\0` AND the total NUL count equals `argc`. The
   kernel's argv-walk in `exec_build_init_stack` re-verifies the count
   as defense-in-depth (extinction on mismatch — should never trigger
   given the body validated).

3. **No handle smuggling** — argv strings are bytes only. No paths
   read the strings as fd indices or other handles. I-4 + I-5 hold
   structurally.

4. **Bounded kernel-stack usage** — the handler uses a stack buffer of
   exactly `SYS_SPAWN_ARGV_DATA_MAX = 4096` bytes for argv copy.
   Kernel stack budget is well above this.

5. **`_pad_envp` reservation** — the wire field is reserved for the
   future envp pass-through extension. The handler rejects any non-zero
   value at v1.0, so a future kernel that wires `_pad_envp` to
   `envp_data_len` cannot land on a v1.0 caller's request.

### Why no specs/spawn_argv.tla

Spec-to-code is FULLY suspended project-wide as of 2026-05-23 (broadened
from the prior 2026-05-21 narrow lift). Per the suspension policy, this
sub-chunk's invariants are validated by prose reasoning in this
reference + the audit round + the runtime test suite (kernel-side
golden + 7 negative paths; userspace `/pouch-hello-argv` end-to-end
echo). The buggy-cfgs for already-spec'd subsystems remain binding;
this sub-chunk adds none.

### What 16b-α did NOT exercise (deferred to 16b-β / 16c)

- **`CAP_HW_CREATE` spawn**: 16b-α uses `cap_mask = 0` for the joey
  probe; 16b-β grants `CAP_HW_CREATE` to stratumd via the same
  syscall.
- **`SPAWN_PERM_MAY_POST_SERVICE` with argv**: 16b-α uses `perm_flags
  = 0`; 16b-β grants the perm so stratumd can call SYS_POST_SERVICE
  for the FS socket.
- **Stratumd-side virtio-blk driver**: 16b-β's Stratum-branch work.
- **Real pool mount**: 16b-β's joey + Stratum integration.

### Audit close (round 1)

**Posture**: 0 P0 + 0 P1 + 2 P2 + 9 P3 = 11 findings dispositioned.
Clean by severity ceiling (no P0/P1). 599/599 PASS × default + UBSan.

**P2 findings fixed by code**:

- **F1 [P2]**: kernel-side tests bypassed the handler — no in-kernel
  coverage of the uaccess/struct-copy path's distinctive checks
  (`_pad_envp != 0`, `perm_flags & ~ALL`, oversize fields).
  Fixed: extracted `sys_spawn_full_argv_validate_req` as a kernel-
  internal helper called by the handler; added 4 helper-direct tests
  (`validate_req_golden` / `validate_req_rejects_pad_envp` /
  `validate_req_rejects_unknown_perm_bits` / `validate_req_rejects_
  oversize_fields`).
- **F2 [P2]**: `/pouch-hello-argv` did not exercise the
  `argv[argc] == NULL` POSIX guarantee; joey's content-check used a
  single substring (`argv[3]=gamma`) so argv[1..2] pointer errors
  could exit 0 without joey catching them. Fixed: pouch-hello-argv
  now asserts `argv[argc] == NULL` and prints a confirmation marker;
  joey's `pouch_smoke_one_argv` changed to take an array of
  `argv_marker` substrings; call site now checks 6 per-position
  markers (argc=4, argv[0..3], argv[argc] NULL terminator).

**P3 findings fixed by code**:

- **F4 [P3]**: handler did not early-reject `(argc > 0, argv_data_len
  == 0)` at the field-bound stage. Fixed in
  `sys_spawn_full_argv_validate_req`.
- **F5 [P3]**: thunk comment referred to the wrong function name
  (`sys_spawn_with_perms_for_proc` instead of
  `sys_spawn_full_argv_for_proc`). Fixed inline.
- **F6 [P3]**: SYS_SPAWN_FULL_ARGV header docs omitted `_pad_envp !=
  0` rejection AND the argc⇔argv_data_len symmetry rejection as
  documented failure modes. Fixed inline.
- **F7 [P3]**: header `req_va` doc said "must be aligned" — kernel
  reads byte-by-byte; alignment is irrelevant. Fixed inline.
- **F8 [P3]**: libt `struct t_sys_spawn_args` mirror lacked per-field
  offsetof asserts (only size pinned). Fixed: added 10 offsetof
  asserts mirroring the kernel struct.
- **F11 [P3]**: no test for SYS_SPAWN_FULL_ARGV's own copy of the
  console-attached gate. Fixed: added
  `rejects_non_console_attached_perm_flags` (uses `proc_alloc()` to
  make a fresh non-attached Proc, independent of kproc's flag state).

**P3 findings deferred**:

- **F3 [P3]**: handle_alloc fd>i defense — pre-existing pattern
  copied from `sys_spawn_with_fds_thunk`. Same latent bug exists in
  every spawn thunk. Defer to a separate hygiene chunk that fixes
  both at once (rather than landing an asymmetric fix here that
  improves the new code's defense without touching the older
  identical code).
- **F9 [P3]**: dead `installed` counter — pre-existing pattern,
  same deferral as F3.
- **F10 [P3]**: `next_start` local inlinable — trivial readability
  cosmetic; defer to a future hygiene pass.

**Test count delta**: 586/586 → 594/594 (initial 16b-α) → **599/599**
(R1 close: +5 tests for F1 helper coverage + F11 gate). 0 UBSan
runtime errors on default + UBSan builds.

---

## 16b-β — stratumd virtio-blk driver arm (foundation)

**Status**: foundation landed; end-to-end pool mount **deferred to a
follow-up sub-chunk (16b-γ)** because two pouch-side v1.x lifts surface
late in the path:
  1. pouch `fstat()` is still `__NR_fstat = 0xFFFF` (the ENOSYS
     sentinel). `stm_keyfile_load` in stratum/v2/src/keyfile/keyfile.c
     calls `fstat(fd)` after `open(path, O_RDONLY)` to validate the
     keyfile size matches the expected layout; without fstat the load
     returns STM_EBACKEND/STM_ENOENT.
  2. `tools/mkcpio.py` builds the ramfs cpio with FLAT layout only
     ("Subdirectories are NOT recursed at v1.0"). So even with the
     0009-pouch-openat.patch landed here, `/etc/stratum/system.key`
     can't actually exist in the ramfs; the path-walk for the
     intermediate `etc/` and `stratum/` directories returns Twalk
     Rerror on the first hop.

Lifting EITHER of these without the other doesn't unblock the mount.
The 16b-β foundation that did land in this sub-chunk is everything
*upstream* of those two lifts; 16b-γ will land the lifts + close the
end-to-end mount + the audit-bearing close.

### What 16b-β foundation landed

**Stratum-side (on the `thylacine-pouch-arm` branch in
`~/projects/stratum/v2`)** — port of `usr/virtio-blk-rw/src/main.rs`
(Rust, 814 LOC) into a long-lived `stm_bdev` arm:

- `src/block/bdev_thylacine.c` (new, ~610 LOC). The synchronous
  `stm_bdev_ops` vtable implemented over Thylacine's HW-capability
  syscalls. VirtIO 1.2 §3.1.1 init state machine (RESET → ACK →
  DRIVER → DeviceFeatures readback → DriverFeatures → FEATURES_OK
  readback → DRIVER_OK); split-virtqueue setup with one chained
  3-descriptor entry (req-header → data → status) reused per
  request; IRQ-driven completion with bounded spurious-wake
  tolerance (MAX_NON_USED_BUFFER_WAKES = 16 per VIRTIO 1.2 §4.2.5);
  per-bdev pthread_mutex serializing all I/O (one request in flight
  per bdev at v1.0); 1 MiB data DMA + 4 KiB ring DMA; cap_sectors
  read from VirtIO blk config-space §5.2.4 byte offset 0..7; HIGH-
  to-LOW slot scan so the FIRST-listed virtio-blk device (the pool)
  takes priority over the SECOND (the legacy test disk.img that the
  virtio-blk-probe / virtio-blk-rw binaries scan LOW-to-HIGH for).
- `src/block/bdev.c`: STM_BDEV_BACKEND_AUTO routes to THYLACINE when
  `__thylacine__` is defined; switch case added; explicit case for
  STM_BDEV_BACKEND_POSIX gated `!__thylacine__` so a caller passing
  POSIX-by-name gets STM_ENOTSUPPORTED rather than runtime ENOSYS.
- `src/block/bdev_internal.h`: forward decl `stm_bdev_open_thylacine`
  gated on `__thylacine__`.
- `include/stratum/block.h`: new enum value `STM_BDEV_BACKEND_THYLACINE = 3`.
- `src/block/CMakeLists.txt`: thylacine arm REPLACES posix.c when
  STM_PLATFORM_THYLACINE (posix.c uses pread/pwrite/fsync which are
  0xFFFF in pouch musl; the thylacine arm uses virtio-mmio directly
  and is the only safe path on Thylacine).

**Thylacine-side**:

- `usr/lib/pouch/patches/0008-pouch-hw-syscalls.patch`: exposes
  `__NR_mmio_create=2`, `__NR_irq_create=3`, `__NR_irq_wait=4`,
  `__NR_mmio_map=5`, `__NR_dma_create=6`, `__NR_dma_map=7` at the
  pouch musl ABI surface. Six numbers; one file (`bits/syscall.h.in`).
  The pouch arm's `bdev_thylacine.c` consumes these via raw
  `syscall(SYS_mmio_create, ...)` etc.
- `usr/lib/pouch/patches/0009-pouch-openat.patch`: rewrites musl's
  `src/fcntl/openat.c` to walk absolute paths one component at a
  time via `SYS_walk_open` (kernel #34). Lifts a v1.x deferral
  from project memory. **Necessary but not sufficient** for the
  stratumd keyfile load (see 16b-γ deferred items above).
- `tools/build.sh::build_stratum_mkfs_host`: native host build of
  `stratum-mkfs` (NOT the pouch cross-build). Used at build time
  to pre-generate the boot system pool fixture.
- `tools/build.sh::build_stratum_pool_fixture`: runs the host
  `stratum-mkfs` against `build/fixtures/pool.img` (64 MiB) +
  `build/fixtures/system.key`. Regenerated each `tools/build.sh
  kernel` run for reproducibility.
- `tools/build.sh::build_ramfs`: copies `build/fixtures/system.key`
  to the ramfs at `etc/stratum/system.key` (per K1 initramfs-literal
  boot key, scripture commit `e82e945`). NOTE: at v1.0 mkcpio.py
  is flat-only; the subdirectory copy is a no-op until 16b-γ lifts
  multi-component devramfs walk.
- `tools/run-vm.sh`: new `pool_flags` array adding a second
  `virtio-blk-device` for `build/fixtures/pool.img`. Listed BEFORE
  `disk_flags` so the pool gets slot 31 (HIGH) and disk.img stays
  at slot 30 (LOW). Scan direction comment block expanded.
- `usr/joey/joey.c::do_joey_main`: stratumd-boot probe replaces the
  16a load-only probe. Constructs argv `["stratumd", "/dev/virtio-blk",
  "--listen", "/srv/stratum-fs", "--keyfile", "/etc/stratum/system.key"]`,
  passes cap_mask=`T_CAP_HW_CREATE`, perm_flags=`T_SPAWN_PERM_MAY_POST_SERVICE`,
  fd_count=3 with the pipe-wr as fd 0/1/2 (so stratumd's stderr is
  captured into the boot log). Bounded `t_srv_connect("stratum-fs")`
  retry loop (600 attempts × ~1ms each = ~600ms total wait). **Probe
  is non-fatal** at this sub-chunk: failure to bind the /srv socket
  is reported with diagnostic + child stderr drain, but boot
  continues. Once 16b-γ lifts the remaining pouch surface (fstat +
  devramfs subdir walk), the probe will become fatal-on-failure +
  audit-bearing.

### Verified end-to-end up to (but not including) keyfile load

Boot output captured in `build/test-boot.log`:

```
joey: stratumd-boot child exit_status=1 — output:
stratumd: serving /dev/virtio-blk on /srv/stratum-fs (backlog=16, msize=8388608, ds=1, ro=0)
stratumd: run failed (rc=-2)
```

The "serving" line confirms:
- stratumd spawned with the foundation argv successfully (16b-α
  argv pass-through verified at runtime).
- stratumd's main reached its CLI-argv parser and produced the
  expected serving line with `fs_path="/dev/virtio-blk"`,
  `socket_path="/srv/stratum-fs"`.
- stratumd's `install_signal_handlers()` completed (pouch sigaction
  arm from sub-chunk 13b ran clean on the {SIGINT, SIGTERM, SIGPIPE}
  set).
- stderr propagation works: stratumd's fd 1 + 2 are inherited from
  joey's pipe and drained into the boot log.

The `rc=-2` (= STM_ENOENT) from `stm_stratumd_run` is the keyfile-load
failure described in the 16b-γ deferred items above. Foundation works;
the lift items gate the final mount.

### Deferred to 16b-γ (mount completion + audit)

- pouch `fstat()` implementation (likely via a new SYS_FSTAT kernel
  syscall + a pouch arm).
- `tools/mkcpio.py` subdirectory recursion + devramfs nested-dir
  walk support (kernel/devramfs.c currently maintains a flat file
  table).
- End-to-end pool mount: `stm_keyfile_load` succeeds → `stm_stratumd_run`
  reaches `stm_fs_mount` → bdev_thylacine reads pool header → AEAD
  decrypt → root dataset visible → listen socket bound on /srv/stratum-fs.
- Joey's probe becomes fatal-on-failure.
- Formal audit prosecutor round on the audit-trigger surfaces
  ("stratumd HW-cap spawn", "stratumd virtio-blk driver arm") added
  to CLAUDE.md in `e82e945`.

### Architecture invariants confirmed by foundation work

- **I-2 (cap monotonic reduction)**: joey holds CAP_HW_CREATE from
  kproc inheritance; passes `cap_mask = T_CAP_HW_CREATE` to
  SYS_SPAWN_FULL_ARGV; `rfork_with_caps` narrows the child's caps to
  the intersection. No cap expansion observed.
- **I-5 (hardware-handle non-transferability)**: stratumd's
  KOBJ_MMIO / KOBJ_IRQ / KOBJ_DMA handles never appear in a 9P
  transfer (the surface doesn't even exist). Static.
- **CAP_HW_CREATE allocation honest**: stratumd actually drives
  hardware (when 16b-γ lands), so the cap is the right authority
  for the role. Not privilege creep.
- **Plan-9-canonical**: the FS server owns its disk. Validated by
  the design + foundation.

### Files added (16b-β foundation)

| File | LOC | Owner |
|---|---|---|
| `src/block/bdev_thylacine.c` (Stratum) | ~610 | Stratum thylacine-pouch-arm branch |
| `usr/lib/pouch/patches/0008-pouch-hw-syscalls.patch` | 100 | Thylacine |
| `usr/lib/pouch/patches/0009-pouch-openat.patch` | 200 | Thylacine |

Plus edits to: `include/stratum/block.h`, `src/block/bdev.c`,
`src/block/bdev_internal.h`, `src/block/CMakeLists.txt` (all Stratum);
`tools/build.sh`, `tools/run-vm.sh`, `usr/joey/joey.c`,
`usr/lib/pouch/patches/series` (all Thylacine).

---

## 16b-γ-mount-close — bdev_thylacine read-I/O fix + pouch abort override (this checkpoint)

**Deliverable**: bdev_thylacine's read I/O actually returns pool data.
The stratumd mount path runs end-to-end through `stm_sb_mount_scan`
(decodes the uberblock at pool.img label-0 slot-4, STRATUM2 magic at
offset 0x4000); proceeds into the downstream pool_open / alloc_open /
sync_create chain. A separate downstream failure surfaces as a clean
`exit-1` from `stratumd: run failed (rc=-N)` (NOT in this sub-chunk's
scope -- the joey probe stays NON-FATAL pending the next sub-chunk
to investigate / fix the post-mount_scan failure).

Sub-chunked in-session at 16b-γ-syscalls + 16b-γ-mount-close because
the read-I/O block was a separate problem from the POSIX-surface
lifts (fstat / lseek / open redirect / walk fixes). 16b-γ-syscalls
landed at `af6fd82` + `416b4f3`; 16b-γ-mount-close lands here.

### The bug

`Stratum/src/block/bdev_thylacine.c` defined its kobj-rights mirror as:

```c
#define T_RIGHT_READ                 (1u << 0)
#define T_RIGHT_WRITE                (1u << 1)
#define T_RIGHT_MAP                  (1u << 2)
#define T_RIGHT_SIGNAL               (1u << 3)   /* WRONG */
```

The kernel's `RIGHT_SIGNAL` per `kernel/include/thylacine/handle.h`
is `(1u << 5)`. The bit at `(1u << 3)` is `RIGHT_TRANSFER` (used
for the 9P-mediated handle transfer surface; not by HW handles).

What this caused: `t_irq_create(intid, T_RIGHT_SIGNAL)` succeeded
(the rights mask `(1u << 3)` is within `RIGHT_ALL = 0x3f`, the
kernel's `rights == 0 || rights & ~RIGHT_ALL` reject doesn't fire).
The handle was created with `rights = (1u << 3)`. Then every
`t_irq_wait(d->irq_handle)` returned -1 because
`sys_irq_wait_handler` gates on `slot->rights & RIGHT_SIGNAL` =
`(1u<<3) & (1u<<5)` = `0`. `do_request`'s `if (count < 0) return
STM_EIO;` collapsed every read to EIO. `stm_sb_mount_scan` saw
every label_read fail and returned STM_ENOENT.

The Rust `usr/virtio-blk-rw` driver was unaffected because it uses
`libthyla_rs::T_RIGHT_SIGNAL = 1 << 5` (correct). The Rust driver
has been passing for many sub-chunks without surfacing this bug,
because nothing else in the tree references `bdev_thylacine.c`'s
private `T_RIGHT_SIGNAL` constant.

The fix is the one-line update of the C driver's constant +
a comment that explicitly calls out the kernel's bit-layout
authority and the adjacent reserved bits `(1u << 3) =
RIGHT_TRANSFER` + `(1u << 4) = RIGHT_DMA`.

### How the bug was diagnosed

Diagnostic fprintfs were temporarily added to `bdev_thylacine.c`'s
`op_read` / `do_request` / IRQ-wait loop, and a temporary kernel-
side EL0 fault diagnostic was added to `arch/arm64/exception.c`
showing the faulting Proc's PID + ELR (the faulting PC) + LR + SP.
The traces showed `t_irq_wait` returning -1 every call. Comparing
`T_RIGHT_SIGNAL` across the three rights-mirror sites
(`bdev_thylacine.c`, `libthyla_rs/src/lib.rs`,
`usr/lib/libt/include/thyla/syscall.h`) found the lone outlier.

All diagnostic fprintfs were reverted before the close commit per
the audit-close discipline. The kernel-side EL0 fault diagnostic
was also reverted.

### Pouch abort override (the v1.x extension, landed early)

The downstream stratumd mount path can call `abort()` (via
`assert()` or direct call). Upstream musl's `abort.c` reaches
`a_crash()` -- a deliberate NULL deref -- as a "last-resort kill
the process" mechanism after a misbehaved SIGABRT handler returns.
At Thylacine v1.0, FAULT_UNHANDLED_USER extincts the kernel
rather than terminating the offending Proc, so a_crash() takes
the whole boot down.

`usr/lib/pouch/patches/0011-pouch-abort.patch` (NEW, applied as
the eleventh entry in the boundary-line patch series) overrides
musl's `abort()` to call `_Exit(127)` directly. Skips the
`raise(SIGABRT) + a_crash()` chain. Programs that install
SIGABRT handlers will not see them invoked under this path.

Status code 127 matches musl's normal-termination fallback at
the bottom of upstream `abort()` (the `_Exit(127)` past the
unreachable `a_crash`). A parent doing `wait_pid` will observe
`WIFEXITED + WEXITSTATUS == 127` instead of the
`WIFSIGNALED + WTERMSIG == SIGABRT` it would observe on Linux.
v1.x will add a sigaction(SIGABRT) surface that distinguishes
the two; the docs cross-reference is in
`docs/reference/83-pouch-signals.md`.

The current downstream failure in stratumd's mount path actually
exits via run.c's `if (rc != STM_OK) return 1;` path (exit_status
1, not 127), so the abort override is not exercised in the
happy-path of this sub-chunk's test. The override lands as a
defensive measure for future pouch programs that DO assert/abort
on the mount path.

### Joey reap workaround

The original `joey/joey.c` failure-branch `t_wait_pid` blocked
unconditionally, which deadlocked if stratumd was alive doing
slow mount work. The workaround restructures the retry loop +
failure branch:

- **6000 retries** (up from 600) of `t_srv_connect(srv_name, ...)`.
  Gives stratumd time to complete mount.
- **Per-retry non-blocking pipe drain**: each retry calls
  `t_poll(sd_rd, timeout=0)` and reads available bytes into the
  boot log via `t_puts`. Keeps the pipe buffer from filling and
  blocking stratumd's stderr writes.
- **Failure-branch bounded drain**: 200 iter x 10 ms poll to
  capture any final diagnostic msg before reap.
- **Failure-branch wait_pid**: after the drain, reap stratumd
  to prevent the unreparented-zombie / kproc-wait_pid `wrong pid`
  extinction race. The race: if stratumd exits before
  joey-userspace, the zombie reparents to kproc on joey-userspace's
  exit; kproc-joey_run's `wait_pid` is unfiltered (returns any
  zombie child) so it can return stratumd's pid instead of
  joey-userspace's pid, triggering the `wrong pid` extinction in
  `kernel/joey.c:204`. With this reap in place, stratumd's zombie
  is collected by joey-userspace before joey-userspace exits.

The 6000-retry + per-retry drain pair are coupled: without the
drain, stratumd's stderr writes would block once the pipe buffer
fills (typically after a few hundred bytes); without the bumped
retry count, joey times out before mount completes.

This is the v1.0 stratumd-boot probe shape. The probe will be
revisited when the joey-probe-flips-FATAL flip lands (queued for
the next sub-chunk after the downstream mount path is fixed).

### What works end-to-end this checkpoint

Boot log confirms (after RIGHT_SIGNAL fix):
- `joey: probe /system.key fstat OK size=3656 mode=0o100644`
- `joey: probe /system.key lseek SEEK_END/SEEK_SET OK`
- `stratumd: serving /dev/virtio-blk on /srv/stratum-fs (...)` -- stratumd
  reached `stm_stratumd_run`, ran libsodium init, opened bdev, etc.
- (NEW WITH FIX) `stm_keyfile_load("/system.key")` -> STM_OK
- (NEW WITH FIX) `stm_bdev_open(THYLACINE)` -> STM_OK +
  capacity = 64 MiB
- (NEW WITH FIX) `stm_sb_mount_scan` -> STM_OK (decodes uberblock
  at label-0 slot-4 -- the STRATUM2 magic at offset 0x4000)
- Mount path proceeds into pool_open / alloc_open / sync_create
- A downstream failure (NOT in this sub-chunk's scope) terminates
  the run with `exit-1`
- `joey: stratumd-boot /srv/stratum-fs not bound (...)` -- expected
  pending the next sub-chunk
- `joey: stratumd-boot child exit_status=1 (final drain)` -- reaped
  cleanly
- Boot continues to stub-bringup, joey exits cleanly, kernel reports
  `Thylacine boot OK`

Test posture: `599/599 PASS` x (default + UBSan). No EXTINCTION.

### Audit-trigger surfaces

This sub-chunk's audit covers:

| Surface | New / Existing |
|---|---|
| `usr/lib/pouch/patches/0011-pouch-abort.patch` (new) | NEW row |
| Stratum-side `src/block/bdev_thylacine.c` -- RIGHT_SIGNAL fix | Existing row "stratumd virtio-blk driver arm" |
| `usr/joey/joey.c` retry+drain+reap workaround | Existing "Initial bringup" / boot-ordering surface |
| **paired with 16b-γ-syscalls surfaces** (carried over from `af6fd82`/`416b4f3`): | |
| `kernel/syscall.c::sys_fstat_handler` / `sys_lseek_handler` / partial-walk reject | Existing row "native fstat + lseek surface" |
| `kernel/devramfs.c::devramfs_walk` reuse-nc + `devramfs_stat_native` | Existing row "native fstat + lseek surface" |
| `kernel/joey.c::joey_run` kproc territory chroot | Existing row "Initial bringup" |

### Files added / modified (16b-γ-mount-close)

| File | LOC delta | Owner |
|---|---|---|
| `src/block/bdev_thylacine.c` (Stratum) | +4 -2 | Stratum `thylacine-pouch-arm` branch |
| `usr/lib/pouch/patches/0011-pouch-abort.patch` (Thylacine, NEW) | +52 | Thylacine |
| `usr/lib/pouch/patches/series` | +1 | Thylacine |
| `usr/joey/joey.c` | retry+drain+reap | Thylacine |
| `docs/reference/86-pouch-stratumd-boot.md` (this file) | +section | Thylacine |
| `CLAUDE.md` audit-trigger row update | small | Thylacine |
| `docs/phase6-status.md` landed row | small | Thylacine |

Stratum side: `Thylacine bdev arm: fix T_RIGHT_SIGNAL constant ...`
committed on `thylacine-pouch-arm` at `389c742` (parent `63a3eb1`).
User pushes the Stratum branch; the Stratum agent merges
`thylacine-pouch-arm` forward to main at its discretion.

### What's queued for the next sub-chunk (post-16b-γ-mount-close)

- Investigate the downstream stratumd mount-path failure. Hypotheses:
  pool_open / alloc_open_blank / sync_create may hit a Stratum-internal
  assert + return non-OK; OR an unexpected encrypted-vs-plaintext path
  mismatch (the bootstrap pool may have been formatted with an
  encryption envelope that this stratumd build doesn't decrypt
  cleanly); OR a missing pouch-side syscall surface (getuid, geteuid,
  fchmod, fchown etc) the mount path needs.
- After fix: stratumd binds /srv/stratum-fs; joey's probe flips to
  FATAL on miss.
- This work expands the boot-path test coverage so additional
  audit-trigger surfaces may need rows.

---

## 16b-γ-mount-bind diagnostic findings (post-16b-γ-mount-close)

**Status**: investigation complete; no code change shipped (the
candidate fix degrades the failure mode from clean-reap to kernel
extinction). Carried forward as v1.x lifts.

### Investigation method

Temporary `fprintf` tracers were inserted into stratumd's mount path
(non-committed):

- `stratumd/src/cmd/stratumd/run.c::stm_cmd_stratumd_main` — entry +
  pre-/post-`stm_stratumd_run`.
- `stratumd/src/cmd/stratumd/serve.c::stm_stratumd_run` — entry +
  pre-/post-`stm_fs_mount` + `listen_unix` + `accept_loop`.
- `stratumd/src/fs/fs.c::stm_fs_mount` — entry + ebr_init +
  keyfile_load + bdev_open + sb_mount_scan + pool_open +
  alloc_open_blank + sync_open.
- `stratumd/src/alloc/alloc.c::open_handle_bare` + `alloc_new` —
  bootstrap_open + bootstrap_stats_get + compute_data_area +
  crypto_init + calloc + mutex_init + btree_mt_new.
- `stratumd/src/sync/sync.c::stm_sync_open` — entry + crypto_init +
  scan loop + compute_auth_gen + content-quorum + pre-/post-
  `free(scans)` + ub validation + pre-/post-`sync_new`.
- `arch/arm64/exception.c::exception_sync_lower_el` — dump
  `ctx->elr` (faulting PC) + `ctx->regs[30]` (LR) + `ctx->sp` (SP)
  on FAULT_UNHANDLED_USER, via `uart_puts` + `uart_puthex64`.

The full diagnostic chain was reproduced; the tracers were reverted
in this commit (kept only in the memory file for future reproduction
guidance).

### Finding 1 — CAP_CSPRNG_READ missing for stratumd

stratumd is spawned by joey with `cap_mask = T_CAP_HW_CREATE` only
(`usr/joey/joey.c` builder for the 16b-γ probe). libsodium's
`sodium_init` (called transitively from `stm_alloc_open_blank ->
alloc_new -> stm_crypto_init`) dispatches `randombytes_stir ->
randombytes_sysrandom_init ->`
`randombytes_linux_getrandom(fodder, 16)` which compiles to
`getrandom(buf, 16, 0)` -> `syscall_cp(SYS_getrandom, buf, 16, 0)`.

`kernel/syscall.c::sys_getrandom_handler` gates on
`(p->caps & CAP_CSPRNG_READ) == 0 -> return -1`. With only
T_CAP_HW_CREATE in the mask, the gate fires; pouch's
`__syscall_ret` maps the kernel -1 to errno EIO. libsodium's loop
only retries on EINTR/EAGAIN, so EIO falls through to
`random_dev_open()` (Linux fallback) which fails (no `/dev/urandom`
on Thylacine) and `sodium_misuse()` runs `raise(SIGSEGV) -> abort()
-> _Exit(127)` (via pouch's 0011-pouch-abort.patch).

The kernel collapses every non-zero exit status to 1 (per the
`SYS_EXITS` two-state convention in `kernel/syscall.c`'s
`sys_exits_handler`), so joey's `t_wait_pid` sees `exit_status=1`.
**Joey reaps cleanly; boot is green.**

**Candidate fix** (NOT shipped): grant `T_CAP_CSPRNG_READ` in the
spawn mask:
```c
.cap_mask = T_CAP_HW_CREATE | T_CAP_CSPRNG_READ,
```
With the fix, stratumd advances past `sodium_init` and reaches
Finding 2.

### Finding 2 — mallocng heap corruption in `stm_sync_open`

With CAP_CSPRNG_READ granted, stratumd reaches `stm_sync_open` and
runs through:

- `stm_crypto_init` — OK (sodium_init succeeds; getrandom seeds the
  CSPRNG).
- scan loop (`n=1`, `quorum=1`): one device; `stm_sb_mount_scan` on
  bdev_thylacine returns OK with label=2, slot=6 — the canonical UB
  decodes cleanly.
- `compute_auth_gen` returns `auth_gen=6`.
- content-quorum check (single device; canonical_idx=0).
- `free(scans)` succeeds (4104-byte allocation; mallocng accepts the
  free without complaint).
- ub validation: `ub_device_count=1`, `ub_device_id=0`,
  `ub_roster_hash=16759053560515923264`, matches pool — passes.
- `sync_new(p, a)` allocates s2=0x100004060 — OK.

The next `__libc_free` (caller not pinned by the tracers; likely a
sync_open downstream alloc/free pair) trips mallocng's
`assert(!end[-5])` in `get_nominal_size`. The exact assert can vary
between runs but the pattern is consistent: a slot-footer sentinel
byte is non-zero when mallocng expects zero.

Pouch's mallocng `glue.h` maps the `assert` macro to `a_crash()` =
`*(volatile char *)0 = 0` (deliberate NULL deref). The fault hits
EL0 with FAR=0x0 and EC=DATA_ABORT_LOWER; arch_fault_handle returns
FAULT_UNHANDLED_USER (no VMA at 0x0); `exception_sync_lower_el`
extincts the kernel.

**Faulting PC** (from the temporary kernel-side tracer):
`0x2a602c` in `__libc_free` — the `mov x11, xzr; strb wzr, [x11]`
pair that implements mallocng's a_crash inline.

**Root cause hypotheses** (not yet narrowed):

1. **scans[i].ub overrun**: `stm_sb_mount_scan` writes to
   `&scans[i].ub` (a 4096-byte stm_uberblock). If the write overruns
   the allocated slot (e.g., off-by-one on a multi-block read), it
   stomps mallocng's inline footer. But scans was freed cleanly
   before the crash, so the corruption is unlikely to be in scans
   itself.
2. **libsodium's `_sodium_alloc_init` allocator hooks**: sodium
   replaces or interposes on malloc/free for its protected
   allocations. Maybe `_sodium_alloc_init` (called from sodium_init)
   sets up state that mismatches mallocng's expectations.
3. **sync_new internal allocation pattern**: sync_new probably
   allocates a substantial stm_sync struct (~few KB); the
   allocation pattern (multiple small allocs followed by frees in
   a non-LIFO order) might trigger a mallocng-specific bug.
4. **General pouch mallocng v1.0 stability**: pouch-hello-malloc
   works for small alloc/free pairs, but the heavier stratumd
   workload (libsodium + Stratum's stm_sync_open allocator dance)
   exposes a latent issue.

The candidate root-cause-narrow procedure would be:

1. Add `malloc_check_assertions` mode to pouch's mallocng build (no
   such mode upstream; would require patching mallocng to dump the
   suspect canary value + allocation history).
2. Or use a per-malloc tracer to identify which alloc caused the
   subsequent free to fail.
3. Or build with `-fsanitize=address` (would require ASan support
   in pouch + the kernel; not v1.0).

### Finding 3 — kernel FAULT_UNHANDLED_USER policy is too strict

The kernel extincts on any EL0 fault that demand-paging can't
resolve (`arch/arm64/exception.c::exception_sync_lower_el ->
FAULT_UNHANDLED_USER -> extinction_with_addr`). This is the
documented v1.0 behavior; v1.x (Phase 5+ note delivery) routes
SIGSEGV-like notes instead, letting the offending Proc terminate
without taking the kernel down.

This finding interacts with Finding 2: a v1.0 mallocng corruption
that triggers `a_crash` would terminate stratumd cleanly under
v1.x but extincts the kernel under v1.0. The pre-existing pouch
`abort()` override (0011-pouch-abort.patch) is the analogous
boundary-line fix for `raise(SIGABRT) -> abort -> a_crash`; an
analogous `0012-pouch-mallocng-crash.patch` would patch mallocng's
`glue.h::assert` macro to `_Exit(127)` instead of `a_crash()`.

### Why the candidate fix is net-negative

Combining Findings 1 + 2 + 3: granting CAP_CSPRNG_READ to stratumd
lets it past sodium_init (Finding 1 cleared) but exposes mallocng
corruption (Finding 2) which then escalates to kernel extinction
(Finding 3). The boot test regresses from `Thylacine boot OK` to
`EXTINCTION: EL0 fault`.

So at this checkpoint we ship the pre-fix state: stratumd dies
clean at sodium_init via `_Exit(127)`; joey reaps cleanly; boot is
green; joey probe stays NON-FATAL.

### Carry-forwards to v1.x (queued)

| # | v1.x lift | Closes |
|---|---|---|
| (a) | Grant `T_CAP_CSPRNG_READ` to stratumd (one-line) | Finding 1 |
| (b) | Investigate + fix mallocng heap corruption in stm_sync_open | Finding 2 root cause |
| (c) | `0012-pouch-mallocng-crash.patch` — patch mallocng's `glue.h::assert` to use `_Exit(127)` instead of `a_crash()` (parallels 0011-pouch-abort.patch) | Finding 3 (partial; via boundary-line) |
| (d) | Phase 5+ SIGSEGV-like note delivery in `exception_sync_lower_el` | Finding 3 (real fix) |

(a) MUST be paired with (b) and/or (c) before landing; landing (a)
alone would regress the boot. (b) is the real fix; (c) is the
boundary-line analog to the abort override. (d) is the architectural
fix that makes (c) unnecessary.

### What 16b-γ-mount-bind did NOT change

- No kernel code change.
- No Stratum-side change.
- No pouch patch added/removed.
- No joey probe change.
- Test suite: 609/609 PASS x default + UBSan (unchanged from
  16b-γ-mount-close audit close).
- Boot: `Thylacine boot OK` reproducible; joey's stratumd-boot
  probe `child exit_status=1 (final drain)` reproducible.

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
