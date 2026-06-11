# HOLOTYPE RW-8 -- Userspace runtime (libthyla-rs + pouch + virtio drivers + Stratum bdev arm)

**Tier**: STANDARD (lenses S + C + area-local T). **Status**: CLOSED (dirty
close; converged round-2). **Fix commits**: Thylacine `cc79551`; Stratum
(`thylacine-pouch-arm`) `bf0cde0`. **Report commit**: *(pending)*.

## Surface

The native userspace runtime every Thylacine-authored program builds against,
plus the boundary-line every ported program shares:

- **libthyla-rs** (~9.7k LOC, 25 modules): alloc, lib.rs (the syscall FFI),
  io, process, thread, handle, err, env, time, rand, fs/*, notes, poll,
  torpor, territory, cap, ninep, hardware, loom.
- **The pouch boundary-line** (15 patches, ~5.4k LOC): the musl translation
  layer -- syscall-seam, sockets (SO_PEERCRED), signals, pthread, poll,
  openat, fstat/lseek, mman, abort/mallocng, srv-stubs.
- **The virtio-* userspace drivers** (7, ~5.5k LOC): blk-rw, input, gpu,
  blk-probe, net-{arp,loop,probe}.
- **The Stratum bdev arm** (`src/block/bdev_thylacine.c`, 879 LOC, the
  Thylacine-facing seam -- ours per stewardship).

### Soundness model (the severity calibration)

Userspace runs OVER a kernel that validates every syscall arg, enforces
rights, and owns handle/fd identity. So a libthyla-rs / virtio bug is
**process-local** (one-process blast radius) -- rated P1/P2, never P0 by
itself. Three exceptions carry higher leverage: the **pouch boundary-line**
(multiplied across every port), the **SO_PEERCRED identity channel** (A-3),
and the **kernel-side** half of a fix (the NDFLT death path can reach a
kernel-invariant break).

## Reviewers

5 Fable holotype-reviewers (split by sub-surface, never by lens) + Opus
self-audit, then a dirty-close round-2 on the fixes:

| Reviewer | Sub-surface | MODEL(start)==end |
|---|---|---|
| R1 | libthyla-rs core (alloc/FFI/io/process/thread/handle) | claude-fable-5 == |
| R2 | libthyla-rs fs + wait-side + cap + 9p | claude-fable-5 == |
| R3 | hardware substrate + Loom (DELTA) | claude-fable-5 (end; start line omitted) |
| R4 | virtio-* drivers + Stratum bdev arm | claude-fable-5 == |
| R5 | the pouch boundary-line patches | claude-fable-5 == |
| RND2 | dirty-close re-audit on the fixes | *(see round-2)* |

The two highest-stakes surfaces R5 was told to hunt came back **clean**: the
syscall-number table (the native `T_SYS_*` mirror AND the composed pouch
table -- 31 retargets, zero drift vs the kernel) and the SO_PEERCRED
principal marshal (field-correct, no zero/stale path on any return). The
recurrent HOLOTYPE theme recurred a fifth time, now on the pouch boundary:
**each layer's boundary was correct when it landed, then the substrate moved
(the P6 multi-thread lift, #809/#811 group-terminate, A-3 identity, stalk
SYS_OPEN, LS-4 cwd) without a per-layer revisit** -- R5-F1/F5/F7 are all that
one failure mode.

## Round-1 findings + dispositions

### P1 (1) -- FIXED

- **R1-F1 / R2-F1 [S]** (triple-converged R1 + R2 + self): `io.rs`
  `BufReader::fill_buf` exposed **uninitialized heap** on the inner-read
  error leg. `set_len(cap)` exposes the Vec's spare capacity; `read()?` on
  the Err leg skipped the `set_len(n)` truncation, leaving `len==cap` with an
  uninitialized tail and `pos` unchanged; the next `fill_buf` saw `pos < len`,
  skipped the refill, and returned `&buf[pos..]` over uninit memory -- UB +
  a process-local info-leak of a recycled allocation (a prior freed
  `String`/key buffer). LATENT (no in-tree BufReader consumer -- R1's
  shell/login-drive-it claim was WRONG; `usr/login/src/main.rs:108` has its
  own raw-fd `read_line`; the reachable-but-undriven rule holds it at P1
  since BufReader is the designated shell line buffer). **Fix**: the Err leg
  resets to empty (`set_len(0); pos=0`); the Ok leg clamps `n.min(cap)`
  (folds R2-F2). **Regression**: `u-test::bufreader_error_leg` -- a scripted
  Read yields Ok->Err->Ok; the post-error read must re-enter inner (reads==3)
  and return the new bytes, not the stale tail (pre-fix: the 3rd `fill_buf`
  finds pos<len, never re-enters inner -> reads==2 -> assert fails).

### P2 (6) -- all FIXED

- **R5-F1 [S]** kernel `sys_noted_handler` NDFLT: a multi-thread Proc refused
  the default-terminate (`-1`), and pouch's bootstrap (always-installed
  handler -> bypasses the LS-5 kernel default-terminate) NCONT-resumed ->
  **a multi-thread pouch daemon was un-terminatable through the entire POSIX
  signal surface**. The refusal predated #809/#811; `exits()` with live peers
  now cascades (proc.c:1712-1726). **Fix**: drop the live-peers refusal,
  call `notes_noted_default(t)` -> `exits()` -> the #811 cascade. (Cleaner
  than the boundary-line option; fixes all `SYS_NOTED` users.)
- **R5-F2 [S]** the `tools/build.sh` seam check (the structural anti-misroute
  gate) had drifted to miss **13 live retargets** (mmap/munmap, srv_accept/
  peer, the 6 HW nums, walk_open, fstat/lseek) -- 0008/0009/0010 never
  extended it. `mmio_map(5)` and `dma_map(7)` share an arg shape, so a
  re-vendor swap would misroute silently. **Fix**: + the 13 entries + the
  fstat/lseek `#undef` (so a dropped 0010 redefine, leaving 0xFFFF to win,
  fails the gate not the runtime).
- **R5-F3 [C]** pouch `0015` `poll()`: a POSIX-ignored negative fd (the
  negate-to-disable / unused-slot=-1 idiom) returned **POLLNVAL-ready** --
  the kernel returned immediately on it, defeating the timeout, busy-spinning
  the caller (or reading as a fatal error). **Fix**: compact the active
  (`fd>=0`) entries OUT into a `map[]`-tracked kernel array so the ignored
  entries never reach the kernel poll; genuinely-invalid TAGGED slots still
  yield POLLNVAL (the EBADF surface).
- **R4-F1 [S, P1-borderline]** Stratum bdev `do_request` left `avail_idx`
  desynced from the device on **any** error (the device saw `avail.idx=V+1`,
  the bdev kept `V`): a retry re-published the same idx -> the FS server
  **hung**; or it consumed the prior request's completion -> **silent
  stale-read / mis-acked write** (a storage-integrity defect Stratum surfaces
  as STM_ECORRUPT). **Fix**: latch `d->failed` (calloc-zeroed; `goto io_fail`
  on every error) -> all later ops return STM_EIO -> Stratum re-opens.
- **R4-F2 [S]** virtio-input checked `(desc_id as u16) >= QUEUE_SIZE` but
  indexed the event pool with the full `u32` -> `0x10000` truncates to 0,
  passes, indexes 512 KiB OOB. **Fix**: full-width compare.
- **R4-F3 [S]** Stratum bdev `op_fsync` was a no-op and `VIRTIO_BLK_F_FLUSH`
  unnegotiated, but the launch attached drives `cache=writeback` -> fsync
  persisted nothing -> host-crash lost committed writes. **Fix**:
  `cache=writethrough` on the boot drives (run-vm.sh) makes the no-op fsync
  correct + the false "write-through" comment fixed (Stratum). F_FLUSH
  negotiation is the registered v1.x perf-preserving version.

### P3 -- fixed

R1-F2 (BufReader `with_capacity(0)` -> false-EOF; floored at 1); R2-F2
(read-count clamp, folded into R1-F1); R2-F3/SA-1 (notes `try_read`
poll-then-blocking-read single-consumer race -> doc + register the kernel
nonblocking-note-read); R1-F3/SA-2 (thread.rs stale "exits extincts on live
peers / v1.x SYS_EXIT_GROUP" doc -- both already landed); R3-F1 (Dma
`as_slice` aliasing-over-DMA caveat); R3-F2 (Dma `read_u32` `compiler_fence`
is compiler-only, hardware `virtio_rmb` mandatory); R4-F4 (net-arp/loop RX
`desc_idx` OOB bound); R4-F5 (virtio-input/gpu bank-0 features read); R4-F6
(blk-probe `virtio_rmb`); R5-F7 (78-pouch.md SO_PEERCRED `uid=gid=0` stub doc
-> the real A-3 principal).

### P3 / H -- registered (not fixed in-arc)

| ID | Lens | Why registered |
|---|---|---|
| R3-F3 | H | typed Mmio/Dma `Drop` leaks the VMA on a churning driver -- dynamic-DMA-future, no v1.0 consumer |
| R4-F7 | C | bdev hand-rolled non-ISV-safe MMIO -- VERIFIED DORMANT (objdump: current codegen is ISV-safe); the structural fix is Stratum hardening |
| R5-F4 | S | pouch_srv_peer_info + t_stat mirrors pin size not field offsets -- the A-3 drift defense; offsetof asserts (the Loom-6c/6d standard) |
| R5-F5 | C | 0009 openat stale (per-component final-omode breaks multi-component O_WRONLY; open("/")->ENOENT; relative->ENOTSUP) -- delegate to SYS_OPEN; a substantial shim rewrite |
| R5-F3b | C | the 0005 select() POLLNVAL->EBADF sibling of R5-F3 |
| R5-F6 | C | stdio direct-syscall bypasses the socket tag -> fdopen-on-socket breaks + leaks the slot (structural; v1.x) |
| R5-F8 | C | getpid/getuid/... return -ENOSYS garbage (4294967258) -- needs a getpid surface |
| R5-F9 | S | siglongjmp out of a handler wedges note delivery (in_handler stuck) -- doc + v1.x |
| SA-3 | C | print!/println! swallow write errors (a coreutil-output footgun) |
| SA-4 | C | alloc 4 MiB fixed heap, no growth (OOM -> clean exit) |

The registered pouch items (R5-F4/F5/F3b/F6/F8/F9 + the 0007/0011/0012
patch-header stale-comment sweep) form one coherent **"pouch boundary-line
POSIX-coherence + drift-defense revisit"** follow-up -- R5's own framing.

## Verified sound (reviewed + survived; what the next reader can trust)

- **Syscall routing**: the native `T_SYS_*` mirror (self) AND the composed
  pouch table (R5) both ZERO-drift vs `kernel/include/thylacine/syscall.h`;
  retired numbers 26/30/43 correctly absent; the 0xFFFF sentinel guarded at
  both chokepoints.
- **The A-3 identity channel** (R5): the SO_PEERCRED principal marshal is
  field-correct (`ucred.uid = principal_id`, `.gid = primary_gid`), no
  zero/stale path; client-side SO_PEERCRED -> ENOTSOCK by design.
- **MMIO ISV-safety** (R3): structural -- the 8 hardware primitives are
  single base-only `ldr/str` (#890 closed by construction); all 7 Rust
  drivers delegate; bdev's hand-rolled accessors are codegen-ISV-safe today
  (objdump-confirmed, R4-F7 dormant). The Drop-closes-fd-keeps-VMA shape
  composes with the landed RW-7 `proc_quiesce_owned_devices` p->vmas walk.
- **Loom DELTA** (R3): the only change since the 6d close is the `offset_of!`
  asserts (all verified vs loom.h); the SPSC happens-before independently
  re-derived sound.
- **Allocator** thread-safety (LockedHeap spin::Mutex) + the lazy-init CAS
  (self + R1); **process.rs** fd-passing leak-free + double-close-free on
  every path + the A-5a supp_gids lifetime (self + R1); **err.rs** decodes
  -1 and -errno with overflow-safe saturation (R1); **pthread** SP-align +
  clear_child_tid + __unmapself (R5); the **bdev write/RMW/barrier/lifetime**
  + all 7 drivers' VirtIO init state machines (R4); **ninep** parsers all
  bounds-checked, **cap.rs** forwards scalars only (R2).

## Round-2 (dirty-close re-audit on the fixes)

A Fable holotype-reviewer (claude-fable-5) prosecuted the four fixes for
fixes-introduced defects: **0 P0 / 0 P1 / 1 P2 / 2 P3.** It EARNED ITS ROUND --
the three structurally-load-bearing fixes (kernel NDFLT cascade, bdev latch,
BufReader reset) survived deep prosecution intact, and it caught one real
fix-introduced defect:

- **RND2-F1 [P2]** the R5-F3 poll() compaction produced `nk==0` for the
  all-negative-fds boundary (the all-disabled case of the very idiom it
  shipped to support); `syscall_cp(SYS_poll, kfds, 0, timeout)` hits the
  kernel's `nfds==0` rejection (-1 -> EPERM) where POSIX wants ignore-all +
  return 0 -- the round-1 busy-spin traded for a different wrong, with false
  comments. **FIX** (`9db9300`): the n==0 and nk==0 legs return 0 for a
  nonblocking probe + ENOSYS for a timed/indefinite wait (matching 0005
  select()); both comments corrected.
- **RND2-F2 [P3]** the 0007 signals bootstrap still documented the retired
  kernel NDFLT refusal (cc79551 fixed the kernel comment + 78-pouch.md but not
  0007) -- the stale-cross-layer-contract class that let R5-F1 lie dormant.
  **FIX** (`9db9300`): rewritten to the post-cc79551 contract.
- **RND2-F3 [P3, registered]** the kernel NDFLT fix landed without a
  regression (a multi-thread uncaught-terminate E2E needs a new
  multi-thread-blocking pouch binary; the fix is verified sound by the round-2
  deep prosecution). Tracked (task #46).

The reviewer's deep verified-sound on the fixes: the **NDFLT cascade** composes
(no lock imbalance; `in_handler` dead on the death path; I-24 die-check-before-
deliver ordering; concurrent peer-NDFLT serialized to exactly-once ZOMBIE; no
lost wake per the #811 register-then-observe); the **bdev latch** is
complete / one-way / lock-clean (every post-publish error reaches io_fail; no
spurious latch on success; avail_idx frozen; re-create RESETs the device ring
in lockstep); the **BufReader** invariant holds on every exit. The reviewer
judged RND2-F1's fix localized (a shim special-case + comments; no wait/wake or
death-path restructure) -> **no round-3**.

## Verdict

**CLOSED -- dirty close, converged over round-2.** 1 P1 + 6 P2 + a P3 cluster
(round-1) + 1 P2 + 1 P3 (round-2 on the fixes) all FIXED; RND2-F3 + the round-1
registered cluster tracked (task #46). 0 P0 / 0 P1 remaining. The two
highest-stakes surfaces (the syscall-number table; the SO_PEERCRED identity
channel) verified clean. The recurrent P6-multi-thread-lift / substrate-moved
theme recurred a fifth time (now the pouch boundary) and is closed on this
surface.

## Posture

- default `823/823 PASS` + `u-test::bufreader_error_leg OK` + `u-test all OK`
  + `/sbin/login` E2E (the FS path provisions + unlocks the per-user DEK home
  through bdev_thylacine) + `Thylacine boot OK` + 0 EXTINCTION.
- Full build clean (kernel + sysroot incl. the extended seam check + stratumd
  with the bdev changes + disk).
- SMP gate: **PASS -- 0 corruption across all 4 configs** (default+UBSan x
  smp4/smp8, N=10 each; 9 clean boots in the heavier smp8/UBSan configs, the
  rest host-timing-inconclusive under the concurrent-reviewer host load --
  0 corruption is the pass criterion).
- The virtio-input QMP-injection harness SKIP is the host-side #34 flake (the
  driver init reached AWAITING_QMP_KEY + reaped status=0; the host QMP
  send-key is absent in this environment).
