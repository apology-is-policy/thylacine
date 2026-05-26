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

### The 16c live-medium + host-bake + pivot design (chosen 2026-05-26)

The headline Phase 6 deliverable is "Thylacine boots from a disk-backed
Stratum FS over real 9P". Surfacing sub-chunk 16c's implementation
forced a question that the original POUCH-DESIGN row 16 had not
addressed: **after joey pivots its territory root to stratumd's mounted
FS, where do future spawn paths resolve from?**

Joey spawns several binaries from `/sbin/...` and `/pouch-hello-*`
during bringup — corvus, stratumd, the pouch hellos, the thread probe.
All of those resolve through the territory's root_spoor at call time.
If joey pivots root to stratumd's tree, any post-pivot spawn fails
because stratumd's tree (a freshly mkfs'd Stratum pool) doesn't contain
those binaries. The binary-corpus problem is structural to the pivot.

#### The architectural reference: every real OS

The pattern every Linux distro, BSD, illumos, macOS, Windows uses is
the **live-medium → installer → boot-from-installed-disk** loop:

1. The bootable medium (CD/USB ISO; or for QEMU, a `-kernel + -initrd`
   pair) contains a complete usable userland — the kernel, an init, a
   set of binaries enough to run.
2. From that live system, an installer (Anaconda, Calamares,
   debian-installer, the FreeBSD/OpenBSD installers, ...) formats the
   target disk, copies the binary corpus from the live medium onto it,
   installs a bootloader, marks the disk bootable.
3. The system reboots, the firmware loads from the installed disk, the
   installed root has everything the OS needs to run continuously.

For Thylacine the mapping is:

| Real distro thing | Thylacine equivalent |
|---|---|
| Live ISO bootable medium | `disk.img` cpio → kernel devramfs |
| Live ISO's kernel | `build/kernel.elf` |
| Live ISO's initramfs / squashfs | The cpio's `/sbin/*` + `/pouch-hello-*` + `/joey` |
| Installer binary | `stratum-mkfs --populate-from <dir>` (host build infra at v1.0) |
| Installed FS (post-install root) | The populated `pool.img` |
| Installed bootloader | N/A — QEMU `-kernel` loads our kernel directly |
| Boot from installed disk | joey: mount stratumd's FS → pivot root → continue |

The binary-corpus problem evaporates because the installer (host-side
at v1.0, runtime at v1.1) PUT the binaries on the pool. Joey's pivot
lands on a root that already has `/sbin/corvus` etc.

#### Why host-bake at build time for v1.0

Three viable options for the installer:

1. **Host-bake (v1.0 expedient)**. Orchestrate the already-shipping
   Stratum v2 tools from `tools/build.sh::build_stratum_pool_fixture`:
   `stratum-mkfs` (already wired) creates the empty pool; `stratumd`
   is started in the background on a temp Unix socket; `stratum-fs
   write` (the audited 9P-CLI client subcommand — `lcreate + buffered
   Twrite + auto-fsync`) copies each boot-corpus file under its
   target path; stratumd is shut down. pool.img ships pre-populated.
   Joey's bringup ends with mount + pivot. The "installer" exists
   conceptually at host build time, implemented as shell orchestration
   of already-audited Stratum binaries — **no new Stratum-side code**.
2. **Runtime installer (v1.1 lift)**. Build a Thylacine `/sbin/installer`
   binary that runs as part of an alternate boot path (joey takes an
   arg, or there's a second init binary). It iterates devramfs, opens
   stratumd's mount, calls Twrite/Tcreate via the kernel 9P client to
   copy each file. Proves the runtime install loop end-to-end.
   Exercises the full 9P client write surface (Twrite/Tcreate/Tmkdir)
   not just read.
3. **No pivot (v1.x deferral)**. 16c does mount + walk + read against
   stratumd. The pivot is deferred indefinitely. Disk-backed FS is
   reachable as a mount subtree but `/` stays devramfs. Sacrifices
   the headline.

**Choice: 1 (host-bake) for v1.0.** Rationale:

- **Significantly less code than 2 — in fact, zero new Stratum-side
  code.** Stratum v2 already ships `stratum-fs` (the 9P-CLI client
  with `write`/`mkdir`/`create`/`sync` subcommands), so host-bake is
  pure shell orchestration of existing audited binaries in
  `tools/build.sh`. No `--populate-from` flag, no Stratum-side patch;
  the populate exercises the same audited Twrite/Tcreate code paths
  the runtime mount will use.
- **Same end-state as 2 for v1.0 boot.** Both pre-populate pool.img with
  the same corpus; the difference is *when* the populate runs.
- **2 stays on the v1.1 plan** as the architecturally complete answer.
  When v1.1 lifts it, the kernel surfaces 16c lands (9p_srvconn
  transport + SYS_ATTACH_9P_SRV + SYS_PIVOT_ROOT) are exactly what
  the runtime installer also consumes — no design rework, just code
  added.
- **3 fails to deliver the headline.** A Phase 6 that doesn't pivot is
  honest only if reframed as "mounts a disk-backed Stratum FS during
  boot." The honest framing has merit but doesn't realize the Phase 6
  ambition.

#### Why joey pivots LAST

Joey is the long-running init Proc. Its bringup is one-shot:

1. Spawn corvus (resolved via devramfs `/sbin/corvus`)
2. Run corvus probes (no further spawns)
3. Spawn stratumd (resolved via devramfs `/sbin/stratumd`) with
   `CAP_HW_CREATE`
4. Wait for stratumd's `/srv/stratum-fs` listener via t_srv_connect
5. Run all pouch-hello probes (each spawns via devramfs; each runs to
   completion + reaps before the next)
6. `t_srv_connect("stratum-fs")` → byte-mode SrvConn handle
7. `t_attach_9p_srv(srv_fd, "/")` → dev9p root Spoor handle
8. `t_mount(root_fd, /* abstract path id */)` → mount in joey's territory
9. **`t_pivot_root(root_fd)`** → joey's root_spoor swaps from devramfs
   to stratumd's tree; old devramfs root_spoor ref drops
10. Optional: post-pivot `t_walk_open` against a path in stratumd's
    tree to PROVE `/` is now the disk-backed FS
11. `Thylacine boot OK`

Step 9 is the last operation that needs a spawn-from-devramfs path.
Every step 1-8 resolves spawn paths through devramfs first; the pivot
happens AFTER all bringup. Post-pivot, joey just sits in its
long-running init role — it does not spawn anything else at v1.0 (a
shell / interactive surface is Phase 7 — Utopia).

Existing peers (corvus, stratumd) hold their own territories — those
were stamped at their spawn time and are not affected by joey's later
pivot. Their already-running executables are loaded in RAM and need
no path resolution to keep running. The pivot is purely joey-local.

#### Why distinct from SYS_CHROOT

The existing `SYS_CHROOT` (per `kernel/syscall.c::sys_chroot_handler`)
allows re-chroot: a subsequent SYS_CHROOT call drops the displaced
root's ref and installs the new one (idempotent on same-spoor). The
machinery for "atomic root_spoor replacement with displaced-ref
tear-down" already exists.

A new `SYS_PIVOT_ROOT` is preferable to overloading SYS_CHROOT
because:

- **Audit-tractability.** SYS_CHROOT is audited (P5-stratumd-stub-
  bringup-e2). Changing its contract — even to make explicit what
  the implementation already does — re-litigates an audited surface.
  A new syscall with its own audit-trigger row is cleaner.
- **Semantic clarity.** "Pivot" names the long-running-Proc usage
  pattern (the v1.x note in `usr/joey/joey.c:293-304` explicitly
  asked for `pivot_root`); SYS_CHROOT names the initial-chroot
  pattern. Two syscalls for two patterns reads more honestly than
  one syscall doing both.
- **Future extensibility.** A v1.x lift might want `SYS_PIVOT_ROOT`
  to preserve specific bind mounts across the pivot (e.g., a `/dev`
  bind survives). Evolving that on a distinct syscall doesn't change
  the SYS_CHROOT contract.

The pivot at v1.0 carries NO mounts across (joey's mount table at
pivot time only contains the just-mounted `/sysroot` entry, and that
entry is structurally subsumed by the pivot — the territory's
root_spoor becomes the same tree the mount points to). v1.x bind-
survivor semantics are a deliberate non-decision at v1.0.

#### Stub retirement scope

The userspace `usr/stub/` stratumd-stub binary + joey's
`do_stratumd_stub_bringup()` invocation retire. The kernel-side
`kernel/dev9p.c` machinery + the kernel-internal tests
(`kernel/test/test_stratumd_stub.c`, `test_stub_driver.c`,
`test_9p_attach.c`, `test_territory_chroot.c`,
`test_sys_spawn_with_fds.c`) are KEPT — they cover the dev9p Dev
vtable + the SYS_ATTACH_9P + the SYS_CHROOT machinery from the
kernel's perspective, and that machinery is still load-bearing for
SYS_ATTACH_9P_SRV (which composes p9_attached_create + dev9p root
the same way). Removing the kernel-side stub would lose audit
coverage for no benefit.

#### Audit-trigger surfaces

Three new rows added to CLAUDE.md's audit-trigger table by this
scripture commit:

1. **9P-srvconn transport adapter** —
   `kernel/9p_srvconn_transport.{c,h}`. The byte-mode SrvConn rings
   wrapped into `p9_transport_ops`. Lifetime via `srvconn_ref` /
   `srvconn_unref`; the byte-mode gate is enforced at the consumer
   (SYS_ATTACH_9P_SRV) not the adapter (defense in depth).
2. **SYS_ATTACH_9P_SRV** — `kernel/syscall.c::sys_attach_9p_srv_handler`.
   The composition of SrvConn handle lookup + byte-mode gate + adapter
   kmalloc + `p9_attached_create` + handle alloc as KOBJ_SPOOR. Full
   failure-path rollback discipline; same shape as the audited
   SYS_ATTACH_9P.
3. **SYS_PIVOT_ROOT + territory_pivot_root** — `kernel/syscall.c::
   sys_pivot_root_handler` + new `kernel/territory.c::territory_pivot_
   root` core. Atomic root_spoor swap with displaced-ref tear-down;
   touches the audited Territory surface.

A fourth row (`stratum-mkfs --populate-from`) is NOT Thylacine-kernel
audit-bearing; it is host build infra (Stratum side). Cross-project
rights-mirror discipline applies the same way as the bdev_thylacine
arm — the populate path's KObj_rights queries (none at v1.0 — it
operates on host files) must mirror `kernel/include/thylacine/handle.h`
if/when it adds any.

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

## 16b-γ-mount-bind deep-dive (Finding 2 narrowing)

The follow-up session pinned Finding 2 (mallocng corruption) to
a specific code site and isolated a key invariant violation, but
did NOT identify a v1.0 root-cause for the corruption itself. All
changes were diagnostic-only (Stratum-side fprintf tracers in
`src/btree_store/crypt.c`, pouch-side temporary CAP_CSPRNG_READ
grant in `usr/joey/joey.c`, pouch-side mallocng instrumentation
in `build/pouch/musl-src/`). All reverted before commit; no v1.0
shippable fix landed this round. The narrowing IS the deliverable.

### Narrowing the call site

With CAP_CSPRNG_READ temporarily granted, stratumd advances
through `stm_sync_open`, then deep into the btree-engine cold path
during mount-root validation. Per-malloc/free ring buffer
instrumentation in pouch mallocng (caller-PC recorded via
`__builtin_return_address(0)`) pinned the failing free to:

- **Function**: `stm_btree_node_decrypt` (Stratum
  `src/btree_store/crypt.c::stm_btree_node_decrypt`).
- **Pattern**: `ct = malloc(node_size); memcpy(ct, buf, node_size);
  stm_aead_decrypt(...); free(ct);` -- the temporary
  ciphertext-scratch buffer.
- **Specifics**: `node_size = STM_BTNODE_SIZE = 131072 bytes`. The
  failing free is at `pc=0x26fe70` (right after the memcpy + AEAD
  decrypt + free chain).

### What's different about the SECOND call

`stm_btree_node_decrypt` is called twice during mount-root
validation:

1. **First call (root)**: from
   `stm_btree_store_deserialize` for the root node. `ct` malloc'd
   at e.g. `0x10002a0b0`. `memcpy(ct, root_buf, 131072)`
   completes; `decrypt` returns OK; `free(ct)` succeeds. Clean
   pass.
2. **Second call (leaf)**: from the same function's child loop
   for the first child leaf. `ct` malloc'd at e.g. `0x10002b0e0`.
   `memcpy(ct, leaf_buf, 131072)` completes; `decrypt` returns
   `STM_EBADTAG` (`s=0xffffff37 = -201`); **`free(ct)` trips
   `assert(!end[-5])` in `get_nominal_size`** (or sometimes a
   different assert in the same chain).

The SAME function code runs in both calls. The DIFFERENCE is the
data and the caller-supplied buf.

### Slot-footer corruption empirics

Adding pre/post probes around the memcpy:

- **PRE-MEMCPY** (immediately after malloc, before memcpy):
  - For both calls: `ct[node_size + 0..16] = 0x00 * 16`
    (clean, anon-mmap zero).
  - For both calls: `ct[node_size + 3860..3868]` =
    `00 00 00 00 1c 0f 00 00` -- mallocng's `set_size`-written
    size field (reserved=3868=0xf1c) and sentinel byte
    (`end[-5]=0`). Correct mallocng setup.
- **POST-MEMCPY** (immediately after memcpy):
  - First call: `past = 00...` (clean), `ftr = 00 00 00 00 ...`
    (clean).
  - Second call: `past = <16 high-entropy bytes>`,
    `ftr = <8 high-entropy bytes>` -- the slot footer has been
    OVERWRITTEN.

The corruption bytes vary across runs (NOT deterministic) but the
pattern is consistent: the SECOND call's ct slot footer becomes
non-zero between malloc-return and free-call.

### What rule-outs were proven

The most baffling part: **the byte-copy operation that triggers
the corruption is provably correct**.

1. **`memcpy` is innocent**: replacing `memcpy(ct, buf, node_size)`
   with a hand-written `for (i=0; i<node_size; i++) ct[i]=buf[i]`
   loop (via `volatile` casts to prevent vectorization) gives the
   IDENTICAL corruption pattern.
2. **The C compiler is innocent**: replacing the C loop with
   explicit inline `__asm__("...ldrb...strb...sub...b...")` byte
   loop gives the IDENTICAL corruption pattern.
3. **The byte loop's writes are bounded**: per disassembly, the
   loop's `strb` writes only to `[ct, ct + node_size)`. The
   loop's `cmp x21 (=node_size), x9 (=counter)` correctly exits
   when counter hits node_size. Verified by encoded instruction
   decode + scanning all `str/stp/strb` instructions in the
   function body for any write via x19 (= ct), x21 (= ct +
   node_size), or x23 (= ct + 134932).
4. **The byte loop's pattern persists**: writing a unique
   pre-loop pattern at `ct[node_size..node_size+16]` (e.g.,
   `0xA0..0xAF`) and checking after the loop shows the pattern
   IS PRESERVED -- the loop does NOT overwrite those bytes
   (verified for the first call; the second call regresses to
   the empirical corruption pattern when the pre-loop pattern is
   removed).
5. **Without the byte copy, no corruption**: replacing the entire
   `memcpy(ct, buf, node_size)` with no-op (just `(void)buf;`)
   makes the boot green -- decrypt fails differently (ct is
   uninitialized), but ct's slot footer stays clean.

So the byte-copy operation is the necessary trigger, but it
DOESN'T write to the corrupted address range.

### Unexplained observations

- Corruption bytes don't match `buf[node_size..node_size+16]`
  (which is also zero per a separate probe).
- Corruption bytes look like AEGIS-256 cipher output or
  high-entropy stack/cache state.
- The first call to `stm_btree_node_decrypt` (root) has
  IDENTICAL code but NO corruption.

### Candidate root-cause directions for next session

(In rough priority order.)

1. **Kernel mmap/page coherency**: ct's mmap region for the
   second call reuses VA pages that were previously freed (the
   first call's root_buf munmap freed its region). The kernel
   may not be flushing dcache on munmap or zeroing the new
   page's cache lines before remapping. The byte loop's writes
   may cause a stale cache line at ct[node_size..] to be
   evicted to RAM, surfacing stale content.
2. **QEMU emulation quirk**: QEMU's TCG cache emulation may
   have a corner case where many sequential byte stores cause
   spurious cache line behavior. Worth testing with KVM (if
   available on M2 host) vs TCG.
3. **bdev_thylacine DMA interaction**: virtio-blk DMA writes
   may have lingering coherency effects on adjacent CPU-visible
   memory regions. The DMA buffer is at THYLA_DATA_USER_VA
   (fixed), distinct from ct's VA, but the kernel's page table
   setup might share TLB entries or cache lines.
4. **libsodium internal allocator hooks**: `_sodium_alloc_init`
   may interpose on malloc/free with allocator state that
   mismatches mallocng's expectations after multiple
   allocations. The first stm_btree_node_decrypt call's
   alloc/free pair clears state; the second's pair (with
   intervening sodium-internal allocations) might trigger a
   sodium-vs-mallocng mismatch.
5. **mallocng size-class 63 (mmap'd) corner case**: 128 KiB
   crosses MMAP_THRESHOLD (131052), so the allocation uses
   sizeclass 63 (individually-mmapped). This path is
   exercised heavily by stratumd but rarely by
   `pouch-hello-malloc` (which uses small slots). A
   sizeclass-63-specific bug in pouch's mallocng port is
   possible.

### Pragmatic fix path

Without root-causing, the most direct fix to unblock 16c is to
eliminate the `malloc(node_size) + memcpy + decrypt + free`
pattern in `stm_btree_node_decrypt` by switching to **in-place
decrypt**: `stm_aead_decrypt(..., buf, node_size, buf, &pt_len)`.
libsodium's `aegis256_decrypt_detached` supports in-place
operation (the spec allows overlapping ct/pt). The behavior delta
is documented in the current code's comment: "a tag-fail leaves
buf in undefined state (the caller must discard it)" -- already
the contract, so in-place is semantically equivalent.

This requires a Stratum-side coordination change on
`thylacine-pouch-arm`. The next session should:

1. Patch `src/btree_store/crypt.c::stm_btree_node_decrypt` to
   in-place.
2. Verify joey boot reaches AT LEAST `Thylacine boot OK` with
   the second-call corruption no longer triggering (decrypt may
   still return STM_EBADTAG, but free's slot integrity holds).
3. If decrypt-EBADTAG persists, that's a SEPARATE issue (likely
   a key/AAD/data mismatch in the leaf's on-disk encoding) and
   shifts focus there.
4. Continue to investigate the underlying byte-copy-vs-slot-
   footer phenomenon in parallel; the in-place fix is a workaround,
   not a root cause.

### What this session did NOT change

- No kernel code change.
- No Stratum-side commit (all crypt.c changes were diagnostic
  and reverted).
- No pouch patch added/removed.
- No joey probe change (the CAP_CSPRNG_READ grant was
  diagnostic-only, reverted).
- Test suite: 609/609 PASS x default + UBSan (unchanged).
- Boot: `Thylacine boot OK` reproducible; joey's stratumd-boot
  probe `child exit_status=1 (final drain)` reproducible.

---

## 16b-γ-mount-bind mmap-bypass attempt + bdev_thylacine read-loop blocker

This session attempted Option (i) from the prior session's
"Pragmatic fix path" (in-place decrypt) but found it BLOCKED by
the existing crypt.c comment (lines 27-37): libsodium's AEGIS-256
is empirically NOT safe under aliased ct/pt -- Stratum tested this
and got zero plaintext output. The recommendation was based on
libsodium's general docs ("m and c can point to overlapping memory")
but didn't account for Stratum's specific empirical finding.

Pivoted to Option (i'): bypass mallocng's sizeclass-63 path via
direct mmap. The page-grain mmap region has no allocator-side slot
footer at `ct[node_size..]` for memcpy's adjacency to perturb;
`munmap` returns the pages cleanly with no integrity check. The
change is gated by `#ifdef __thylacine__` in Stratum's
`src/btree_store/crypt.c`, leaving the Linux + glibc path
byte-identically preserved.

### What landed

Stratum-side (branch `thylacine-pouch-arm`, commit `c5b998c`):

| Component | Path |
|---|---|
| Mmap-bypass in `stm_btree_node_decrypt` | `src/btree_store/crypt.c` |
| Session-handoff doc | `docs/session-handoff-2026-05-25-thylacine-mmap-decrypt.md` |

Thylacine-side: no kernel/userspace code change. This reference
doc + phase status + memory updates only.

### Verification status: DORMANT

The mmap fix CANNOT be verified end-to-end yet. A SEPARATE upstream
bug in `bdev_thylacine.c` (the same branch's earlier `63a3eb1`)
sends a zero-sized virtio descriptor at approximately the 113th
sequential read during `stm_sb_mount_scan`'s label x slot
iteration. QEMU rejects with `virtio: zero sized buffers are not
allowed` and the boot eventually times out / EL0-faults.

Empirically (with diagnostic fprintfs in `stm_sb_mount_scan` AND
`T_CAP_CSPRNG_READ` granted in joey):

- `li=0 si=0..62` -- 63 reads, each returns -2 (STM_ENOENT for
  unused slots, expected for most positions).
- `li=1 si=0..48` -- 49 reads, also -2 expected.
- `li=1 si=49` pre-read fires, then QEMU virtio error.

The pool fixture has a valid uberblock at `lbl=2 slot=6` (per the
prior session's diagnostic of sync.c's internal mount_scan). The
fs.c-side `stm_sb_mount_scan` would normally reach this on its own
loop. The bug halts the loop before label 2.

Without the diagnostic fprintfs, the same path produces an EL0
fault at vaddr 0x0 -- likely the same root cause but observed at a
faster downstream symptom (the QEMU error is the kernel's
bdev_thylacine emitting the bad descriptor; the EL0 fault is
stratumd dereferencing NULL when bdev_thylacine returns an error
that's not handled).

### Why this leaves the fix in-tree

The mmap-bypass is a **forward-looking** quality improvement:

1. When the bdev_thylacine read-loop bug is fixed in a later
   session, the mount path will reach `stm_btree_node_decrypt`'s
   second call. Without this fix, that call would trip mallocng's
   slot-footer assertion and extinct the kernel.
2. The change is gated by `#ifdef __thylacine__`, so other
   platforms see no behavioral change.
3. The 2026-05-23 SIGSEGV-like note delivery (Phase 5+
   FAULT_UNHANDLED_USER architectural fix) would protect against
   kernel extinction from user-mode NULL deref, but mallocng's
   `assert(!end[-5])` would still trip and `_Exit(127)` the
   stratumd process. The mmap fix prevents the assert from firing
   at all.

### Boot state at this checkpoint

- `Thylacine boot OK` (599/599 PASS x default + UBSan).
- joey's stratumd-boot child `exit_status=1 (final drain)` --
  same as baseline (without CAP_CSPRNG_READ, sodium_init fails;
  joey reaps clean).
- The CAP_CSPRNG_READ grant in joey was attempted and REVERTED
  this session. It remains a v1.x lift, hard-paired with the
  bdev_thylacine read-loop fix.

### Carry-forwards (priority order)

1. **bdev_thylacine read-loop bug** (NEW, blocks 16c): root-cause
   the zero-sized virtio descriptor at the ~113th sequential read.
   Likely a descriptor reuse bug, a read-buffer reset gap, or a
   sequence number off-by-one in `bdev_thylacine`'s single-chain
   descriptor reuse. Stratum-side `src/block/bdev_thylacine.c`
   `do_request` and the descriptor chain init (the IO-2 setup).
2. **mallocng root cause** (still open): the underlying mechanism
   that causes the sizeclass-63 slot footer to corrupt after a
   memcpy whose writes don't touch the footer. Top hypothesis:
   kernel page-allocation coherency on freed-then-reused mmap
   regions. The mmap-bypass dodges the symptom without addressing
   the mechanism. See "16b-γ-mount-bind deep-dive (Finding 2
   narrowing)" above.
3. **SIGSEGV-like note delivery** (Phase 5+ architectural):
   `FAULT_UNHANDLED_USER` should deliver a note to the offending
   Proc rather than extinct the kernel. Independent of (1) and
   (2) but reduces the blast radius of any future user-mode bug.

### What this session did NOT change

- No kernel code change.
- No Thylacine userspace code change (the CAP_CSPRNG_READ grant
  was attempted then reverted).
- No new pouch patch (the existing 0011-pouch-abort.patch and the
  other 11 patches remain).
- Test suite: 599/599 PASS x default + UBSan (unchanged).
- Boot: `Thylacine boot OK` reproducible.

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

---

## 16b-γ-mount-bind narrowing — AEGIS-256 soft decrypt as the corruption source

This session re-investigated the "bdev_thylacine read-loop bug"
documented above (the symptom: zero-sized virtio descriptor at
~the 113th read). With diagnostic fprintfs in `mount_scan` and
`do_request` and a kernel-side PC/LR/ESR/FAR dump on EL0 fault
(all reverted before commit), the prior session's diagnosis was
**falsified**:

- bdev_thylacine reads cleanly through ALL 252 mount_scan iterations
  + 5 subsequent reads (avail_idx 0..509). No descriptor drift,
  no zero-sized buffers, no DSB SY ordering issue.
- The "zero sized buffers" QEMU error from the prior session was
  a TIMING ARTIFACT — heavy fprintf instrumentation slows virtio
  enough that QEMU eventually sees a stale descriptor; without the
  fprintfs (or with a lighter dose) the loop completes cleanly.
- mount_scan returns OK; sync_open advances; the path then reads
  two 128 KiB btree-node chunks (lba 2304 + lba 2816, paddr 288 +
  paddr 352, both with the `STBTNODE` magic).

The actual crash site: **inside libsodium's AEGIS-256 soft
`decrypt_detached`**, called by `stm_btree_node_decrypt` on the
**second** btree node read (paddr 352, gen 1). The crash is
detected by mallocng's `alloc_slot` `assert(!p[-4])` (the slot
footer sentinel) — heap corruption.

### Empirical bisect (all reverted)

Kernel diag dumped on EL0 fault:
- PC inside `alloc_slot` (mallocng's slot-corruption a_crash)
- LR also inside `alloc_slot` (recursive `alloc_group ->
  alloc_slot`)
- FAR = 0x0 (a_crash: `mov x17, xzr; strb wzr, [x17]`)
- ESR = 0x92000046 (data abort lower EL, L2 translation fault)
- SP = 0x7fff90d0 (well within the 256 KiB user stack)

Bisect by overriding the soft `decrypt_detached`'s loop body:

| Loop body | Boot outcome |
|---|---|
| Original `aegis256_dec(m+i, c+i, state)` | EL0 NULL deref in alloc_slot |
| Empty (`i = (mlen / RATE) * RATE`) | **`Thylacine boot OK`** |
| `memset(m+i, 0xaa, RATE)` | **`Thylacine boot OK`** |

The empty-loop and memset-loop variants write the SAME 131040
bytes to the SAME output buffer that the original loop targets.
The CRASH IS SPECIFIC TO `aegis256_dec` — not buffer overrun,
not iteration count.

Periodic-fprintf bisect of the original loop:
- Last successful iteration: i = 102400 (m+i = 0x100021090)
- Crash window: i ∈ [102400, 103424) — i.e., between the
  6400th and 6464th call to `aegis256_dec`.

### Path validation

- Implementation pointer correctly resolves to
  `aegis256_soft_implementation` (impl=0x2b4008, impl->dd=0x27a52c).
  ARM crypto path is NOT taken (Thylacine's auxv lacks AT_HWCAP, so
  `sodium_runtime_has_armcrypto()` returns 0; expected).
- `crypto_aead_aegis256_decrypt_detached` IS entered with valid
  args (m / c / clen / mac all reasonable, k non-NULL).
- `decrypt_detached` (the soft impl) entry + post-init + pre-loop
  fprintfs all fire. Post-loop fprintf does NOT fire — confirms the
  crash is INSIDE the loop or its compiled tail.

### Hypotheses (ordered by likelihood)

1. **Compiler / vectorization bug**. `aegis256_dec` is pure C in
   `aegis256_common.h`, but clang at -O2 cross-compiling for aarch64
   may vectorize the XOR / AND chain into NEON instructions with an
   alignment / ordering bug. The fact that memset (which the
   compiler emits as straight NEON store-pair `stp q0, q0, ...`)
   does NOT crash suggests the issue is specific to the
   `LOAD_XOR_XOR_XOR_AND_STORE` pattern compiled by clang for the
   soft path, not pure store-bandwidth.

2. **`aegis256_update`-side stack corruption**. Each iteration calls
   `aegis256_update(state, msg)` which calls `softaes_block_encrypt`
   5 times. `_encrypt` allocates ~1 KiB of stack (`uint32_t
   t[4][4][16]` + alignment) per call. The 6400+ iterations
   exercise this path deeply; a stack-overlap or compiler
   stack-management bug could touch caller frames. (Stack depth is
   bounded — peak ~3-4 KiB above the user stack base, well within
   256 KiB.)

3. **mallocng heap layout interaction**. The output buffer m =
   `0x100008090` sits inside a mallocng-managed 131072-byte
   allocation (sizeclass 63 — direct-mmap path); the mmap region
   reaches ~0x100029080. Writes go to m..m+131040 (= 0x100028070),
   in-bounds. But the heap also holds mallocng's per-group meta
   structures, secrets, the `ctx.active[sc]` linkages, etc., and
   if `aegis256_dec`'s compiled body touches any of those by
   mistake (e.g., via a register-spill that points at random heap)
   the corruption shows up on the next mallocng call (the post-
   loop fprintf transitively triggers `alloc_slot`).

The root cause needs a deeper investigation — disassembling the
compiled `decrypt_detached` body and walking the actual writes,
or running a small unit test of `crypto_aead_aegis256_decrypt`
against a 131040-byte plaintext under pouch's libsodium + arch=arm64
to reproduce in isolation. That isolated reproducer can then be
shared with libsodium upstream if the bug is on their side.

### Workaround path (v1.x)

The pre-existing **mmap-bypass for the decrypt scratch** (Stratum
`c5b998c`) is correct but insufficient — it dodges one
mallocng-cycle bug (the ct allocation/free corruption) but the
AEGIS-256 soft decrypt itself has a separate corruption mechanism
that fires on the m output write.

Pragmatic workarounds for an unblocking v1.x close:

- **(W-a)** Force libsodium's AEGIS-256 ARM crypto path. The
  ARMv8-A AES instructions (`vaeseq_u8` / `vaesmcq_u8`) are
  hardware-implemented on QEMU's TCG-emulated cpu=max model. The
  detection currently fails because Thylacine doesn't expose
  AT_HWCAP; either (1) add AT_HWCAP with HWCAP_AES bit to
  `kernel/exec.c::exec_build_init_stack`, or (2) patch pouch's
  `sodium_runtime_has_armcrypto()` to return 1 unconditionally.
  Trades: option (1) adds an audit-bearing kernel surface; option
  (2) is a boundary-line patch local to libsodium.
- **(W-b)** Hot-swap AEGIS-256 with XChaCha20-Poly1305 in
  Stratum (which Stratum already supports via `STM_AEAD_XCHACHA20_SIV`
  enum). Behind `#ifdef __thylacine__` in `src/btree_store/crypt.c`,
  call `stm_aead_decrypt(STM_AEAD_XCHACHA20_SIV, ...)`. Requires
  rewriting any persistent ciphertext in the boot pool; effectively
  a format-break for the system pool. Heavier.
- **(W-c)** Vendor or patch the pouch libsodium build to disable
  vectorization on AEGIS-256 (`-O0` or `__attribute__((optnone))`
  on the soft impl). Lightest blast radius if the hypothesis (1)
  is correct.

(W-a) is the cleanest and arrives with the smallest code change;
(W-c) confirms hypothesis 1 if the slow-down builds boot OK.

### Verification status: BLOCKED (root cause unknown)

The mmap-bypass landed at Stratum `c5b998c` remains correct + in
tree but DORMANT. The bdev_thylacine "read-loop bug" recorded as
task #712 in the prior session is **falsified**; the real blocker
is the AEGIS-256 soft decrypt path. Baseline preserved:
`Thylacine boot OK`, 599/599 PASS × default + UBSan, joey probe
NON-FATAL (clean reap on stratumd's sodium_init exit-127).

---

## 16b-γ-mount-bind further narrowing — libsodium AEGIS-256 isolated as NOT broken

This session attempted to root-cause the AEGIS-256 corruption by
isolating it. Key findings (all diagnostics reverted before commit;
baseline restored):

### libsodium AEGIS-256 works correctly in isolation

A minimal repro was added to `pouch-hello-sodium.c`: malloc(131072)
two buffers, encrypt 131040 bytes with AEGIS-256, decrypt back,
verify round-trip. Plus a stress loop of 8 iterations
(malloc/decrypt/free) to exercise mallocng's sizeclass-63 mmap
reuse. **Both passed cleanly** under pouch musl + libsodium 1.0.20
on Thylacine. This **falsifies any "libsodium AEGIS-256 is broken
on Thylacine" hypothesis** — the primitive works in isolation.

### Why pouch-hello works on Thylacine and Stratum doesn't

macOS arm64 uses libsodium's ARM crypto path (HW AES) because
`sodium_runtime_has_armcrypto()` returns 1 (via `sysctlbyname`).
Most Linux ARM with HWCAP_AES likewise. The portable C AEGIS-256
soft impl is rarely exercised in practice. On Thylacine the soft
impl IS used (no AT_HWCAP → has_armcrypto=0). pouch-hello-sodium
proves the soft impl is correct on Thylacine. So the corruption is
not in the soft impl itself.

### The corruption is context-specific to Stratum's heap state

A self-test inserted at the START of `stm_stratumd_run` (before any
Stratum logic) — same malloc(131072) + encrypt + decrypt(131040) —
**works**. The corruption only manifests after Stratum's mount
code has run (keyfile_load, bdev_open, sb_mount_scan, pool_open,
alloc_open, sync_open). At the point where `stm_btree_node_decrypt`
is called for paddr=352 gen=1, the heap is in a state that triggers
the crash on the real call.

A self-test inserted INSIDE stm_btree_node_decrypt (right before
the real `stm_aead_decrypt` call) — using FRESH malloc'd input +
fresh malloc'd output, with the same `cx->metadata_key` / nonce / ad
as the real call — **works**. So the libsodium dispatch is correct
at that program point with arbitrary args.

### The corruption requires the specific (mmap'd ct + mallocng buf) combo

Bisecting the actual `stm_aead_decrypt(key, nonce, ad, ct, len, buf, &pt_len)`
call by swapping the buffers:

| ct                                              | buf                        | Result   |
|---|---|---|
| **mallocng** (via `malloc(131072)`, copied from real ct) | mallocng (real buf, 0x100008090) | **WORKS** |
| **mmap'd** (via the mmap-bypass at 0x10002a000) | mallocng (fresh malloc'd 131072) | **WORKS** |
| **mallocng** (fresh malloc'd 131072)            | mallocng (fresh malloc'd 131072) | **WORKS** |
| **mmap'd** (the actual scratch, 0x10002a000)    | mallocng (real buf, 0x100008090) | **CRASHES** |

The ONLY failing combination is **direct mmap'd ct + the actual
mallocng-allocated buf**. The crash signature is mallocng's
`alloc_slot::enframe()::assert(!p[-4])` failing — heap corruption.

### Hypotheses for the actual root cause (next session)

1. **Page-zeroing**: the direct mmap-bypass might receive pages that
   weren't properly zeroed. mallocng's enframe expects `p[-4] == 0`;
   if the kernel's `SYS_BURROW_ATTACH` returns stale-content pages
   from a recycled VMA, that invariant is violated. Test: after the
   direct mmap, print `ct[0..7]` and `ct[131072..131088]` — if any
   non-zero on a fresh mmap, the kernel isn't zeroing.

2. **VMA adjacency / TLB interaction**: real ct at 0x10002a000 and
   real buf at 0x100008090 are in adjacent VAs (~135KB apart). The
   kernel's pgtable code might mishandle some bookkeeping when two
   large 33-page VMAs are processed alternately by AEGIS-256 (each
   iteration: read 16 bytes from ct+i, write 16 bytes to buf+i).
   When mt_buf is a fresh malloc, its VMA is elsewhere; that breaks
   the alternation pattern.

3. **mallocng metadata adjacency**: mallocng's allocation for real
   buf has metadata at known offsets. The direct mmap-bypass for ct
   sits at a specific address that might overlap with a mallocng-
   tracked region (the "active_idx" group struct, or the ctx state).
   When AEGIS-256 reads ct and writes buf, the writes might cross-
   contaminate mallocng's tracking structures.

The diagnostic at iteration i ∈ [102400, 103424) (from the prior
session's bisect) corresponds to writing to buf+102400..buf+103440.
That's still inside buf's allocation (which extends to buf+131072).
So a forward-overrun of buf isn't the issue. The corruption must be
a side-effect of the WRITE PATTERN reaching some specific
combination of buf address + ct address that confuses mallocng's
state machine.

### Verification status: BLOCKED at narrowing-level-3

The bug is now narrowed to "specifically the combination of direct
mmap'd ct + mallocng-allocated buf causes mallocng heap corruption
after AEGIS-256 decrypt". Next investigation: instrument the kernel
side `SYS_BURROW_ATTACH` and `userland_demand_page` to see if the
mmap'd pages are properly zeroed; OR instrument mallocng's
`ctx.active[*]` state to see what gets corrupted.

NOT a libsodium bug. NOT reportable upstream — the primitive works.

Workaround W-a (expose AT_HWCAP → ARM crypto path) remains the
cleanest unblock; bypassing the soft impl entirely sidesteps this
context-specific interaction.

---

## 16b-γ-mount-bind H1 investigation — kernel page-zeroing falsified at code-review level + AEGIS corruption is content-sensitive across pool.img regenerations (2026-05-26)

Follow-up to the prior session's three documented hypotheses. Two
findings, no code shipped (all diagnostics reverted; baseline preserved
at tip `7045e1f`).

### Finding 1 — H1 (page-zeroing) falsified at code-review level

`alloc_pages(order, KP_ZERO)` in `mm/phys.c:228-236` unconditionally
zeros the entire 2^order chunk via the kernel direct map after the
buddy / magazine allocation:

    if (flags & KP_ZERO) {
        u64 *q = pa_to_kva(page_to_pa(p));
        u64 n  = (1ull << order) << PAGE_SHIFT;
        for (u64 i = 0; i < n / 8; i++) q[i] = 0;
    }

The zeroing covers the FULL buddy chunk, not just the user-requested
range — so e.g. a 33-page request that the buddy serves from a 64-page
chunk gets all 64 pages zeroed. `SYS_BURROW_ATTACH` → `burrow_create_anon`
passes `KP_ZERO`; `userland_demand_page` (arch/arm64/fault.c:339-369)
installs the PTE pointing at the already-zeroed PA. The independently-
proven `pouch-hello-sodium` repro (malloc 131072 x 2 + AEGIS-256 round-
trip + 8-iter stress) shows userspace observes zero pages from
mallocng's sizeclass-63 path. H1 is mechanically out.

### Finding 2 — what `assert(!p[-4])` actually checks

Two paths through mallocng's `enframe` (`third_party/musl/src/malloc/
mallocng/meta.h:196-227`) reach the assertion with different
interpretations of `p[-4]`:

- **sizeclass < 48 (multi-slot group)**: `p = storage + stride*idx`
  with `idx > 0`. `p[-4]` is the 4-byte zone at the END of slot
  `idx-1` — the slot's "overflow byte" planted by `set_size` and re-
  checked by `get_nominal_size`. Assertion firing means the previous
  slot's user wrote PAST its allocation, smashing the trailing
  sentinel.

- **sizeclass = 63 (single-slot mmap)**: `idx = 0`, so
  `p = g->mem->storage` (offset 16 of the fresh mmap region).
  `p[-4]` is byte 12 of the mmap — inside `struct group::pad[7]`
  (offsets 9..15 per the struct layout in `meta.h:17-22`). After
  fresh kernel mmap + the only write `g->mem->meta = g` (offset 0..7),
  the pad bytes are never touched and should remain zero.

The two interpretations point at completely different root causes:
multi-slot signals **user-data overflow** (some allocation wrote past
its bounds); single-slot signals **fresh-page non-zero** (which H1
already falsifies via code review) OR **stale-VA reuse** (the kernel
returned a VA whose pages weren't actually re-zeroed because some
allocator-side caching bypassed `KP_ZERO`).

The prior session's narrowing notes report the crash signature as
`alloc_slot::enframe::assert(!p[-4])` but did not record the failing
sizeclass. **A future diagnostic must capture sizeclass + idx at the
firing site** to disambiguate.

### Finding 3 — AEGIS corruption is content-sensitive: didn't reproduce in this session

This session built a minimal diagnostic patch on `meta.h` to print
sizeclass / idx / maplen / stride / bytes around `p` at the assertion
site before letting `a_crash` fire. Two boots:

1. **With diagnostic instrumentation** — built sysroot + stratumd
   against the instrumented `meta.h`. stratumd reached
   `serving /dev/virtio-blk on /srv/stratum-fs (backlog=16, msize=...)`
   without any `MNG:` diagnostic line firing.
2. **Pristine baseline** — reverted the diagnostic, rebuilt sysroot
   + stratumd. stratumd ALSO reached the same `serving` message.

Both boots are past the prior session's documented crash point
(stratumd crashing during `stm_sb_mount_scan` BEFORE setting up the
listen socket).

The root cause for the divergent behavior: `build_stratum_pool_fixture`
(`tools/build.sh:947-997`) **regenerates `build/fixtures/pool.img`
from scratch each `tools/build.sh kernel` run**, deleting the prior
file (build.sh:982). stratum-mkfs's UUID seed derives from `time + pid`
(build.sh:957-961), so the pool's btree node payload bytes differ
across builds. Today's pool.img happens to encode data that doesn't
trigger the AEGIS-256-soft / mallocng interaction; the prior session's
pool.img did.

This finding is the strongest evidence yet that the corruption is
**content-sensitive at the bit-pattern level**: same AEGIS-256 code,
same mallocng code, same kernel, same caps — different decrypt input
bytes → different mallocng-heap-state side-effect → bug fires or
doesn't.

Both boots hung at the same downstream point (after stratumd's
`serving` message, before any `joey: stratumd-boot /srv/stratum-fs
bound after retry` success line). That downstream hang is the
**actual 16b-γ-mount-bind blocker** — separate from the AEGIS
corruption, queued under the original sub-chunk scope. The prior
sessions conflated the two; the AEGIS corruption was the visible
symptom at the time, but the mount-bind hang was always there
behind it.

### Implications for next session

- The AEGIS corruption is no longer reliably reproducible on tip
  `7045e1f` — pool.img regeneration changes the trigger pattern.
  Reproducing the prior session's exact behavior would require
  pinning the pool.img bytes (recovering from a prior session's
  build/ if available, OR seeding stratum-mkfs's RNG deterministically).
- The mount-bind hang IS the visible blocker today. Investigation
  shifts from "fix AEGIS corruption" to "diagnose why joey's
  `t_srv_connect` retry loop never succeeds even after stratumd
  posts /srv/stratum-fs".
- The AEGIS investigation work is preserved (3 documented hypotheses
  in the prior section + this section's H1 falsification + assertion
  semantics) for the eventual recurrence — content-sensitive bugs
  reappear when the trigger content does.
- W-a (expose AT_HWCAP → ARM crypto) remains the recommended
  hardening: it eliminates the soft AEGIS path as an attack surface,
  making AEGIS-related corruption recurrence impossible.

### Cross-references

- Code-review of kernel mmap path: `kernel/burrow.c::burrow_create_anon`
  (lines 88-120), `mm/phys.c::alloc_pages` (lines 220-238),
  `arch/arm64/fault.c::userland_demand_page` (lines 289-372),
  `kernel/syscall.c::sys_burrow_attach_for_proc` (lines 1804-1855).
- mallocng `enframe` assertion: `third_party/musl/src/malloc/
  mallocng/meta.h:196-227`.
- mallocng `struct group` layout: `meta.h:17-22`.
- Pool fixture regeneration: `tools/build.sh:947-997`.

---

## 16b-γ-mount-bind followup — there is no mount-bind hang; boot completes per prior session's documented behavior (2026-05-26)

Brief followup to the H1 investigation. Per user direction, before
investing in memory-model hardening, sequenced a confirmation run to
verify whether the apparent "downstream mount-bind hang" was a real
bug or just slow joey retry-loop timing.

### Setup

- Tip `bf42414` (post-H1 documentation commits).
- Pristine baseline (no diagnostics).
- `BOOT_TIMEOUT=600` (10 min, doubled from default 420 to allow full
  retry-loop wall-clock).

### Result

Boot completes cleanly: `Thylacine boot OK` at ~5 min wall-clock.
Tail of the boot log:

    joey: probe /system.key lseek SEEK_END/SEEK_SET OK
    stratumd: serving /dev/virtio-blk on /srv/stratum-fs (backlog=16, ...)
    joey: stratumd-boot /srv/stratum-fs not bound (16b-gamma-mount-close;
                                downstream stratumd mount path requires
                                further work)
    joey: stratumd-boot child exit_status=1 (final drain)
    stratumd-stub: serving on fd 0 (rx) + fd 1 (tx)
    stratumd-stub: EOF on rx; exit 0
    joey: stub-bringup ok (pipe + spawn + attach + mount + unmount + walk_open + read)
      joey: /joey pid=1524 exited cleanly (status=0)
    Thylacine boot OK

This matches the prior session's documented expected behavior
(`project_active.md`: "Boot continues to stub-bringup, joey exits
cleanly, kernel reports Thylacine boot OK"). **There is no mount-bind
hang.** The apparent "hang" in earlier sessions was just joey's
6000-retry busy-wait loop at ~50ms per retry (`usr/joey/joey.c:1470`
`for (volatile long j = 0; j < 1000000L; j++)`) — total ~5 min wall-
clock, with the boot output silent between stratumd's "serving"
message and joey's "not bound" message because neither side produces
stderr during the spin.

### Sub-finding: stratumd exit_status mismatch

The 16b-γ-mount-close prior session documented stratumd dying at
sodium_init via `abort() -> _Exit(127)` (the 0011-pouch-abort.patch
path). joey-side `t_wait_pid` should then surface `exit_status=127`.

Today's boot reports `child exit_status=1`. The `1` is incompatible
with the abort path (which exits 127) AND with stratumd main's normal
post-`stm_stratumd_run`-failure path (which returns 2 per `run.c:761`).
After the "serving" message and before `stm_stratumd_run` returns,
the only paths to exit_status=1 are pre-serve checks at lines 230,
315, 339, 348, 361, 375, 384, 398, 444, 463, 474, 490, 501, 515, 525,
540, 551, 571, 582, 590 (`grep -nE 'return 1' run.c`) — but those all
fire BEFORE the serving message, and we observe the serving message.

This means stratumd is dying somewhere OTHER than sodium_init's abort
path, with an unexplained exit code. The prior session's
characterization of "sodium_init -> abort -> _Exit(127)" may have
been incorrect, or the failure mode shifted across pool.img
regenerations (consistent with this section's "content-sensitive"
findings).

Investigation deferred — the resolution joins the AEGIS investigation
under the same v1.x lift envelope.

### Implications

The mount-bind investigation is closed: no bug, just slow retry.
The visible blocker is stratumd's silent exit at status=1, which:

- is NOT the documented sodium_init/_Exit(127) path
- happens AFTER the "serving" message
- happens BEFORE bind to `/srv/stratum-fs` registers the service

Next-session strategic decision: the original hardening proposals
(memory-model audit + page-poison + don't-extinct-on-userspace-faults
+ lockable mkfs seed) become the right next investment, since
(a) there is no new mount-bind bug to chase, and (b) the visible
blocker is opaque (silent exit) — hardening would make it diagnosable.

### Cross-reference

- Joey's retry+drain+reap loop: `usr/joey/joey.c:1444-1517` (the
  6000-retry budget at line 1445; the 1M busy-wait at line 1470).
- The "expected" rc=1 documented disposition: `project_active.md`
  (P6 16b-γ-mount-close section) — but the prior-session note also
  characterizes the failure as `sodium_init -> abort -> _Exit(127)`,
  which is incompatible with rc=1. The discrepancy is unexplained.

---

## 16c-pre — unblock stratumd's listen path on /srv/stratum-fs

**Status**: LANDED. Pure pouch-side boundary-line; NOT audit-bearing.
Two new patches (`0014-pouch-srv-stubs.patch`, `0015-pouch-poll-tag.patch`)
lift the visible "listen on /srv/stratum-fs failed: Function not
implemented" blocker that surfaced at the end of the 4-item memory-
model hardening plan close (`e69d634`).

### The blocker shape

At the close of the AEGIS arc + R-3a audit, the visible state was:

```
joey: probe /system.key fstat OK size=3656 mode=0o100644
joey: probe /system.key lseek SEEK_END/SEEK_SET OK
stratumd: serving /dev/virtio-blk on /srv/stratum-fs (...)
stratumd: listen on /srv/stratum-fs failed: Function not implemented
stratumd: run failed (rc=-207)
```

`stm_stratumd_listen_unix` (in Stratum's `src/cmd/stratumd/serve.c`)
runs an idiomatic POSIX socket-server prep dance:

```c
lstat(path, &st);          /* "is the socket path stale?" */
if (exists && !S_ISSOCK)   return -EEXIST;
unlink(path);              /* remove any stale entry */
socket(AF_UNIX, SOCK_STREAM, 0);
umask(0777 & ~mode);
bind(fd, ...);
umask(prev_umask);
chmod(path, mode);
listen(fd, backlog);
fcntl(fd, F_GETFD);
fcntl(fd, F_SETFD, FD_CLOEXEC);
```

Each of these had a hidden failure surface at the pouch boundary:

- `lstat` -> SYS_newfstatat = 0xFFFF -> ENOSYS -> the FIRST failure;
  message bubbles back to stratumd's `strerror(-listen_fd)` =
  "Function not implemented".
- `unlink` -> SYS_unlinkat = 0xFFFF -> ENOSYS (would be the next
  failure if lstat had succeeded).
- `socket` / `bind` / `listen` / `accept` -> already wired by sub-
  chunk 12 (pouch-sockets).
- `umask` -> SYS_umask = 0xFFFF -> ENOSYS. NON-fatal: umask discards
  the return value of its second call; bind happens regardless.
- `chmod` -> SYS_fchmodat = 0xFFFF -> ENOSYS (would block after
  bind succeeds).
- `fcntl(F_GETFD)` -> SYS_fcntl = 0xFFFF -> ENOSYS. NON-fatal:
  `if (flags >= 0) ...` skips the F_SETFD when F_GETFD fails.

The patch closes the three *blocking* surfaces (lstat, unlink, chmod)
and leaves the two non-blocking surfaces (umask, fcntl) at their
existing ENOSYS sentinel.

### 0014-pouch-srv-stubs.patch

Three single-function full-rewrite hunks. Each pre-checks the path
for the `/srv/` prefix; on match, short-circuits to the POSIX-shape
answer; on mismatch, falls through to the upstream call which still
yields ENOSYS via the 0xFFFF sentinel today.

| Wrapper | /srv/ short-circuit | Non-/srv | POSIX semantic |
|---|---|---|---|
| `lstat(p, &st)` | `errno = ENOENT; return -1` | `fstatat(AT_FDCWD, p, ..., AT_SYMLINK_NOFOLLOW)` (ENOSYS) | "no stale socket" — programs proceed to bind |
| `unlink(p)`     | `return 0`                  | `syscall(SYS_unlinkat, ..., 0)` (ENOSYS)             | "removed any stale entry" — no kernel op needed |
| `chmod(p, m)`   | `return 0`                  | `syscall(SYS_fchmodat, ..., m)` (ENOSYS)             | "set perms" — byte-mode SrvConn has no perm bits |

**Why `/srv/<name>` lstat returns ENOENT, not a stat-of-the-registry**:
v1.0 has no syscall to query the kernel `/srv` registry through a
stat shape. The Plan-9-correct surface is a registry-stat — but that
requires the kernel-side `/srv` namespace lift (Phase 5+ work
alongside ARCH §28's namespace-tree composition). Until then, the
"ENOENT" lie is safe because:

- Stratumd's check sequence is `if (lstat == 0 && !S_ISSOCK) return -EEXIST;
  else if (errno != ENOENT) return -errno`. ENOENT is tolerated.
- A future `/srv/<name>` collision surfaces from `bind()`'s
  SYS_post_service_byte (EACCES) — observable + actionable.
- The lstat-then-bind race window where another Proc registers in
  between is moot at v1.0 (single-Proc accept loop).

**Why `unlink` and `chmod` return 0**:

- `unlink("/srv/...")` returning 0 lies "removed the stale". The
  same future-collision surface (bind EACCES) catches the case
  where a stale registration actually existed; the program sees
  the clear error there.
- `chmod("/srv/...", mode)` returning 0 lies "set the mode". The
  byte-mode SrvConn carries no POSIX mode bits; the caller's
  intent (clamp permissions) has no analog in the registry.
  Returning 0 is the no-op-correct answer for a namespace where
  mode bits do not exist.

**Why `umask` and `fcntl` are NOT touched**:

- `umask(...)` is called twice in stratumd's listen path. The
  return value of the second call is discarded; the first call's
  return (cast to `mode_t` from `-1`) is stored as `prev_umask`
  and passed unchanged to the second call. No observable
  failure; the umask state is undefined but unobserved.
- `fcntl(fd, F_GETFD)` returns `-1` on ENOSYS. Stratumd's code is
  `if (flags >= 0) (void)fcntl(fd, F_SETFD, ...)`. The F_SETFD
  call is skipped. The CLOEXEC bit is not set on the listener
  fd, but Thylacine has no exec() yet so CLOEXEC is moot.

Both are documented in the patch preamble as "future-lift if a
real consumer materializes". Today their ENOSYS-sentinel posture
is P-3-truthful.

### 0015-pouch-poll-tag.patch

A SECOND blocker surfaced after 0014 unblocked listen: stratumd's
accept loop calls `poll(&pfd, 1, 200)` on the listener fd. The
listener fd is a *tagged* pouch socket fd (`>= 0x40000000 + slot`,
per sub-chunk 12). The kernel's `sys_poll_for_proc` looks up each
`pollfd.fd` in the Proc's handle table; the table tops out at
`PROC_HANDLE_MAX = 64`. A tagged fd at 0x40000000+ is out of range;
`kernel/poll.c::poll_scan_one` writes `POLLNVAL` into revents.

Stratumd's loop:

```c
if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
```

POLLNVAL trips the break, the accept loop returns STM_EBACKEND,
stratumd's main returns 2, the process exits. joey's downstream
wait_pid for stratumd-stub then returns stratumd's pid instead
(both are joey's children; wait_pid is unfiltered); the "wrong
pid" extinction cascade fires the kernel via kproc.wait_pid (also
unfiltered at v1.0).

The fix mirrors the read/write/close dispatch shape from sub-chunk
12: `poll()` checks each input pollfd for a tagged fd; if any are
tagged, walks the array translating tagged fds to their kernel
handles via `pouch_sock_kernel_fd` into a stack-bounded scratch
array `kfds[PROC_HANDLE_MAX=64]`; calls SYS_poll on the scratch;
copies revents back into the caller's array. Untagged-only polls
take the original pristine fast path (zero copy).

| Slot state | kernel_fd outcome | Translated kfds[i].fd | Kernel response |
|---|---|---|---|
| LISTENING | valid Spoor/Srv handle in [0, 64) | translated to kernel handle | normal poll semantics |
| CONNECTED | valid Spoor handle | translated | normal poll semantics |
| FRESH     | -1 (errno = ENOTCONN)  | -1                          | POLLNVAL (fast-path in poll_scan_one fd<0 check) |
| vacant    | -1 (errno = EBADF)     | -1                          | POLLNVAL (same) |

The caller's `fds[i].fd` identifiers are NOT mutated; the
translation lives only in the scratch.

**Why this is NOT audit-bearing**: pure userspace boundary-line
gap-fill. No new kernel surface. The pouch_sock_kernel_fd primitive
is audited (sub-chunk 12); the kernel's `poll_scan_one` is audited
(P5-poll-a + P5-poll-b). 0015 only routes existing audited paths
correctly together. The pattern is identical in shape to the
existing 0006-pouch-sockets dispatch shims for read/write/close.

### Test posture

`599/599 PASS x default + UBSan`. `Thylacine boot OK` reproducibly
across 3+ default runs + 1 UBSan run. Boot log confirms the new
chain:

```
joey: probe /system.key fstat OK size=3656 mode=0o100644
joey: probe /system.key lseek SEEK_END/SEEK_SET OK
stratumd: serving /dev/virtio-blk on /srv/stratum-fs (backlog=16, msize=8388608, ds=1, ro=0)
joey: stratumd-boot /srv/stratum-fs bound after retry 174 (pool mounted via bdev_thylacine)
stratumd-stub: serving on fd 0 (rx) + fd 1 (tx)
stratumd-stub: EOF on rx; exit 0
joey: stub-bringup ok (pipe + spawn + attach + mount + unmount + walk_open + read)
  joey: /joey pid=1524 exited cleanly (status=0)
Thylacine boot OK
```

The 174-retry budget consumed (~1.7 s of joey busy-waiting)
matches stratumd's libsodium init + bdev claim + mount + bind
time. UBSan slows the path enough that BOOT_TIMEOUT=1200 is
required (the 600 s default is at the timing boundary; expect
occasional "not bound" flakes on UBSan with the default timeout
that are NOT regressions — joey's NON-FATAL branch absorbs them
and boot still completes).

### Cross-references

- `usr/lib/pouch/patches/0014-pouch-srv-stubs.patch` — the file
  rewrites.
- `usr/lib/pouch/patches/0015-pouch-poll-tag.patch` — the poll
  dispatch.
- `usr/lib/pouch/patches/series` — both entries appended.
- Stratum source: `~/projects/stratum/v2/src/cmd/stratumd/serve.c`
  `::stm_stratumd_listen_unix` (the call chain) +
  `::stm_stratumd_accept_loop` (the post-listen poll-then-accept
  pattern).
- Kernel poll handler: `kernel/poll.c::sys_poll_for_proc` +
  `poll_scan_one` (the fd-out-of-range POLLNVAL surface).
- Sub-chunk 12 audit closed list:
  `memory/audit_p6_pouch_sockets_closed_list.md` (poll-on-tagged-
  fd gap NOT caught at sub-chunk 12; closed here).

### What 16c-pre does NOT do

- Does not retire the joey stratumd-boot probe's 6000-retry
  workaround. Sub-chunk 16c lifts that when the kernel 9P client
  + real `t_srv_connect` lands.
- Does not flip the joey probe to FATAL on miss. Same scope as
  above; 16c lift.
- Does not touch kernel surfaces. The `pouch_sock_kernel_fd`
  primitive + SYS_poll handler remain unchanged.
- Does not address the documented v1.x lifts (kproc.wait_pid pid
  filter; cross-thread shootdown via SYS_EXIT_GROUP; structured
  exit_status). The wait_pid race window is closed *contingently*
  in this checkpoint by stratumd staying alive in its accept
  loop — no zombie surfaces — but the architectural fix is still
  v1.x.

### Next: sub-chunk 16c

With stratumd serving 9P over /srv/stratum-fs reliably (accept
loop active), 16c lands the kernel 9P client + `/sysroot` mount
+ ramfs pivot + stub retire. The headline Phase 6 deliverable
("Thylacine boots from a disk-backed Stratum FS over real 9P")
is one audit-bearing sub-chunk away.
