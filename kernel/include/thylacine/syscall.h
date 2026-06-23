// Userspace syscall dispatch (P3-Ec).
//
// Per ARCHITECTURE.md §13 (syscalls). Plan 9 Thylacine uses a small
// syscall surface — at v1.0 P3-Ec the absolute minimum to demonstrate
// userspace runs:
//
//   SYS_EXITS(status)    — terminate calling process. Maps to kernel
//                           exits("ok"/"fail") based on status==0 / !=0.
//                           Never returns.
//   SYS_PUTS(buf, len)   — write `len` bytes from `buf` to UART for
//                           operator visibility / test diagnostic.
//                           Returns `len` on success, -1 on argument
//                           validation failure (NULL / oversized).
//
// AArch64 ABI (matches Linux for familiarity):
//   x8       = syscall number
//   x0..x5   = arguments (positional)
//   x0       = return value
//
// EL0 issues `svc #imm` (imm currently ignored at v1.0 — Phase 5+ may
// use it as a class selector). The hardware delivers a sync exception
// at EL1 with EC=0x15 (EC_SVC_AARCH64). exception_sync_lower_el routes
// to syscall_dispatch.
//
// Phase 5+ extends with the full Thylacine syscall set: open / close /
// read / write / mount / bind / rfork / wait / mmap / munmap /
// notify / etc. Each syscall lands in its own translation unit; this
// header just lists numbers + the dispatch entry.

#ifndef THYLACINE_SYSCALL_H
#define THYLACINE_SYSCALL_H

#include <thylacine/errno.h>
#include <thylacine/types.h>

// Syscall numbers. v1.0 P3-Ec stable; new syscalls append.
enum {
    SYS_EXITS       = 0,
    SYS_PUTS        = 1,
    // P4-Ib: hardware-handle creation + IRQ wait. Caller must hold
    // CAP_HW_CREATE (proc->caps). Returns hidx_t handle index (>=0)
    // on success, -1 on permission denied / resource exhausted / arg
    // validation failure. Maps to specs/handles.tla HandleAlloc with
    // k \in HwKObjs precondition.
    SYS_MMIO_CREATE = 2,    // arg: pa (x0), size (x1), rights (x2)
    SYS_IRQ_CREATE  = 3,    // arg: intid (x0), rights (x1)
    SYS_IRQ_WAIT    = 4,    // arg: handle (x0)
    // P4-Ic2: install a user-VA mapping for a KObj_MMIO handle. arg:
    // handle (x0), vaddr (x1), prot (x2). The Burrow + VMA carry the
    // KObj_MMIO ref so the mapping lifetime is bound to the proc's
    // address space (proc exit → VMA tear-down → burrow_release_mapping
    // → final unref of the kobj_mmio).
    SYS_MMIO_MAP    = 5,    // arg: handle (x0), vaddr (x1), prot (x2)
    // P4-Ic5b1b: DMA-buffer allocation + mapping. Caller must hold
    // CAP_HW_CREATE. Distinct from MMIO at the syscall level because
    // the kernel chooses the PA (via alloc_pages) instead of the caller
    // specifying it.
    //
    // SYS_DMA_CREATE allocates a contiguous pinned page chunk of `size`
    // bytes (page-aligned, ≤ KOBJ_DMA_MAX_SIZE) and returns a KOBJ_DMA
    // handle. The PA is stable for the handle's lifetime; the kernel
    // doesn't migrate the pages.
    //
    // SYS_DMA_MAP installs a user-VA mapping for an existing KOBJ_DMA
    // handle and returns the underlying PA so the driver can embed it
    // into device-visible descriptors (VirtIO virtqueue rings etc.).
    // Returns a non-negative PA on success; -1 on failure. PA fits in
    // 40 bits (IPS bound at v1.0) so the sign bit is never set for a
    // valid PA; negative return is unambiguously an error.
    SYS_DMA_CREATE  = 6,    // arg: size (x0), rights (x1)
    SYS_DMA_MAP     = 7,    // arg: handle (x0), vaddr (x1), prot (x2)

    // P5-fd-pipe: create a connected Spoor pair via `pipe_create()`,
    // install both as KOBJ_SPOOR handles in the caller's HandleTable.
    // No arguments; returns the read-end fd in x0 and the write-end fd
    // in x1. On failure (handle table full, OOM) returns x0 = -1 and
    // both Spoors are clunked on the way out.
    //
    // This is the first userspace consumer of KOBJ_SPOOR. The handle-
    // table release path (handle_release_obj at P5-fd-pipe) calls
    // spoor_clunk on KOBJ_SPOOR, so closing the handle (or the Proc
    // exiting) tears down the underlying Spoor end-to-end.
    SYS_PIPE        = 8,    // no args; returns rd in x0, wr in x1

    // P5-fd-rw: read / write through a KOBJ_SPOOR fd. Each routes
    // through the Spoor's Dev vtable (`dev->read` / `dev->write`)
    // after validating the user-VA buffer and the handle's rights
    // (RIGHT_READ for SYS_READ; RIGHT_WRITE for SYS_WRITE).
    //
    // SYS_READ(fd, buf_va, len)  → bytes read (>=0), 0 on EOF, -1 on error
    // SYS_WRITE(fd, buf_va, len) → bytes written (>=0), -1 on error
    //
    // Length is capped at SYS_RW_MAX (4096) per call — userspace
    // loops for larger transfers. Bouncing through a 4 KiB kernel
    // stack scratch buffer; uaccess_load_u8 / uaccess_store_u8 do
    // the per-byte user-VA copy with fault-fixup.
    SYS_READ        = 9,    // arg: fd (x0), buf_va (x1), len (x2)
    SYS_WRITE       = 10,   // arg: fd (x0), buf_va (x1), len (x2)

    // P5-fd-close: thin wrapper over handle_close. Returns 0 on
    // success or -1 if the fd is invalid / not present. For
    // KOBJ_SPOOR handles, handle_release_obj (wired at P5-fd-pipe)
    // routes to spoor_clunk → frees the underlying Spoor on last ref.
    //
    // P5-fd-dup: wraps handle_dup; allocates a new handle slot
    // pointing at the same kobj as `oldfd` with `new_rights`
    // (subset of the original's rights — elevation is rejected).
    // For KOBJ_SPOOR, handle_acquire_obj (wired at P5-fd-pipe) calls
    // spoor_ref so the dup'd handle has its own reference.
    SYS_CLOSE       = 11,   // arg: fd (x0)
    SYS_DUP         = 12,   // arg: oldfd (x0), new_rights (x1)

    // P5-attach-syscall: wrap a byte-pipe Spoor pair (tx + rx) in a
    // p9_client + drive the Tversion + Tattach handshake; return a
    // KOBJ_SPOOR fd for the resulting 9P tree's root (a dev9p Spoor
    // whose close tears down the entire attach session).
    //
    // For duplex byte pipes (Unix socket, vsock — Phase 5+), userspace
    // passes the same fd as both tx_fd and rx_fd. For half-duplex
    // (Plan 9 pipes from SYS_PIPE), userspace creates two pipe pairs
    // and passes the matching write-end and read-end.
    //
    // SYS_ATTACH_9P(tx_fd, rx_fd, aname_va, aname_len, n_uname)
    //   x0 = tx_fd (client→server byte pipe)
    //   x1 = rx_fd (server→client byte pipe)
    //   x2 = aname_va (user-VA pointer to the attach name string)
    //   x3 = aname_len
    //   x4 = n_uname (u32; 0 for no-auth attach at v1.0)
    // Returns: x0 = new fd (>=0) on success; -1 on:
    //   - invalid tx_fd or rx_fd (not KOBJ_SPOOR / out-of-range)
    //   - missing RIGHT_READ on rx_fd / RIGHT_WRITE on tx_fd
    //   - aname_va outside user-VA bound / aname_len > SYS_ATTACH_ANAME_MAX
    //   - kmalloc OOM for adapter / p9_attached_create handshake failure
    //   - handle table full
    SYS_ATTACH_9P   = 13,   // arg: tx_fd, rx_fd, aname_va, aname_len, n_uname

    // P5-mount-syscall: graft a Spoor's tree at a target path in the
    // caller's Territory mount table. The source Spoor can be ANY
    // KOBJ_SPOOR — a dev9p-backed root from SYS_ATTACH_9P, a kernel
    // synthetic Dev root, a pipe end (degenerate but legal — walking
    // a pipe-as-mount mostly produces -1, but the lifetime discipline
    // composes regardless), or a future cross-territory share.
    //
    // SYS_MOUNT(path_va, path_len, source_spoor_fd, flags) → 0/-1
    //   x0 = path_va  (user VA of the absolute mount-point path)
    //   x1 = path_len (1 .. SYS_OPEN_PATH_MAX; bytes, NUL-free)
    //   x2 = source_spoor_fd (hidx_t; must be a KOBJ_SPOOR handle)
    //   x3 = flags (u32; MREPL / MBEFORE / MAFTER / MCREATE)
    // stalk-2: path-keyed (was an abstract target_path_id). The kernel
    // `stalk`s `path` from the caller's Territory root to the mount-point
    // Spoor (STALK_MOUNT: resolve, do NOT cross the final mount, do NOT
    // open -- so re-mounting onto an already-mounted point MREPL-replaces
    // it) and records the mount keyed by the mount point's
    // (dc, devno, qid.path) identity. The MOUNT POINT MUST EXIST as a
    // walkable directory (Plan 9 M1; devramfs ships /srv + /proc, the
    // disk FS provides its own). Resolves from root only at v1.0 (absolute
    // paths); a relative-mount start_fd is a v1.x add.
    // Returns: 0 on success, -1 on:
    //   - path absent / empty / too long / not resolvable / NUL-embedded
    //   - invalid source_spoor_fd (not KOBJ_SPOOR, out-of-range)
    //   - missing RIGHT_READ on the source (it must be consumable as a tree)
    //   - flags has bits outside the MREPL|MBEFORE|MAFTER|MCREATE set
    //   - territory mount table full (PGRP_MAX_MOUNTS reached)
    //
    // Lifecycle (per ARCH §9.6.6): `mount` bumps the source Spoor's refcount
    // (the mount-table entry holds its own ref). The caller can close
    // their source_spoor_fd afterward; the mount table keeps the Spoor
    // alive. unmount() (or Territory destruction) drops the per-entry
    // ref; if it was the last ref, the Spoor's Dev close runs (which,
    // for dev9p-backed Spoors set up by SYS_ATTACH_9P, tears down the
    // entire 9P session). The transient mount-point Spoor is clunked
    // immediately -- the table keeps only its identity, not the Spoor.
    SYS_MOUNT       = 14,   // arg: path_va, path_len, source_spoor_fd, flags

    // SYS_UNMOUNT(path_va, path_len) → 0/-1
    //   x0 = path_va  (user VA of the absolute mount-point path)
    //   x1 = path_len (1 .. SYS_OPEN_PATH_MAX)
    // stalk-2: path-keyed. The kernel `stalk`s `path` (STALK_MOUNT) to the
    // mount point's own identity and removes the FIRST mount entry matching
    // (dc, devno, qid.path).
    // Returns: 0 on success, -1 on:
    //   - path absent / empty / too long / not resolvable
    //   - no entry at that mount-point identity in the caller's Territory
    //
    // Drops the per-entry Spoor ref; the Spoor's Dev close runs if
    // this was the last ref.
    SYS_UNMOUNT     = 15,   // arg: path_va, path_len

    // P5-corvus-syscalls: v1.0 hardening syscalls per CORVUS-DESIGN.md
    // §4.1.1 + ARCH §11.2b. corvus + per-user stratumd call these at
    // startup; ordinary user procs use them at discretion. Each sets
    // a one-way per-Proc flag (PROC_FLAG_*) or performs a one-shot
    // action (explicit_bzero, getrandom).

    // SYS_MLOCKALL(flags) → 0/-1
    //   x0 = flags (u32; currently unused, reserved for MCL_CURRENT /
    //                MCL_FUTURE distinction at v1.x when swap exists)
    // Pin all currently-mapped and future-mapped pages. Caller must
    // hold CAP_LOCK_PAGES. Sets PROC_FLAG_MLOCKED on the Proc. v1.0
    // has no swap; the flag is forward-compat scaffolding consumed by
    // corvus + per-user stratumd at startup. Returns 0 on success, -1
    // on missing cap.
    SYS_MLOCKALL     = 16,   // arg: flags (x0)

    // SYS_SET_DUMPABLE(dumpable) → 0/-1
    //   x0 = dumpable (u32; 0 = disable core dump, 1 = enable [default])
    // One-way: setting to 0 sets PROC_FLAG_NODUMP. Setting to 1 from a
    // Proc that already has PROC_FLAG_NODUMP set is REFUSED (-1). v1.0
    // has no core dumps; the flag is forward-compat scaffolding.
    SYS_SET_DUMPABLE = 17,   // arg: dumpable (x0)

    // SYS_SET_TRACEABLE(traceable) → 0/-1
    //   x0 = traceable (u32; 0 = refuse future debug-Spoor attach,
    //                       1 = allow [default])
    // One-way: setting to 0 sets PROC_FLAG_NOTRACE. Setting to 1 from
    // a Proc that has PROC_FLAG_NOTRACE set is REFUSED. v1.0 has no
    // debug Spoors; the flag is forward-compat scaffolding.
    SYS_SET_TRACEABLE = 18,  // arg: traceable (x0)

    // SYS_EXPLICIT_BZERO(buf_va, len) → 0/-1
    //   x0 = buf_va (user-VA; same bound checks as SYS_PUTS)
    //   x1 = len (bytes; ≤ SYS_RW_MAX = 4096 per call)
    // Compiler-barrier'd memset of the user-VA buffer to zero. Used by
    // corvus + per-user stratumd to wipe secrets without the optimizer
    // eliding the memset. Returns 0 on success, -1 on user-VA bound
    // violation. Length cap matches SYS_PUTS / SYS_RW_MAX; userspace
    // loops for larger buffers.
    SYS_EXPLICIT_BZERO = 19, // arg: buf_va (x0), len (x1)

    // SYS_GETRANDOM(buf_va, len, flags) → bytes_read / -1
    //   x0 = buf_va (user-VA destination)
    //   x1 = len (bytes; ≤ SYS_RW_MAX = 4096 per call)
    //   x2 = flags (u32; 0 = block until kernel CSPRNG seeded [default];
    //                    GRND_NONBLOCK (= 1) = return -1 if not seeded)
    // Read `len` bytes from the kernel CSPRNG into the user-VA buffer.
    // Caller must hold CAP_CSPRNG_READ. Returns bytes read (= len on
    // success) or -1 on: missing cap / bad user-VA / CSPRNG not seeded
    // (when GRND_NONBLOCK set) / CSPRNG hardware fault. v1.0 backend
    // is ARM RNDR (FEAT_RNG); always-seeded if available. Each call is
    // a fresh CSPRNG read — no caching.
    SYS_GETRANDOM    = 20,   // arg: buf_va (x0), len (x1), flags (x2)

    // SYS_SPAWN(name_va, name_len) → child_pid / -1
    //   x0 = name_va (user-VA; NUL not required — name_len authoritative)
    //   x1 = name_len (bytes; 1..SYS_SPAWN_NAME_MAX = 64)
    // Look up the binary by name in the boot initrd (devramfs), allocate
    // a fresh child Proc via rfork(RFPROC, ...), exec_setup the binary
    // into the child, and return its PID. Returns -1 on:
    //   - name_len out of range / name_va bound violation
    //   - binary not resolvable in the caller's namespace (stalk miss / X-deny)
    //   - executable exceeds EXEC_FILE_MAX (256 MiB sanity ceiling; REVENANT R-4
    //     retired the old 1-MiB SYS_SPAWN_BLOB_MAX slurp cap)
    //   - kmalloc / rfork OOM
    //   - exec_setup_from_spoor failure (child exits "fail-exec" → parent's
    //     SYS_WAIT_PID observes non-zero status)
    // The child inherits no capabilities (CAP_ALL & 0u = 0). v1.0 model
    // is "the child fully describes its needs"; future SYS_RFORK with a
    // cap-mask argument lands at P5-spawn-caps when needed.
    SYS_SPAWN        = 21,   // arg: name_va (x0), name_len (x1)

    // SYS_WAIT_PID(want_pid, flags, status_out_va) → reaped_pid / 0 / -1
    //   x0 = want_pid (-1 = any child; >0 = that child; 0/<-1 reserved →
    //                  match nothing → -1)
    //   x1 = flags (WAIT_WNOHANG = 1: do not block — return 0 if no
    //               matching zombie is ready; any other bit set → -1)
    //   x2 = status_out_va (user-VA; 4-byte int destination, or 0 to skip)
    // Reap a ZOMBIE child, optionally filtered by pid and/or non-blocking.
    // Writes the child's exit_status to *status_out_va (if non-zero) on a
    // successful reap. Returns the reaped PID (>0); 0 under WAIT_WNOHANG
    // when a matching child is alive but not yet a zombie (0 is never a
    // valid child pid); -1 if there is no matching child OR a flag bit
    // outside WAIT_WNOHANG is set. Mirrors kernel/proc.c::wait_pid_for
    // (POSIX waitpid(pid, flags) shape; the WAIT_WNOHANG value lives in
    // proc.h, mirrored by the userspace libs). NB (v2 ABI): x1/x2 are NEW —
    // any caller not going through the libt / libthyla-rs wrapper MUST set
    // (zero) them; a stale-register x1/x2 from a pre-v2 binary is rejected
    // (garbage flags → -1) rather than silently reaping the wrong child.
    SYS_WAIT_PID     = 22,   // args: want_pid (x0), flags (x1), status_out_va (x2)

    // SYS_SPAWN_WITH_FDS(name_va, name_len, fd_list_va, fd_count) → child_pid / -1
    //   x0 = name_va        user-VA pointer to the binary name
    //   x1 = name_len       bytes; 1..SYS_SPAWN_NAME_MAX = 64
    //   x2 = fd_list_va     user-VA pointer to u32[fd_count] array (or 0 if fd_count==0)
    //   x3 = fd_count       0..SYS_SPAWN_MAX_FDS = 16
    // Like SYS_SPAWN, but with explicit fd inheritance: the listed fds
    // (which must all be KOBJ_SPOOR in the caller's handle table at
    // v1.0) are installed in the spawned child's handle table at
    // slots 0..fd_count-1 BEFORE exec_setup. The child's main() finds
    // those fds pre-populated. The parent retains its own holds on the
    // same handles (caller-side refcount unchanged); the inheritance
    // is "give the child its own ref," not "transfer."
    //
    // KOBJ_SPOOR-only at v1.0 because (a) it covers the production
    // use case (pipes + 9P transports) and (b) ARCH I-5 prohibits
    // KOBJ_MMIO / KOBJ_IRQ / KOBJ_DMA from cross-Proc transfer.
    // KOBJ_BURROW inheritance lands when a v1.x workload needs it.
    //
    // Returns -1 on:
    //   - same as SYS_SPAWN (name validation / binary lookup / blob size / OOM)
    //   - fd_count > SYS_SPAWN_MAX_FDS
    //   - fd_list_va bound violation (when fd_count > 0)
    //   - any fd in the list is not a valid open handle in the caller
    //   - any fd in the list is not KOBJ_SPOOR
    SYS_SPAWN_WITH_FDS = 23, // arg: name_va, name_len, fd_list_va, fd_count

    // SYS_SPAWN_WITH_CAPS(name_va, name_len, cap_mask) → child_pid / -1
    //   x0 = name_va        user-VA pointer to the binary name
    //   x1 = name_len       bytes; 1..SYS_SPAWN_NAME_MAX = 64
    //   x2 = cap_mask       caps_t bitmask of caps to grant the child
    // Like SYS_SPAWN, but the child's caps are `parent->caps & cap_mask`
    // (not zero). Wraps the existing kernel-internal `rfork_with_caps`
    // which enforces ARCH I-2 / I-6 monotonic-reduction: even if
    // cap_mask sets bits the parent doesn't hold, the AND clamps to
    // the parent's actual caps. v1.0 use case: joey spawns corvus
    // with cap_mask = CAP_LOCK_PAGES | CAP_CSPRNG_READ (a subset of
    // joey's CAP_ALL).
    //
    // The child inherits no Spoor handles (use SYS_SPAWN_WITH_FDS
    // or future SYS_SPAWN_FULL if both inheritance modes are needed).
    //
    // Returns -1 on:
    //   - same as SYS_SPAWN (name validation / binary lookup / blob / OOM)
    //   - cap_mask outside the u64 range of caps_t (validation already
    //     happens via the cast; no separate rejection)
    SYS_SPAWN_WITH_CAPS = 24, // arg: name_va, name_len, cap_mask

    // SYS_SPAWN_FULL(name_va, name_len, fd_list_va, fd_count, cap_mask)
    //   x0 = name_va
    //   x1 = name_len
    //   x2 = fd_list_va    user-VA to u32[fd_count] (or 0 if fd_count==0)
    //   x3 = fd_count      0..SYS_SPAWN_MAX_FDS = 16
    //   x4 = cap_mask      caps_t bitmask; child gets parent->caps & mask
    // Combination of SYS_SPAWN_WITH_FDS + SYS_SPAWN_WITH_CAPS: child gets
    // both the listed Spoor fds (slots 0..fd_count-1) AND the cap-subset.
    // Needed at P5-corvus-bringup where joey spawns /sbin/corvus with a
    // pipe pair (for login communication) AND CAP_LOCK_PAGES +
    // CAP_CSPRNG_READ. The previous variants stay because each call site
    // expresses intent more clearly than passing zeros to SYS_SPAWN_FULL.
    //
    // Returns -1 on the union of SYS_SPAWN_WITH_FDS and SYS_SPAWN_WITH_CAPS
    // failure conditions.
    SYS_SPAWN_FULL   = 25,   // arg: name_va, name_len, fd_list_va, fd_count, cap_mask

    // 26 — RETIRED (stalk-3c). Was SYS_POST_SERVICE. Posting a /srv service
    // is now SYS_WALK_CREATE on a /srv directory (create=post; the create
    // perm's DMSRVBYTE bit selects byte- vs 9P-mode) -> devsrv_post_listener.
    // The number stays reserved: no reuse, no compat shim.

    // P5-corvus-srv-impl-a3b: SYS_SRV_ACCEPT(service_handle) →
    // connection_handle / -1
    //   x0 = service_handle   the KObj_Srv handle from SYS_POST_SERVICE
    // Accept one kernel-minted /srv connection for the service the caller
    // posted (CORVUS-DESIGN.md §6.2). BLOCKS until a client opens the
    // service — corvus calls SYS_SRV_ACCEPT in a loop, one accept per
    // client connection. Returns a KObj_Spoor connection handle: the
    // server endpoint, on which corvus reads client→server 9P frame bytes
    // with SYS_READ and writes server→client bytes with SYS_WRITE; close
    // tears the connection down.
    //
    // Gated to the service's poster — the caller must hold the service
    // handle AND its stripes must match the registry entry's poster.
    //
    // Returns the connection handle (hidx ≥ 0) on success, -1 on:
    //   - service_handle is not a KObj_Srv service handle the caller holds
    //   - the caller is not the service's current poster
    //   - the service ceased to be LIVE while the accept blocked
    //   - kmalloc OOM for the connection Spoor / handle table full
    SYS_SRV_ACCEPT   = 27,   // arg: service_handle (x0)

    // P5-corvus-srv-impl-a3c: SYS_SRV_PEER(connection_handle, out_va) → 0/-1
    //   x0 = connection_handle  the KObj_Spoor endpoint from SYS_SRV_ACCEPT
    //   x1 = out_va             user-VA pointer to a struct srv_peer_info
    // Read a /srv connection's kernel-stamped peer identity (CORVUS-DESIGN
    // §6.3; invariant C-22). corvus calls this per request to learn who is
    // on the other end of a connection — the identity is stamped by the
    // kernel, never supplied by the client, never cached on corvus's fid
    // state.
    //
    // The kernel writes a struct srv_peer_info to *out_va:
    //   - `stripes` + `console` are the peer Proc's IMMUTABLE identity,
    //     captured by value on the connection at mint — available even
    //     after the peer exits (no Proc lookup, no use-after-free).
    //   - `caps` is the peer's MUTABLE capability set, read LIVE under the
    //     process-table lock. `alive` is 1 iff an ALIVE Proc still carries
    //     `stripes`; when the peer has exited (zombie / reaped) the
    //     dead-Proc guard fail-closes — `alive` = 0 and `caps` = 0, never
    //     a stale snapshot (specs/corvus.tla ConnOpPeerWasLive).
    //
    // Gated to the service's poster: the connection captured the poster's
    // identity at mint, and the caller's stripes must match it — only
    // corvus may query its own connections' peers.
    //
    // Returns 0 on success, -1 on:
    //   - connection_handle is not a KObj_Spoor /srv connection endpoint
    //     the caller holds (with RIGHT_READ)
    //   - the caller is not the connection's service poster
    //   - out_va is outside the user-VA bound
    SYS_SRV_PEER     = 28,   // arg: connection_handle (x0), out_va (x1)

    // P5-poll-a: SYS_POLL(fds_va, nfds, timeout_ms) → ready-fd count / -1
    //   x0 = fds_va        user-VA pointer to a `struct pollfd[nfds]` array
    //                      (8 B each; layout pinned by <thylacine/poll.h>)
    //   x1 = nfds          1..PROC_HANDLE_MAX = 64
    //   x2 = timeout_ms    s32: <0 = block indefinitely, 0 = non-blocking,
    //                           >0 = block at most this many milliseconds
    // The multi-fd wait primitive (ARCH §23.3; specs/poll.tla). Parks the
    // caller until at least one of the listed fds becomes ready, or the
    // timeout elapses. Returns the number of pollfds with revents != 0
    // (≥ 0) or -1 on error. `revents` is written back to the user array
    // per pollfd; a partial write fault scrubs already-written bytes.
    //
    // Returns -1 on:
    //   - nfds == 0 or > PROC_HANDLE_MAX
    //   - fds_va outside the user-VA bound (or fds_va == 0 with nfds > 0)
    //   - a partial-write fault on the revents writeback
    //
    // POLLNVAL is set in revents (kernel-filled) for a per-pollfd invalid
    // fd; the call as a whole still returns a non-negative count.
    SYS_POLL         = 29,   // arg: fds_va (x0), nfds (x1), timeout_ms (x2)

    // 30 — RETIRED (stalk-3c). Was SYS_SRV_CONNECT. Connecting to a /srv
    // service is now SYS_OPEN on /srv/<name> (open=connect -> devsrv_open_
    // connect: a 9P-mode service yields a dev9p root Spoor, a byte-mode
    // service a CSRVCLIENT conn Spoor). Reserved: no reuse, no compat shim.

    // P5-corvus-srv-impl-b3a: SYS_SPAWN_WITH_PERMS(name_va, name_len,
    //                                              fd_list_va, fd_count,
    //                                              cap_mask, perm_flags)
    //                                              → pid / -1
    //   x0..x4 — name + fd_list + cap_mask, identical to SYS_SPAWN_FULL.
    //   x5 = perm_flags — bitmask of SPAWN_PERM_* bits stamped on the
    //                     child Proc atomically inside the spawn thunk
    //                     (BEFORE the child's exec_setup, so the child
    //                     never observes the un-stamped intermediate
    //                     state — race-free vs. a stamp-after-spawn race
    //                     where the child might call SYS_POST_SERVICE
    //                     before the parent's mark lands).
    //
    // PROC_FLAG_MAY_POST_SERVICE is the design's "kernel-stamped" gate
    // for the /srv service registry (CORVUS-DESIGN.md §6.1; the flag is
    // NOT a cap_mask bit because rfork must not propagate it across the
    // OS-internal trust chain). SYS_SPAWN_WITH_PERMS is the production
    // path joey uses to confer the bit on /sbin/corvus at spawn time.
    //
    // Granting any SPAWN_PERM_* bit requires the caller to be
    // console-attached (the local-console trust anchor — joey is, an
    // ordinary Proc is not). The check fires per-bit: a call with
    // perm_flags=0 behaves identically to SYS_SPAWN_FULL.
    //
    // Returns -1 on:
    //   - perm_flags contains an unknown SPAWN_PERM_* bit
    //   - any SPAWN_PERM_* bit is set AND the caller is not console-
    //     attached
    //   - any condition that SYS_SPAWN_FULL itself returns -1 on
    //     (bad fds / missing binary / oversized name / OOM / etc.)
    SYS_SPAWN_WITH_PERMS = 31,  // arg: name_va, name_len, fd_list_va, fd_count,
                                //      cap_mask, perm_flags

    // P5-hostowner-b-b: SYS_CAP_GRANT(cap_mask, target_stripes) and
    // SYS_CAP_USE(cap_mask) — userspace bridges to the kernel `cap` device
    // (CORVUS-DESIGN.md §5.5.1). A namespace-aware open exists since
    // stalk (SYS_OPEN resolves paths), but the syscall bridges REMAIN the
    // production path because the two writers are CHROOTED — corvus to its
    // storage capability, the redeemer to its session tree — and a /cap
    // file walk is not reachable from those roots (the argued file-first
    // deviation; CLAUDE.md A-4 row). The Dev write op (devcap_write on
    // /cap/grant or /cap/use) stays the conceptual path for un-chrooted
    // writers + tests. Same gate semantics as the Dev op
    // (CAP_GRANT_HOSTOWNER for grant; PROC_FLAG_CONSOLE_-
    // ATTACHED + matching stripes + matching cap_mask for use).
    //
    // SYS_CAP_GRANT returns 0 on success (a synthetic "wrote frame" ack;
    // we don't echo the byte count the file-write op returns since this
    // is a syscall, not a fd write). -1 on any gate fail / bad args /
    // table full.
    //
    // SYS_CAP_USE returns 0 on success; -1 on gate fail / no pending
    // grant / mismatched cap / writer stripes == 0.
    SYS_CAP_GRANT    = 32,   // arg: cap_mask (x0), target_stripes (x1)
    SYS_CAP_USE      = 33,   // arg: cap_mask (x0)

    // P5-stratumd-stub-bringup-e1: walk-and-open a single path component
    // through a Spoor's Dev vtable; the v1.0 minimal walk-through-mount
    // primitive. Composes spoor_clone + dev->walk + dev->open + handle_
    // alloc to give userspace a way to reach a file under an attached /
    // mounted root without going through the (still-absent) Plan-9-style
    // open(name, mode) namec walker. Single-component-only at v1.0 — no
    // '/' splitting, no '.' / '..', no leading or trailing slashes; the
    // multi-component walker lands with the production open() syscall.
    //
    // SYS_WALK_OPEN(spoor_fd, name_va, name_len, omode) → opened_fd / -1
    //   x0 = spoor_fd   (hidx_t; must be KOBJ_SPOOR with RIGHT_READ — OR
    //                    the SYS_WALK_OPEN_FROM_ROOT sentinel == (u64)-1,
    //                    in which case the kernel uses the caller's
    //                    territory's root_spoor as the walk source.
    //                    P5-stratumd-stub-bringup-e2.)
    //   x1 = name_va    (user-VA pointer to the component name)
    //   x2 = name_len   (bytes; > 0, ≤ SYS_WALK_OPEN_NAME_MAX)
    //   x3 = omode      (u32; Plan 9 OREAD=0 / OWRITE=1 / ORDWR=2 / OEXEC=3
    //                    in the low 2 bits, optionally OTRUNC=0x10. Bits
    //                    outside SYS_WALK_OPEN_OMODE_VALID are rejected.)
    // Returns: x0 = opened KOBJ_SPOOR fd (>=0) on success, or -1 on:
    //   - spoor_fd not KOBJ_SPOOR / out-of-range / missing RIGHT_READ
    //     (excluding the FROM_ROOT sentinel)
    //   - FROM_ROOT sentinel used but the caller has no pivoted root_spoor
    //   - the backing Dev has no walk or no open vtable op
    //   - name_va outside user-VA / name_len == 0 / > 64
    //   - name contains '/' or '\0'
    //   - omode has bits outside the SYS_WALK_OPEN_OMODE_VALID mask
    //   - dev->walk fails (file not found / 9P Rlerror / OOM)
    //   - dev->open fails (Rlerror / permission)
    //   - handle table full
    //
    // The returned handle's rights are DERIVED FROM omode (A-3b F1,
    // rights_for_omode: OREAD→RIGHT_READ, OWRITE→RIGHT_WRITE, ORDWR→R|W,
    // OEXEC→RIGHT_READ, +OTRUNC→+RIGHT_WRITE; a normally-opened handle
    // adds RIGHT_TRANSFER; T_OPATH keeps the born-R|W navigation base
    // with NO TRANSFER) — the capability axis cannot exceed the access
    // the perm_check validated. The server additionally enforces the
    // fid's omode (defense in depth, no longer the only gate).
    //
    // Lifecycle: dev9p_walk allocates a fresh fid + populates the new
    // Spoor's priv with fid_owned=true. dev9p_close on the new Spoor
    // clunks that fid. A failed walk OR open spoor_clunks the new Spoor
    // (which clunks the fid if walk had succeeded, or no-ops if it
    // hadn't). The source Spoor's fid is untouched. For the FROM_ROOT
    // sentinel path the source is the Territory's root_spoor (held by
    // the Territory; not freed by the syscall).
    SYS_WALK_OPEN    = 34,   // arg: spoor_fd, name_va, name_len, omode

    // P5-stratumd-stub-bringup-e2: stamp the calling Proc's Territory
    // root_spoor to the given Spoor. The pivoted root is the Spoor at
    // which name resolution starts for the SYS_WALK_OPEN_FROM_ROOT
    // sentinel (spoor_fd == -1). Maps to specs/territory.tla::Chroot.
    //
    // SYS_CHROOT(spoor_fd) → 0 / -1
    //   x0 = spoor_fd   (hidx_t; must be KOBJ_SPOOR with RIGHT_READ)
    // Lifecycle: the Territory takes its own refcount on the source Spoor
    // (via territory_chroot → spoor_ref). The caller MAY close their
    // spoor_fd afterward; the Territory keeps the Spoor alive until a
    // subsequent SYS_CHROOT replaces it (spoor_clunk on the displaced
    // root) OR until Territory destruction (proc_free → territory_unref
    // → spoor_clunk on root_spoor). Idempotent: SYS_CHROOT to the same
    // Spoor returns 0 without bumping refcount.
    //
    // Returns 0 on success, -1 on:
    //   - spoor_fd not KOBJ_SPOOR / out-of-range / missing RIGHT_READ
    //   - the caller has no Territory (kernel invariant; structurally
    //     impossible for a userspace Proc, defense-in-depth)
    //
    // Audit-bearing: touches `kernel/territory.c` (CLAUDE.md §25.4 trigger
    // surface — cycle-freedom, isolation, mount-refcount consistency).
    SYS_CHROOT       = 35,   // arg: spoor_fd

    // P6-pouch-kernel-auxv: SYS_SET_TID_ADDRESS(tidptr) → tid
    //   x0 = tidptr   user-VA of the calling thread's tid word (the
    //                 "clear-child-tid" address, per the Linux
    //                 set_tid_address(2) contract).
    // A C runtime (pouch — the Thylacine POSIX libc) calls this once at
    // thread startup (musl's __init_tp) to learn the thread id. Returns
    // the calling thread's tid. At v1.0 — single-threaded Procs — the
    // main thread's tid is the Proc's pid, mirroring the Linux
    // convention that the thread-group leader's tid equals the pid.
    //
    // `tidptr` is ACCEPTED but neither stored nor acted on at v1.0: its
    // only effect — clearing *tidptr + a futex wake on thread exit — is
    // observable only with multiple threads, which land with the
    // pouch-threads sub-chunk (POUCH-DESIGN.md §12.4); it is wired there
    // alongside the per-thread tid. Never fails for a userspace caller.
    SYS_SET_TID_ADDRESS = 36,  // arg: tidptr (x0)

    // P6-pouch-mem: SYS_BURROW_ATTACH(length) → vaddr
    //   x0 = length   bytes; 1..BURROW_ATTACH_MAX. Rounded up to a
    //                 PAGE_SIZE multiple.
    // Attach an anonymous Burrow of `length` bytes into the calling
    // Proc's address space and return its base user-VA. The kernel
    // chooses the address — a first-fit scan (vma_find_gap) of the
    // burrow-attach window (EXEC_USER_BURROW_BASE..TOP, well above the
    // user stack). The region is RW, demand-zero. The v1.0 native
    // memory-growth primitive (ARCHITECTURE.md §6.5 Tier 1) — the
    // substrate for libt's and pouch's malloc.
    //
    // Wraps the audited burrow_create_anon → burrow_map(RW) →
    // burrow_unref discipline: the installed VMA's mapping_count keeps
    // the Burrow alive, handle_count is 0 (no handle — Tier 1). Pages
    // are eagerly allocated by burrow_create_anon (power-of-2 page
    // rounding); PTEs install on demand via the user-fault path.
    //
    // Returns the page-aligned base VA (≥ EXEC_USER_BURROW_BASE, so
    // never negative) on success, -1 on:
    //   - length == 0 or length > BURROW_ATTACH_MAX
    //   - no free range of the rounded length in the burrow window
    //   - burrow_create_anon / burrow_map OOM
    SYS_BURROW_ATTACH = 37,  // arg: length (x0)

    // P6-pouch-mem: SYS_BURROW_DETACH(vaddr, length) → 0 / -1
    //   x0 = vaddr    the base VA a prior SYS_BURROW_ATTACH returned
    //   x1 = length   the attached length — the caller's original
    //                 request OR any value that page-rounds to the
    //                 same span (the match is on the page-rounded
    //                 [vaddr, vaddr + round_up(length)) range)
    // Detach a region previously attached by SYS_BURROW_ATTACH. The
    // (vaddr, rounded length) must match an installed VMA exactly — no
    // partial detach at v1.0 (mirrors burrow_unmap's constraint). The
    // VMA is removed and, mapping_count reaching 0 with handle_count
    // already 0, the Burrow's pages are freed.
    //
    // Returns 0 on success, -1 on:
    //   - length == 0 or length > BURROW_ATTACH_MAX
    //   - vaddr not page-aligned
    //   - no VMA matches [vaddr, vaddr + round_up(length)) exactly
    SYS_BURROW_DETACH = 38,  // arg: vaddr (x0), length (x1)

    // P6-pouch-wait-addr (sub-chunk 8): the `torpor` wait-on-address
    // primitive (POUCH-DESIGN.md §10) — Thylacine's futex-equivalent.
    // The substrate over which pouch's pthread mutex/condvar
    // implementation runs (sub-chunk 9 `pouch-threads`).
    //
    // SYS_TORPOR_WAIT(addr_va, expected, timeout_us) → 0 / -errno
    //   x0 = addr_va      user-VA pointer to a 4-byte aligned u32 word
    //   x1 = expected     compare against *addr_va under the kernel
    //                     `torpor_lock`; if they don't match, return 0
    //                     immediately (fast path — userspace re-evaluates)
    //   x2 = timeout_us   s64 — < 0 = block indefinitely; >= 0 = block
    //                     at most this many microseconds (TORPOR_MAX_TIMEOUT_US
    //                     = 1 hour caps the explicit values; 0 is a probe
    //                     that returns -ETIMEDOUT immediately if the value
    //                     still matched)
    //
    // Returns 0 on success (woken OR value mismatch); explicit -errno on
    // failure (Linux/musl-numeric, decoded by pouch's syscall_ret.c):
    //   -EINVAL   (-22)   bad addr_va alignment / outside user VA / null
    //                     Proc / timeout_us > TORPOR_MAX_TIMEOUT_US
    //   -EFAULT   (-14)   addr_va in range but unmapped or
    //                     permission-denied at load
    //   -ETIMEDOUT (-110) timeout lapsed with no wake
    //
    // No-lost-wakeup proof (CLAUDE.md "Spec-to-code suspended" —
    // validated by reasoning, not specs/futex.tla): the consumer
    // atomically (under torpor_lock) loads *addr_va and registers
    // before sleeping; the producer's WAKE takes the same torpor_lock
    // to walk. Lock-acquire/release pairing handles all interleavings —
    // either WAKE-unlock precedes WAIT-lock (consumer sees the
    // producer's store, returns 0 without sleep), or WAIT-lock precedes
    // WAKE-lock (consumer is registered, WAKE delivers the wakeup).
    // Spurious wakes are possible; userspace re-checks the atomic word
    // (standard futex discipline).
    SYS_TORPOR_WAIT  = 39,   // arg: addr_va, expected, timeout_us

    // SYS_TORPOR_WAKE(addr_va, count) → woken / -errno
    //   x0 = addr_va      user-VA pointer; same alignment + bound rules
    //                     as SYS_TORPOR_WAIT. WAKE does NOT load the
    //                     word — it only hashes the address and walks
    //                     the wait queue.
    //   x1 = count        max waiters to wake. 0 is a no-op (returns 0);
    //                     UINT32_MAX is the "wake all" pattern
    //                     (pthread_cond_broadcast).
    // Returns the number of waiters actually woken (`>= 0`), or
    // -EINVAL on bad args. Cross-Proc shared-futex semantics
    // deferred to v1.x with Tier-2 burrows (POUCH-DESIGN.md §10);
    // v1.0 hashing is `(Proc *, addr_va)`-keyed.
    SYS_TORPOR_WAKE  = 40,   // arg: addr_va, count

    // P6-pouch-threads (sub-chunk 9): the kernel-side thread-spawn /
    // thread-exit pair. The substrate over which pouch's pthread_create
    // / pthread_exit / pthread_join run. POUCH-DESIGN.md §7 [RESOLVED 7.3].
    //
    // SYS_THREAD_SPAWN(entry_va, sp_va, arg, tls_va, ptid_va) → tid / -errno
    //   x0 = entry_va   user-VA of the entry function (EL0 PC)
    //   x1 = sp_va      user-VA of the new thread's user stack TOP (the
    //                   address that will be installed into SP_EL0; must
    //                   be 16-byte-aligned per AAPCS64)
    //   x2 = arg        user value passed as x0 (AAPCS64 arg-0) to the
    //                   entry function. Opaque to the kernel.
    //   x3 = tls_va     user-VA written to TPIDR_EL0 before eret; 0 is
    //                   permitted (no TLS yet — musl's __pthread_self is
    //                   then a faulting deref until the entry function
    //                   does its own TLS setup).
    //   x4 = ptid_va    CLONE_PARENT_SETTID (#112): user-VA of a 4-byte
    //                   word the kernel publishes the new tid into BEFORE
    //                   the child is made runnable. 0 opts out (no
    //                   publish). Non-zero must be 4-byte aligned + within
    //                   user VA (same gate as SYS_SET_TID_ADDRESS's
    //                   tidptr); a bad ptid is -EINVAL. Because parent and
    //                   child share the address space, this one write
    //                   serves both — pouch passes &new->tid so neither
    //                   the parent nor the child ever observes new->tid==0
    //                   (retires the #111 tid==0 window at its root).
    //
    // Allocates a new Thread + 16 KiB kstack in the CALLING Proc (same
    // pgtable_root + ASID — the new thread shares the caller's address
    // space). The new Thread is registered on the run-tree as RUNNABLE;
    // the first dispatch lands at thread_user_trampoline which erets to
    // EL0 at entry_va running on sp_va with TPIDR_EL0 = tls_va + x0 = arg.
    //
    // Returns the new thread's tid (positive int) on success. Returns
    // negative -errno on failure (Linux/musl-numeric; pouch's
    // syscall_ret.c decodes [-4095, -2]):
    //   -EINVAL  bad alignment / out-of-bound entry_va / out-of-bound
    //            sp_va / out-of-bound tls_va / bad ptid_va / caller is kproc
    //   -EAGAIN  per-Proc thread cap (PROC_THREAD_MAX) reached (#65, I-32)
    //   -ENOMEM  kstack alloc fail / Thread cache alloc fail
    // The ptid publish is best-effort: a fault writing *ptid_va (a buggy
    // caller; the address passed align+bound but is unmapped) is tolerated
    // -- the spawn still succeeds and the tid is returned in x0 -- mirroring
    // the exit-time clear_child_tid store's discipline.
    //
    // The new Thread inherits no user state from the caller — the entry
    // function is the responsible adult. fd inheritance is implicit
    // (single handle table per Proc); register state is the four parked
    // args + zeros.
    SYS_THREAD_SPAWN = 41,   // arg: entry_va, sp_va, arg, tls_va, ptid_va

    // SYS_THREAD_EXIT — terminate the calling Thread. NEVER returns.
    //   (no args)
    //
    // Atomically:
    //   1. If clear_child_tid != 0 on this Thread (set via
    //      SYS_SET_TID_ADDRESS): uaccess_store_u32(0) at that user-VA +
    //      torpor_wake(UINT32_MAX) on the same address. Best-effort —
    //      a failed store (page unmapped) skips the wake but does not
    //      extinct.
    //   2. Mark self THREAD_EXITING under g_proc_table_lock.
    //   3. If this is the LAST non-EXITING thread in the Proc, also
    //      transition the Proc to ZOMBIE with exit_status = 0 + wake
    //      parent's child_waiters (mirrors exits() with status 0).
    //   4. yield via sched(); never returns.
    //
    // After-exit reaping: the Thread descriptor + kstack remain
    // allocated until the Proc dies (then wait_pid's reap path frees
    // every Thread in p->threads). v1.0 accepts this — short-lived
    // programs (libsodium tests) bound the leak; long-running daemons
    // (stratumd) join their threads at shutdown so the Proc dies
    // shortly after. Per-Thread reaping is a v1.x extension.
    //
    // Never returns to userspace — userspace must treat any return as
    // a kernel bug. The x0 the SVC dispatch writes on a fall-through
    // path is undefined; v1.0 reaches sched() unconditionally.
    SYS_THREAD_EXIT  = 42,   // no args

    // 43 — RETIRED (stalk-3c). Was SYS_POST_SERVICE_BYTE. Byte-mode posting
    // is now SYS_WALK_CREATE on a /srv dir with the DMSRVBYTE perm bit
    // (create=post). Reserved: no reuse, no compat shim.

    // P6-pouch-signals-impl (sub-chunk 13a): the note delivery primitive.
    // Design in ARCH §7.6.1-§7.6.8 (binding scripture at 237f096); the
    // novel angle (fd-first, async-handler as opt-in) is NOVEL.md §3.1.

    // SYS_NOTE_OPEN — mint a fd to the calling Proc's note Spoor read end.
    //   (no args)
    // Idempotent: each call mints a fresh handle against the same kernel-
    // owned note Spoor (`devnotes`). Closing one fd doesn't affect the
    // queue or future opens; the queue lives with the Proc (N-5).
    //
    // The returned fd reads `struct note_record` (32 bytes — name + arg +
    // sender_pid + timestamp_ns; ABI-pinned in <thylacine/notes.h>) one
    // record per read() call at v1.0. poll() integrates: POLLIN iff the
    // queue has at least one entry whose NOTE_BIT_* is NOT in the calling
    // Thread's note_mask.
    //
    // Returns the new fd (>= 0) on success, -1 on:
    //   - handle table full
    //   - devnotes_open failure (defense-in-depth; structurally impossible)
    SYS_NOTE_OPEN    = 44,

    // SYS_NOTIFY(handler_va) — register / clear the async note handler.
    //   x0 = handler_va   user-VA of the handler function (0 to clear).
    // The handler is per-Proc (inherited across rfork(RFPROC) — the v1.0
    // rfork path is single-Proc-spawn; the inheritance lands when
    // pthread_create-equivalent rfork-share semantics arrive). When set,
    // the EL0-return-tail dispatch in arch/arm64/exception.c pops the
    // next deliverable note from the queue and lands the handler on it.
    //
    // Returns 0 on success, -1 on:
    //   - handler_va is non-zero AND outside the user-VA bound
    SYS_NOTIFY       = 45,

    // SYS_NOTED(arg) — return from a running note handler.
    //   x0 = arg    NCONT (= 0; restore saved user context + resume)
    //               NDFLT (= 1; take the note's default action — for the
    //                            v1.0 supported set, exits with a status
    //                            string matching the note name)
    // NEVER RETURNS NORMALLY. Either the saved user context is restored
    // (the syscall's exception_context is rewritten with the t->note_saved_*
    // fields), or the Proc transitions to ZOMBIE via exits.
    //
    // Returns -1 on:
    //   - caller is NOT in a handler (t->in_handler == false)
    //   - arg is not NCONT or NDFLT
    SYS_NOTED        = 46,

    // SYS_POSTNOTE(pid, name_va, name_len) — post a note to another Proc.
    //   x0 = pid        target Proc's pid, OR the self-post sentinel
    //                    pid == 0 (P6-pouch-signals sub-chunk 13b — used
    //                    by pouch's raise() since pouch has no userspace
    //                    getpid path at v1.0; matches POSIX kill(0, sig)
    //                    semantics, which says "send to every process in
    //                    the calling process's group" — Thylacine has
    //                    no process groups, so the closest equivalent is
    //                    "self"; the sentinel is the canonical name for
    //                    that mapping)
    //   x1 = name_va    user-VA pointer to the note name bytes
    //   x2 = name_len   bytes (1..NOTE_NAME_MAX-1; NUL not required —
    //                    name_len is authoritative)
    //
    // Permission gate at v1.0: caller must be the target's parent OR
    // pid == caller's own pid OR pid == 0 (self-post is always allowed).
    // Future: CAP_KILL (ARCH §7.6.8 [OPEN Q 7.6.B]) plus the long-term
    // namespace-shape (write to /proc/<pid>/note when path resolution
    // arrives).
    //
    // Returns 0 on success, -1 on:
    //   - bad name_len bounds (0 or > NOTE_NAME_MAX - 1)
    //   - name_va outside the user-VA bound / unmapped
    //   - name not in the v1.0 supported set (NOTE_NAME_* enumerated in
    //     <thylacine/notes.h>)
    //   - target pid not found (-ESRCH-equivalent collapsed to -1)
    //   - target Proc's queue is full (-EAGAIN-equivalent)
    //   - permission denied (caller is not the parent + pid != self)
    SYS_POSTNOTE     = 47,

    // SYS_NOTE_MASK(new_mask, old_mask_out_va) — set the calling Thread's
    //   x0 = new_mask         the new mask (bit set = defer that note)
    //   x1 = old_mask_out_va  user-VA pointer to a u64; the previous mask
    //                          is written there if old_mask_out_va != 0
    // The mask is per-Thread (POSIX pthread_sigmask semantics).
    // Unsupported bits are tolerated (set but unused) so future supported-
    // note additions don't break old userspace that wrote a wider mask.
    //
    // Returns 0 on success, -1 on:
    //   - old_mask_out_va is non-zero AND outside the user-VA bound /
    //     unmapped at store time
    SYS_NOTE_MASK    = 48,

    // SYS_SPAWN_FULL_ARGV(req_va) — combined spawn primitive that subsumes
    // SYS_SPAWN / SYS_SPAWN_WITH_FDS / SYS_SPAWN_WITH_CAPS / SYS_SPAWN_FULL
    // / SYS_SPAWN_WITH_PERMS and adds argv pass-through (P6-pouch-stratumd-
    // boot sub-chunk 16b-alpha). v1.0 spawn surfaces inherit argv =
    // [name] (one entry); stratumd in sub-chunk 16b-beta needs real argv
    // (binary path + --keyfile + --listen + ...). Rather than adding yet
    // another permutation to the SYS_SPAWN_* family, this entry takes a
    // single user pointer to a struct sys_spawn_args carrying every
    // existing spawn feature plus the new argv buffer.
    //
    //   x0 = req_va         user-VA pointer to a struct sys_spawn_args
    //                       (must be fully mapped + readable; alignment
    //                        is not required — the kernel reads byte-by-
    //                        byte to be alignment-tolerant)
    //
    // The struct's wire fields (see struct sys_spawn_args below) carry
    // name + argv + fds + cap_mask + perm_flags. Validation rules + error
    // semantics mirror the SYS_SPAWN_WITH_PERMS handler one-to-one for the
    // shared fields; the new argv fields are validated against the
    // SYS_SPAWN_ARGV_* bounds + the NUL-termination invariant.
    //
    // Lifetime: the kernel copies the argv_data buffer into kernel memory
    // BEFORE rfork. The buffer is owned by the spawn_args struct until the
    // child thunk consumes it via exec_setup_with_argv; the user-side
    // buffer is never observed post-syscall. argv buffer never crosses
    // Proc boundaries by handle transfer (strings only — I-4 + I-5
    // structurally upheld; argv contains no handles).
    //
    // The child observes argv via the System V startup frame at sp
    // (exec_build_init_stack lays out argc / argv[] / envp[] / auxv +
    // strings region; pouch's musl _start exposes argv to main).
    //
    // Returns the child's pid on success (positive), -1 on any failure:
    //   - req_va outside the user-VA bound / unmapped at struct-copy time
    //   - any field outside its documented bounds (name_len, argv_data_len,
    //     argc, fd_count, perm_flags, cap_mask — same as the legacy
    //     SYS_SPAWN_WITH_PERMS rejections)
    //   - _pad_envp is nonzero (reserved for forward-compat envp_data_len
    //     when envp pass-through lands; rejected loudly on v1.0 so a
    //     future kernel that wires the field cannot land on a v1.0
    //     caller that left random bytes in the slot)
    //   - argv_data does not end in NUL OR NUL count != argc
    //   - argc > 0 but argv_data_len == 0 (or vice versa — the two
    //     fields are tied; argc == 0 requires argv_data_len == 0 and
    //     vice versa)
    //   - perm_flags carries any bit outside SPAWN_PERM_ALL
    //   - perm_flags nonzero but caller is not console-attached
    //   - any inherited fd is not a KOBJ_SPOOR handle
    //   - binary not resolvable in the namespace, OR exceeds EXEC_FILE_MAX
    //     (REVENANT R-4 retired the 1-MiB SYS_SPAWN_BLOB_MAX slurp cap)
    //   - kmalloc OOM at any step
    //   - rfork_with_caps OOM (Proc / Thread allocation)
    SYS_SPAWN_FULL_ARGV = 49,

    // SYS_FSTAT(spoor_fd, stat_va) → 0 / -1 (P6-pouch-stratumd-boot sub-chunk
    // 16b-gamma; A-2a grew it to 80). Populate the user-VA `struct t_stat` with
    // metadata for the file opened on `spoor_fd`. The Thylacine-native stat
    // surface — Plan 9 9P qid identity carried verbatim alongside POSIX-shaped
    // mode/size/timestamps so pouch's `fstat()` can translate to musl's
    // `struct stat` without losing 9P provenance.
    //
    //   x0 = spoor_fd   (hidx_t; must be KOBJ_SPOOR with RIGHT_READ)
    //   x1 = stat_va    user-VA pointer to an 80-byte struct t_stat (must be
    //                   fully mapped + writable; alignment NOT required — the
    //                   kernel stores byte-by-byte for alignment tolerance)
    //
    // The kernel dispatches to dev->stat_native if the Dev implements it.
    // devramfs implements it (returning size from the cpio metadata and
    // mode/qid from the in-kernel file table). Other Devs without a real
    // stat_native (devcons / devnull / devzero / devpipe / devnotes / devsrv /
    // SrvConn) leave the slot NULL — SYS_FSTAT on those fds returns -1, the
    // graceful "no stat for this object" answer.
    //
    // Rights: RIGHT_READ on the handle. fstat does not "read" file contents
    // but it does observe metadata, which is the read-side of access (POSIX
    // fstat requires the fd be valid, not specifically readable; we tighten
    // to RIGHT_READ because every v1.0 caller that fstats a fd also reads it,
    // and the tightening cheaply blocks a fstat-as-side-channel on a
    // write-only handle).
    //
    // Returns 0 on success (out_stat populated), -1 on:
    //   - spoor_fd not KOBJ_SPOOR / out-of-range / missing RIGHT_READ
    //   - stat_va outside the user-VA bound / unmapped at store time
    //   - the Dev does not implement stat_native (NULL slot)
    //   - dev->stat_native returned an error
    //
    // Audit-bearing: touches `kernel/syscall.c` (rights gate + uaccess store)
    // + `kernel/devramfs.c` (the stat_native implementation). CLAUDE.md
    // §"stratumd boot mount close" audit-trigger row.
    SYS_FSTAT        = 50,   // arg: spoor_fd (x0), stat_va (x1)

    // SYS_LSEEK(spoor_fd, offset, whence) → new_offset / -1 (P6-pouch-stratumd-
    // boot sub-chunk 16b-gamma). Position the kernel-side read/write cursor
    // for `spoor_fd`. Maps to POSIX `lseek(2)`. The cursor is the `s64 offset`
    // field of the Spoor that SYS_READ / SYS_WRITE advance per call.
    //
    //   x0 = spoor_fd   (hidx_t; must be KOBJ_SPOOR; rights not required —
    //                    the cursor is metadata local to the open file, not
    //                    a read or write of content)
    //   x1 = offset     signed 64-bit offset; interpretation depends on
    //                   `whence` (treated as s64 throughout)
    //   x2 = whence     T_SEEK_SET (0) / T_SEEK_CUR (1) / T_SEEK_END (2)
    //
    // Semantics:
    //   - T_SEEK_SET: new_offset = offset                  (offset >= 0)
    //   - T_SEEK_CUR: new_offset = c->offset + offset      (overflow check)
    //   - T_SEEK_END: new_offset = file_size + offset      (overflow check;
    //                 file_size queried via dev->stat_native; Devs without
    //                 stat_native return -1)
    //
    // The kernel rejects new_offset < 0 (POSIX EINVAL). The cursor is set
    // ATOMICALLY relative to a concurrent reader/writer on the same Spoor
    // — concurrent SYS_READ / SYS_WRITE on the same fd from different
    // threads is unspecified at v1.0 (no per-Spoor cursor lock yet); the
    // POSIX user should serialize themselves.
    //
    // Returns the new offset (>= 0) on success, -1 on:
    //   - spoor_fd not KOBJ_SPOOR / out-of-range
    //   - whence not in {T_SEEK_SET, T_SEEK_CUR, T_SEEK_END}
    //   - new_offset would be < 0 (T_SEEK_SET with offset < 0; T_SEEK_CUR
    //     underflow; T_SEEK_END underflow)
    //   - T_SEEK_END but Dev does not implement stat_native
    //
    // Audit-bearing: per the SYS_FSTAT row in CLAUDE.md.
    SYS_LSEEK        = 51,   // arg: spoor_fd (x0), offset (x1), whence (x2)

    // P6-pouch-stratumd-boot 16c: SYS_ATTACH_9P_SRV(srv_fd, aname_va,
    //                                                 aname_len, n_uname)
    //                                                 -> spoor_fd / -1
    //   x0 = srv_fd      (hidx_t; must be a byte-mode CSRVCLIENT conn Spoor
    //                    (KOBJ_SPOOR) the caller holds, with RIGHT_READ +
    //                    RIGHT_WRITE -- the kernel 9P client writes
    //                    Twalk/Tread/Twrite and reads Rwalk/Rread/Rwrite
    //                    over the conn's c2s/s2c rings)
    //   x1 = aname_va    (user-VA pointer to the attach name string;
    //                    NUL not required -- aname_len authoritative)
    //   x2 = aname_len   (bytes; <= SYS_ATTACH_ANAME_MAX = 256;
    //                    zero-length aname is permitted)
    //   x3 = n_uname     (u32; 0 for no-auth attach at v1.0)
    //
    // Parallel to SYS_ATTACH_9P but the transport is a byte-mode SrvConn
    // reached by open=connect (SYS_OPEN on a byte-mode /srv service ->
    // devsrv_open_connect -> a CSRVCLIENT conn Spoor) rather than a Spoor
    // pair. Pre-stalk-3b-β the endpoint was a KObj_Srv handle from
    // SYS_SRV_CONNECT; C1 retargeted it to the KOBJ_SPOOR conn Spoor. The
    // composition (srvconn_attach_dev9p_root):
    //   1. handle_get(srv_fd) -> KOBJ_SPOOR slot; CSRVCLIENT flag check;
    //      devsrv_conn_of -> the SrvConn
    //   2. atomic-ACQUIRE on cn->byte_mode; reject if 9P-mode (a 9P-mode
    //      service is connected via open=connect directly, not attached)
    //   3. kmalloc + p9_srvconn_transport_init (takes 1 srvconn_ref) +
    //      srvconn_set_kernel_attached (the rings become load-bearing for
    //      the kernel 9P client; userspace I/O on the conn Spoor is then
    //      refused by devsrv_read/write's kernel_attached guard)
    //   4. p9_attached_create -> drives Tversion + Tattach on the
    //      kernel-owned p9_client wrapping the adapter (the per-SrvConn
    //      embedded 9P client was retired in stalk-3b-β-D; this SHARED
    //      kernel client is the only one over the rings)
    //   5. p9_attached_root_spoor -> dev9p Spoor pointing at the bound
    //      root_fid
    //   6. handle_alloc KOBJ_SPOOR with RIGHT_READ | WRITE | TRANSFER
    //      (same rights envelope as SYS_ATTACH_9P)
    //
    // The returned KOBJ_SPOOR fd can be passed to SYS_MOUNT to graft
    // the resulting 9P tree at a target path in the caller's territory.
    //
    // Lifetime: the caller's conn-Spoor handle reference is INDEPENDENT
    // of the adapter's reference. Userspace closing the source srv_fd
    // does NOT tear down the SrvConn while the returned KOBJ_SPOOR is
    // still alive (the conn Spoor's close honors kernel_attached: skip
    // teardown, unref only; the p9_attached holds the adapter holds its
    // own srvconn_ref). The SrvConn lives until ALL holders unref --
    // matches the SYS_ATTACH_9P discipline for transport-Spoor refs
    // (p9_attached_install_transport).
    //
    // Returns: x0 = new fd (>=0) on success; -1 on:
    //   - invalid srv_fd (not KOBJ_SPOOR / not CSRVCLIENT / not a devsrv
    //     conn Spoor / out-of-range / corrupted)
    //   - srv_fd missing RIGHT_READ or RIGHT_WRITE
    //   - srv_fd is 9P-mode (byte-mode gate)
    //   - aname_va outside user-VA bound OR aname_len > SYS_ATTACH_ANAME_MAX
    //   - n_uname > U32_MAX
    //   - kmalloc OOM for adapter / p9_attached_create handshake failure
    //   - handle table full
    //
    // Audit-bearing: CLAUDE.md §"Audit-triggering changes" SYS_ATTACH_
    // 9P_SRV row.
    SYS_ATTACH_9P_SRV = 52,  // arg: srv_fd, aname_va, aname_len, n_uname

    // P6-pouch-stratumd-boot 16c: SYS_PIVOT_ROOT(new_root_fd) -> 0 / -1
    //   x0 = new_root_fd   (hidx_t; must be KOBJ_SPOOR with RIGHT_READ)
    //
    // Atomic root_spoor swap for the caller's Territory. Unlike
    // SYS_CHROOT (which is documented for initial-chroot use and
    // typically called by kproc at boot to stamp the devramfs root),
    // SYS_PIVOT_ROOT is the long-running-Proc primitive that exchanges
    // an existing root for a new one. Joey calls this LAST in its
    // bringup, swapping its devramfs root for stratumd's mounted FS
    // root (the dev9p Spoor from SYS_ATTACH_9P_SRV + SYS_MOUNT).
    //
    // Semantics (mirrors Linux pivot_root(2) minus the put_old arg --
    // the displaced root is simply unreferenced via spoor_clunk, which
    // tears down the underlying tree IF this was the last holder):
    //   1. handle_get(new_root_fd) -> KOBJ_SPOOR slot
    //   2. validate rights (RIGHT_READ on the new root)
    //   3. territory_pivot_root(territory, new_root_spoor):
    //        a. spoor_ref(new) -- bump BEFORE swap (extincts on
    //           corrupted source under spoor_ref's invariant; pre-
    //           swap any failure leaves the territory unchanged)
    //        b. atomically replace root_spoor with new
    //        c. spoor_clunk(old) -- if territory was last holder,
    //           Dev close hook runs (frees underlying state)
    //
    // Idempotent on same-spoor (returns 0 without bumping refcount).
    //
    // Why distinct from SYS_CHROOT (not a re-chroot): semantic clarity
    // for the audit trail + future v1.x extensibility (e.g., preserving
    // specific bind mounts across the pivot) without re-litigating
    // SYS_CHROOT's contract. Closes the v1.x note in usr/joey/joey.c
    // around line 293-304 ("v1.x adds SYS_UNCHROOT or a proper
    // pivot_root").
    //
    // Returns: 0 on success, -1 on:
    //   - new_root_fd not KOBJ_SPOOR / out-of-range / missing RIGHT_READ
    //   - caller has no Territory (kernel invariant -- structurally
    //     impossible for userspace; defense-in-depth)
    //
    // Audit-bearing: CLAUDE.md §"Audit-triggering changes" SYS_PIVOT_
    // ROOT + territory_pivot_root row.
    SYS_PIVOT_ROOT   = 53,   // arg: new_root_fd (x0)

    // Convergence-detour FS-mutation foundation (IDENTITY-DESIGN.md §9.2).
    //
    // SYS_WALK_CREATE(parent_fd, name_va, name_len, omode, perm) -> opened_fd / -1
    //   The create-then-open sibling of SYS_WALK_OPEN. Creates the single
    //   component `name` inside the directory `parent_fd` and returns a NEW
    //   opened KOBJ_SPOOR fd referring to the created object (file OR dir).
    //   x0 = parent_fd  (hidx_t; KOBJ_SPOOR with RIGHT_WRITE -- create mutates
    //                    the directory -- OR the SYS_WALK_OPEN_FROM_ROOT
    //                    sentinel (u64)-1 for the caller's Territory root.)
    //   x1 = name_va    (user-VA of the single component name.)
    //   x2 = name_len   (bytes; > 0, <= SYS_WALK_OPEN_NAME_MAX; reject '/' '\0'
    //                    + the "." / ".." entries, same shape as SYS_WALK_OPEN.)
    //   x3 = omode      (Plan 9 open mode for the returned fd; OREAD/OWRITE/
    //                    ORDWR + optional OTRUNC; SYS_WALK_OPEN_OMODE_VALID.
    //                    For a DMDIR create the new dir is opened OREAD
    //                    regardless -- you readdir a directory, never write it.)
    //   x4 = perm       (u32 Plan 9 perm. Low 9 bits = rwxrwxrwx mode. The
    //                    DMDIR bit (0x80000000) selects directory creation
    //                    (Tmkdir) instead of a file (Tlcreate). Bits outside
    //                    SYS_WALK_CREATE_PERM_VALID are rejected.)
    //
    // The created object's group is stamped from the CALLER's primary_gid
    // (carried into the 9P Tlcreate/Tmkdir gid field). Full owner attribution
    // (= caller principal_id) and per-file rwx ENFORCEMENT are A-2 (this is the
    // create MECHANISM; I-22 holds -- nothing enforces rwx yet to bypass).
    //
    // Returns: x0 = opened KOBJ_SPOOR fd (>=0; R|W|TRANSFER, matching
    // SYS_WALK_OPEN) on success, or -1 on:
    //   - parent_fd not KOBJ_SPOOR / missing RIGHT_WRITE (excluding FROM_ROOT)
    //   - FROM_ROOT sentinel used but no pivoted root_spoor
    //   - backing Dev has no walk or no create vtable op (e.g. read-only ramfs)
    //   - name_va outside user-VA / name_len 0 / > 64 / contains '/' or '\0' /
    //     "." / ".."
    //   - omode has bits outside SYS_WALK_OPEN_OMODE_VALID
    //   - perm has bits outside SYS_WALK_CREATE_PERM_VALID
    //   - dev->create fails (Rlerror: name exists / no space / server perm)
    //   - handle table full
    //
    // Audit-bearing: CLAUDE.md FS-mutation-syscalls row.
    SYS_WALK_CREATE  = 54,   // arg: parent_fd, name_va, name_len, omode, perm

    // SYS_FSYNC(fd, datasync) -> 0 / -1 (FS-mutation foundation; §9.2).
    //   x0 = fd        (hidx_t; KOBJ_SPOOR with RIGHT_WRITE -- fsync is the
    //                   write-side durability barrier.)
    //   x1 = datasync  (u32; 0 = full fsync [data + metadata], 1 = data only.)
    // The "write-then-fsync = durable" contract on the integrity FS. Dispatches
    // dev->fsync (dev9p -> p9_client_fsync -> Stratum Tsync; in-memory-durable
    // Devs no-op success). Returns -1 on: fd not KOBJ_SPOOR / missing
    // RIGHT_WRITE; the Dev has no .fsync slot; server Rlerror.
    // Audit-bearing: CLAUDE.md FS-mutation-syscalls row.
    SYS_FSYNC        = 55,   // arg: fd (x0), datasync (x1)

    // SYS_READDIR(fd, buf_va, buf_len) -> bytes_written (>=0) / -1 (§9.2).
    //   x0 = fd        (hidx_t; KOBJ_SPOOR opened on a directory, RIGHT_READ.)
    //   x1 = buf_va    (user-VA out buffer.)
    //   x2 = buf_len   (1 .. SYS_RW_MAX (4096).)
    // Reads the next run of directory entries into buf, advancing the Spoor's
    // offset (the same offset SYS_READ / SYS_LSEEK use); 0 bytes returned ==
    // end-of-directory. The buffer is the raw 9P2000.L Treaddir dirent stream
    // (per entry: qid(13) + offset(8) + type(1) + name_len(2 LE) + name); the
    // caller parses it. Dispatches dev->readdir (dev9p -> p9_client_readdir).
    // Returns -1 on: fd not KOBJ_SPOOR / missing RIGHT_READ; buf_len 0 or >
    // SYS_RW_MAX; buf_va outside user-VA; the Dev has no .readdir slot; server
    // Rlerror. Audit-bearing: CLAUDE.md FS-mutation-syscalls row.
    SYS_READDIR      = 56,   // arg: fd (x0), buf_va (x1), buf_len (x2)

    // Convergence-detour FS-gamma (IDENTITY-DESIGN.md §9.3). rename + unlink,
    // pulled forward ahead of A-1b to give corvus's identity-DB persistence the
    // classic write-tmp + fsync + atomic rename-swap substrate (and owed for the
    // A-2 coreutils mv/rm/rmdir). The kernel 9P client already implements the
    // wire half (p9_client_renameat / unlinkat); these are the syscall wrappers
    // + two new Dev vtable slots (.rename / .unlink). Unlike SYS_WALK_CREATE,
    // renameat/unlinkat operate on the dirfid(s) BY NAME and do not transition
    // them, so the handlers run the op DIRECTLY on the looked-up dir Spoor(s)
    // with no clone-walk (mirrors SYS_FSYNC / SYS_READDIR).
    //
    // SYS_RENAME(olddir_fd, oldname_va, oldname_len, newdir_fd, newname_va, newname_len) -> 0 / -1
    //   Atomically rename/move the single component `oldname` in directory
    //   `olddir_fd` to `newname` in `newdir_fd`. POSIX rename(2) / 9P Trenameat:
    //   an existing destination is ATOMICALLY REPLACED (the property A-1b's
    //   DB-swap relies on).
    //   x0 = olddir_fd  (hidx_t; KOBJ_SPOOR directory with RIGHT_WRITE -- OR the
    //                    SYS_WALK_OPEN_FROM_ROOT sentinel (u64)-1 for the
    //                    Territory root. Both directories are mutated.)
    //   x1 = oldname_va (user-VA of the source single-component name.)
    //   x2 = oldname_len(1 .. SYS_WALK_OPEN_NAME_MAX; reject '/' '\0' "." "..".)
    //   x3 = newdir_fd  (hidx_t; KOBJ_SPOOR directory with RIGHT_WRITE -- OR
    //                    FROM_ROOT. Must be the SAME Dev as olddir_fd -- a 9P
    //                    renameat is within one server; cross-Dev -> -1.)
    //   x4 = newname_va (user-VA of the destination single-component name.)
    //   x5 = newname_len(1 .. SYS_WALK_OPEN_NAME_MAX; reject '/' '\0' "." "..".)
    //   Returns 0 on success, -1 on: either fd not KOBJ_SPOOR / missing
    //   RIGHT_WRITE; FROM_ROOT with no pivoted root; name bounds / '/' / '\0' /
    //   "." / ".."; cross-Dev (or, for dev9p, cross-session); Dev has no .rename
    //   slot; server Rlerror (source ENOENT; dest a non-empty dir; EXDEV; perm).
    //   Audit-bearing: CLAUDE.md FS-mutation-syscalls (rename / unlink) row.
    SYS_RENAME       = 57,   // arg: olddir_fd, oldname_va, oldname_len, newdir_fd, newname_va, newname_len

    // SYS_UNLINK(parent_fd, name_va, name_len, flags) -> 0 / -1 (FS-gamma; §9.3).
    //   Remove the single component `name` from directory `parent_fd` -- a
    //   non-directory, or (with SYS_UNLINK_REMOVEDIR) an EMPTY directory. 9P
    //   Tunlinkat.
    //   x0 = parent_fd  (hidx_t; KOBJ_SPOOR directory with RIGHT_WRITE -- OR
    //                    FROM_ROOT.)
    //   x1 = name_va    (user-VA of the single-component name to remove.)
    //   x2 = name_len   (1 .. SYS_WALK_OPEN_NAME_MAX; reject '/' '\0' "." "..".)
    //   x3 = flags      (u32; 0 = unlink a non-directory; SYS_UNLINK_REMOVEDIR
    //                    = rmdir an empty directory. Any other bit set -> -1.)
    //   Returns 0 on success, -1 on: parent not KOBJ_SPOOR / missing
    //   RIGHT_WRITE; FROM_ROOT with no pivoted root; name bounds / '/' / '\0' /
    //   "." / ".."; reserved flag bit; Dev has no .unlink slot; server Rlerror
    //   (ENOENT; ENOTEMPTY for rmdir on a non-empty dir; EISDIR/ENOTDIR mode
    //   mismatch; perm). Audit-bearing: CLAUDE.md FS-mutation (rename/unlink) row.
    SYS_UNLINK       = 58,   // arg: parent_fd, name_va, name_len, flags

    // Convergence-detour A-2a (IDENTITY-DESIGN.md §9.5). The chmod/chown
    // MECHANISM: set a file's mode/owner/group via 9P Tsetattr. The kernel 9P
    // client already implements the wire half (p9_client_setattr); this is the
    // syscall wrapper + a new NULL-permitted Dev vtable slot (.wstat_native ->
    // dev9p_wstat_native). Register-passed (no user buffer): the only fields a
    // v1.0 chmod/chown sets are mode + uid + gid, which fit in x1..x4 with the
    // valid mask in x1. SIZE (truncate) + ATIME/MTIME stay out -- separate
    // concerns; a v1.x SYS_WSTAT2 or O_TRUNC covers them.
    //
    // This chunk builds the MECHANISM only. The per-file rwx PERMISSION check
    // (who may chmod/chown -- owner-only chmod, CAP_HOSTOWNER chown) is A-2d
    // (the kernel rwx-enforcement layer); at A-2a the handle RIGHT_WRITE gate is
    // the only gate, and I-22 stands (no rwx enforcement exists yet to bypass).
    //
    // SYS_WSTAT(fd, valid, mode, uid, gid) -> 0 / -1
    //   x0 = fd     (hidx_t; KOBJ_SPOOR with RIGHT_WRITE -- setattr mutates.)
    //   x1 = valid  (u32 bitmask of T_WSTAT_MODE | T_WSTAT_UID | T_WSTAT_GID;
    //                at least one bit set; any other bit -> -1.)
    //   x2 = mode   (u32; new permission bits when T_WSTAT_MODE. The 9 rwx bits
    //                only -- setuid/setgid/sticky (07000) + any bit outside 0777
    //                are REJECTED (-1). setuid is explicitly unsupported, §S5.)
    //   x3 = uid    (u32; new owner principal-id when T_WSTAT_UID. PRINCIPAL_-
    //                INVALID (0) -> -1.)
    //   x4 = gid    (u32; new group when T_WSTAT_GID. GID_INVALID (0) -> -1.)
    //   Returns 0 on success, -1 on: fd not KOBJ_SPOOR / missing RIGHT_WRITE;
    //   valid 0 or with a reserved bit; mode outside 0777; uid/gid INVALID;
    //   Dev has no .wstat_native slot; server Rlerror. Audit-bearing:
    //   CLAUDE.md A-2 FS-permission row.
    SYS_WSTAT        = 59,   // arg: fd (x0), valid (x1), mode (x2), uid (x3), gid (x4)

    // SYS_EXIT_GROUP(status) -- terminate the WHOLE Proc (POSIX exit_group(2)).
    // NEVER returns. Cascades termination to every peer Thread of the calling
    // Proc (proc_group_terminate flags the Proc + wakes/kicks its Threads so
    // each self-exits at its EL0-return die-check), then exits the caller; the
    // LAST Thread out transitions the Proc to ZOMBIE with `status` collapsed to
    // the ok/fail convention (status == 0 -> "ok"/0, else -> "fail"/1; the
    // structured 64-bit status is a Phase-5+ deferral). Replaces the v1.0
    // behavior where _Exit / exit_group routed to SYS_EXITS and EXTINCTED the
    // kernel when the Proc had live peer Threads. pouch rewires
    // __NR_exit_group -> 60; a single-thread Proc gets exits(status)-equivalent
    // semantics. Audit-bearing: CLAUDE.md "Group termination / cross-thread
    // shootdown" row; ARCH §7.9.1 + invariant I-24.
    //   x0 = status (int; 0 = clean exit, non-zero = error)
    SYS_EXIT_GROUP   = 60,   // arg: status (x0); NEVER returns

    // SYS_CAP_GRANT_CLEARANCE -- the A-4a clearance grant-side bridge (the
    // legate analog of SYS_CAP_GRANT). Registers a pending CLEARANCE grant for
    // `target_stripes` by calling cap_register_clearance_grant_for_writer; gated
    // on the writer holding CAP_GRANT_CLEARANCE (corvus). The 32-byte
    // /cap/grant Dev write (devcap_write) is the conceptual production path, but
    // corvus is chrooted to its storage cap and reaches the cap device by
    // syscall (not a /cap file walk) -- exactly as the hostowner grant already
    // does via SYS_CAP_GRANT. The REDEEM still rides SYS_CAP_USE (its unified
    // handler already kind-branches to the clearance core); only the grant side
    // needs this bridge. Returns 0 on success, -1 on any gate/bounds failure
    // (writer lacks CAP_GRANT_CLEARANCE, cap_mask escapes CAP_GRANTABLE_CLEARANCE
    // or is 0, target_stripes == 0, session_id == 0 or > u32, table full).
    //   x0 = cap_mask, x1 = target_stripes, x2 = valid_for_ns, x3 = session_id
    SYS_CAP_GRANT_CLEARANCE = 61,

    // A-5a (login + session): the three boot->session-transition syscalls. The
    // session shape is "joey persists as init": joey runs its boot-test asserts,
    // signals SYS_BOOT_COMPLETE (the banner fires here, NOT on joey exit), drops
    // its boot console-attach (SYS_CONSOLE_RELINQUISH, I-27), and getty-loops
    // /sbin/login on a console handle (SYS_CONSOLE_OPEN). See IDENTITY-DESIGN.md
    // section 9.9 + the ARCH section 25.4 "A-5" audit-trigger row.

    // SYS_BOOT_COMPLETE -- init signals "boot-test asserts passed; the system is
    // up." The kernel prints the "Thylacine boot OK" banner (TOOLING.md section
    // 10 ABI) exactly ONCE here, replacing the post-joey-exit print: joey no
    // longer exits on success (it persists as the session supervisor), so the
    // banner cannot ride joey's reap. GATE: caller must be console-attached (the
    // boot console-trust anchor -- joey, pre-relinquish), so a spawned child
    // cannot spoof a premature banner (-> a false test PASS). ONE-SHOT (a 2nd
    // call is a no-op). No args. Returns 0 (or -1 if not console-attached).
    SYS_BOOT_COMPLETE = 62,

    // SYS_CONSOLE_RELINQUISH -- the caller drops its OWN console-attach
    // (PROC_FLAG_CONSOLE_ATTACHED) and, if it is the current g_console_owner,
    // clears the owner pointer. I-27 carry: during a user session corvus must be
    // the SOLE console-attached Proc, so joey (the boot anchor) relinquishes at
    // the bringup->session boundary -- else a post-SAK state is {joey,corvus}
    // both-attached. SELF-ONLY (cannot revoke another Proc). GATE: caller must be
    // console-attached (can only relinquish what you hold). No args. Returns 0
    // (or -1 if not console-attached).
    SYS_CONSOLE_RELINQUISH = 63,

    // SYS_CONSOLE_OPEN -- attach the kernel UART console Dev (/dev/cons, dc='c')
    // and install a KOBJ_SPOOR handle with RIGHT_READ|RIGHT_WRITE. The session
    // getty (joey) opens this and hands it to /sbin/login as fd 0/1/2 (the Unix
    // login-reads-the-tty model); the A-4c-1 devcons_read blocking read drains
    // the RX ring. No args. Returns the fd (>= 0) or -1. GATED on the caller
    // being console-attached (A-5a audit F2): /dev/cons is a single-reader global,
    // so an ungated open would let a user Proc steal/deny the getty's console
    // input. joey opens it while still attached -- BEFORE SYS_CONSOLE_RELINQUISH --
    // and hands it to login; login + the user shell are never console-attached.
    SYS_CONSOLE_OPEN = 64,

    // SYS_OPEN -- the multi-component pathname open (stalk-1; A-5b-0;
    // docs/STALK-DESIGN.md). Generalizes SYS_WALK_OPEN from one component to a
    // full path resolved by the `stalk` resolver (per-component X-search;
    // '.'/'..' contained at the base; one Dev at v1.0 -- mount-crossing is
    // stalk-2). Supersedes SYS_WALK_OPEN going forward (which remains as the
    // single-component fast path until its callers migrate).
    //   x0 = start_fd : a KOBJ_SPOOR handle (RIGHT_READ) OR
    //                   SYS_WALK_OPEN_FROM_ROOT ((u64)-1) for the Territory root.
    //   x1 = path_va  : user-VA of the path bytes (NUL-free; '/'-separated).
    //   x2 = path_len : 1 .. SYS_OPEN_PATH_MAX.
    //   x3 = omode    : OREAD/OWRITE/ORDWR/OEXEC (+ OTRUNC); SYS_WALK_OPEN_OPATH
    //                   selects a walk-only (unopened) handle.
    // Returns an opened (or O_PATH walkable) KOBJ_SPOOR fd (>= 0) or -1.
    SYS_OPEN = 65,

    // Loom -- the io_uring-inverted shared-memory ring transport for 9P
    // (docs/LOOM.md; ABI in <thylacine/loom.h>). Userspace posts 9P-shaped ops
    // into a submission ring living in a shared Burrow; the kernel's #841
    // elected-reader 9P client drives them; R-messages return as completion
    // queue entries. The opcodes ARE the p9_client_* surface -- no new opcode
    // namespace.

    // SYS_LOOM_SETUP(entries, params_va) -> loom_fd / -1   (Loom-2a)
    //   x0 = entries   : SQ entries; power of two, 1..LOOM_MAX_ENTRIES.
    //   x1 = params_va : user-VA of a `struct loom_params`. IN: params.flags
    //                    (LOOM_SETUP_*; must be 0 at Loom-2a -- SQPOLL/CQSIZE
    //                    land later). OUT: the ring geometry (ring_va + the
    //                    per-region offsets/sizes) the caller maps.
    // Allocates the ring Burrow, maps it RW into the caller (the burrow-attach
    // window), installs a KObj_Loom handle, fills params, returns the fd. -1 on
    // bad args / non-zero flags / OOM / handle-table-full.
    SYS_LOOM_SETUP   = 66,   // arg: entries (x0), params_va (x1)

    // SYS_LOOM_REGISTER(loom_fd, op, arg_va, nargs) -> 0 / -1   (Loom-2a)
    //   x0 = loom_fd : a KObj_Loom handle.
    //   x1 = op      : LOOM_REGISTER_HANDLES (install the fixed-handle table)
    //                  at Loom-2a; LOOM_REGISTER_BUFFERS is reserved (Loom-6).
    //   x2 = arg_va  : LOOM_REGISTER_HANDLES -> user-VA of a u32[nargs] of fds
    //                  (each must be a KOBJ_SPOOR handle in the caller). The
    //                  call REPLACES the whole table (IORING_REGISTER_FILES
    //                  semantics); each registered handle is resolved + its
    //                  rights snapshotted (the I-30 submit-time-pin substrate).
    //   x3 = nargs   : 0..LOOM_MAX_REG_HANDLES.
    // -1 on bad loom_fd / unsupported op / nargs out of range / a non-KOBJ_SPOOR
    // fd in the list.
    SYS_LOOM_REGISTER = 67,  // arg: loom_fd (x0), op (x1), arg_va (x2), nargs (x3)

    // SYS_LOOM_ENTER(loom_fd, to_submit, min_complete, flags) -> n / -1  (Loom-3)
    //   x0 = loom_fd      : a KObj_Loom handle.
    //   x1 = to_submit    : consume up to this many SQEs from the SQ (in SQ-index
    //                       order), dispatch each to the 9P engine (async submit
    //                       + the submit-time pin, docs/LOOM.md 8.5).
    //   x2 = min_complete : after submitting, drive the engine reader until at
    //                       least this many CQEs are available to reap (bounded
    //                       by in-flight ops -- never blocks for completions that
    //                       cannot arrive). The wait is death-interruptible (#811).
    //   x3 = flags        : LOOM_ENTER_* (LOOM_ENTER_GETEVENTS / _NONBLOCK).
    // Returns the number of SQEs consumed (>= 0), or -1 on bad args. Per-op
    // failures (bad opcode / handle / rights) surface as an error CQE (result <
    // 0), NOT a syscall error -- the SQE was still consumed. v1.0 dispatches the
    // no-payload opcodes (NOP / FSYNC); the payload opcodes (READ/WRITE/GETATTR/
    // ...) land with Loom-6's registered-buffer surface and post -ENOSYS until
    // then (docs/LOOM.md 10).
    SYS_LOOM_ENTER   = 68,   // arg: loom_fd (x0), to_submit (x1), min_complete (x2), flags (x3)

    // SYS_CHDIR(path_va, path_len) -> 0 / -1  (LS-4)
    //   Set the caller's Territory per-Proc cwd ("dot") to `path`. A relative
    //   path resolves against the current cwd; an absolute path against the
    //   Territory root. The target must resolve, be a directory, and the caller
    //   must hold search (X) permission on it (a perm_enforced Dev). On success
    //   the cwd becomes the cleaned absolute path; a subsequent SYS_OPEN of a
    //   relative path (with the FROM_ROOT sentinel) resolves against it -- POSIX
    //   openat(AT_FDCWD, ...). dot is SHARED by a Proc's threads (per-process
    //   cwd) and INHERITED by children at spawn (an independent snapshot).
    //   Name-based at v1.0 (a cleaned path string; LIFE-SUPPORT.md LS-4): a
    //   handle-based dot Spoor is the v1.x upgrade, landing with symlinks.
    SYS_CHDIR  = 69,   // arg: path_va (x0), path_len (x1)

    // SYS_GETCWD(buf_va, buf_len) -> len / -1  (LS-4)
    //   Copy the caller's cwd (a cleaned absolute path; "/" until the first
    //   chdir) into the user buffer, NUL-terminated. Returns the path length
    //   (excluding the NUL), or -1 if buf_len is too small for path + NUL.
    SYS_GETCWD = 70,   // arg: buf_va (x0), buf_len (x1)

    // SYS_FD2PATH(fd, buf_va, buf_len) -> len / -1  (#66; the Plan 9 fd2path(2))
    //   Copy the namespace name the fd was reached by (the cleaned path it was
    //   walked/opened along -- e.g. "/bin/joey", "/srv/stratum") into the user
    //   buffer, NUL-terminated. Returns the path length (excluding the NUL), or
    //   -1 if `fd` is not a held KOBJ_SPOOR handle or buf_len is too small for
    //   path + NUL. A fd with NO known name (a nameless attach root, or a walk
    //   from a nameless fd) returns 0 (an empty result -- "unknown"); a real
    //   path always begins with '/', so len == 0 unambiguously means unknown.
    //   No access RIGHT is required (the name is of something the caller already
    //   holds -- or, for an inherited fd, something the spawner walked). The name
    //   is best-effort introspection metadata, NEVER load-bearing (ARCHITECTURE.md
    //   I-33): it may be unknown (empty) OR STALE -- it is the path the Spoor was
    //   reached by, not a live lookup, so a later rename / unmount of a component
    //   can leave it naming a different object. Do NOT use it as a re-open key.
    SYS_FD2PATH = 71,  // arg: fd (x0), buf_va (x1), buf_len (x2)

    // LS-K identity reads (ARCH §22.6). Each returns the calling Proc's field;
    // no args, no memory write, no capability. The field values are < 2^32, so
    // the s64 return is always non-negative (never aliases an error).
    SYS_GETPID = 72,   // -> pid (the per-Proc pid, always > 0)
    SYS_GETUID = 73,   // -> principal_id (the durable user; A-1a)
    SYS_GETGID = 74,   // -> primary_gid (the durable primary group; A-1a)

    // SYS_CLOCK_GETTIME(clk_id, timespec_va) -> 0 / -EINVAL / -EFAULT  (LS-K)
    //   Fill a struct t_timespec at timespec_va for clk_id:
    //     T_CLOCK_REALTIME  (0): wall-clock ns since the Unix epoch
    //     T_CLOCK_MONOTONIC (1): ns since boot (CNTVCT; never goes backward)
    //   Returns 0 on success, -T_E_INVAL on an unknown clk_id, -T_E_FAULT on a
    //   bad timespec_va. The clk_id is validated FIRST, so a bad id never reads
    //   the buffer. See ARCH §22.6.
    SYS_CLOCK_GETTIME = 75,  // arg: clk_id (x0), timespec_va (x1)

    // virtio-PCI transport (pci-1c; docs/VIRTIO-PCI-DESIGN.md). The userspace
    // half of the KObj_PCI mechanism: a CAP_HW_CREATE driver claims a PCI
    // function, maps its BARs, and reads its resolved topology. KObj_PCI is
    // non-transferable (I-5) -- the claimer is always the driver.

    // SYS_PCI_CLAIM(virtio_device_id) -> handle / -1  (pci-1c)
    //   Claim the first VirtIO-PCI function matching virtio_device_id (the
    //   VIRTIO device id: 1 = net, 4 = rng, ...). The kernel assigns + enables
    //   the function's memory BARs, walks the VIRTIO_PCI_CAP_* list into the
    //   region map, and resolves the INTx GIC INTID. On success mints a
    //   KOBJ_PCI handle with FIXED rights R|W|MAP (never TRANSFER -- I-5): a
    //   device owner always needs read + write + map, and the handle cannot be
    //   passed, so there is no partial-rights use case. Requires CAP_HW_CREATE
    //   (like SYS_MMIO_CREATE). Returns -1 on: cap-missing / device-not-found /
    //   already-claimed / BAR-assign failure / malformed cap list / OOM.
    SYS_PCI_CLAIM = 76,    // arg: virtio_device_id (x0)

    // SYS_PCI_MAP_BAR(handle, vaddr, bar_index, prot) -> 0 / -1  (pci-1c)
    //   Install a user-VA mapping at `vaddr` for BAR `bar_index` of a KOBJ_PCI
    //   handle. `prot` is bounded by the handle rights (R|W|MAP); EXEC is
    //   rejected (device memory is not executable) and W-without-R is rejected
    //   (AArch64 has no W-only AP encoding). The mapping spans the full decoded
    //   BAR size; the driver indexes the VIRTIO_PCI_CAP_* regions within it by
    //   the offsets SYS_PCI_INFO reports. Requires RIGHT_MAP + CAP_HW_CREATE.
    //   Returns -1 on: bad handle / wrong kind / missing RIGHT_MAP / out-of-range
    //   or absent BAR / prot exceeds rights / EXEC / prot == 0 / W-without-R /
    //   OOM / VMA overlap.
    SYS_PCI_MAP_BAR = 77,  // arg: handle (x0), vaddr (x1), bar_index (x2), prot (x3)

    // SYS_PCI_INFO(handle, info_va) -> 0 / -1  (pci-1c)
    //   Copy a struct t_pci_info (the resolved BAR + VIRTIO_PCI_CAP_* region map
    //   + the swizzled INTID + bdf) for a KOBJ_PCI handle into the user buffer
    //   at info_va. Requires RIGHT_READ (the fixed claim always has it). Returns
    //   -1 on: bad handle / wrong kind / missing RIGHT_READ / bad info_va.
    SYS_PCI_INFO = 78,     // arg: handle (x0), info_va (x1)

    // SYS_CLOCK_SETTIME(clk_id, timespec_va) -> 0 / -EINVAL / -EFAULT / -EACCES
    //   (net-7a; NET-DESIGN section 10). Step CLOCK_REALTIME to the wall time at
    //   timespec_va. clk_id MUST be T_CLOCK_REALTIME -- MONOTONIC is non-settable
    //   (-T_E_INVAL), the Linux/POSIX rule. Re-anchors the single wall-clock
    //   offset (g_wallclock_offset_ns) at runtime; MONOTONIC is untouched. Gated
    //   on CAP_HOSTOWNER (the fs/clock-admin authority; -T_E_ACCES otherwise) --
    //   a clock step is system-global, so it is the host owner's, never an
    //   identity's. Validates clk_id + the cap BEFORE reading the buffer; a bad
    //   timespec_va -> -T_E_FAULT, a tv_nsec outside [0,1e9) or tv_sec < 0 ->
    //   -T_E_INVAL. The SNTP client (net-7a) is the consumer. See ARCH section
    //   22.6 + the audit-trigger row.
    SYS_CLOCK_SETTIME = 79,  // arg: clk_id (x0), timespec_va (x1)

    // 80 reserved for SYS_FD_DEVCLASS (the Menagerie fd->device-class query;
    // NET-THROUGHPUT.md section 6.1). Not yet built.

    // SYS_WEFT_SHARE(ring_va, ring_size) -> share_id / -1  (Weft-6a-2;
    //   NET-THROUGHPUT.md section 6). The netd side of the per-flow zero-copy
    //   ring: register an ANON Burrow (mapped at ring_va in the caller's AS,
    //   whole-ring, RW / no-exec) as a flow ring. Takes the I-30 registration
    //   pin (burrow_ref) + mints a kernel-scoped share_id (never 0) the kernel
    //   echoes in Rweft and claims at SYS_WEFT_MAP. The share_id is inert in any
    //   other hand (it only maps a ring when the kernel's own Tweft round-trip
    //   returns it), so no capability is required to mint one. -1 on: NULL Proc /
    //   ring_va not an ANON-Burrow VMA start / ring_size != the Burrow size /
    //   an exec mapping / a full registry.
    SYS_WEFT_SHARE = 81,   // arg: ring_va (x0), ring_size (x1)

    // SYS_WEFT_MAP(data_fd, hint_va) -> ring_va / -1  (Weft-6a-2;
    //   NET-THROUGHPUT.md section 6). The guest side: lazily map a /net data
    //   fd's per-flow ring into the caller. Resolves data_fd -> (client, fid F)
    //   via dev9p_client_fid, issues Tweft(F) -> Rweft(share_id) on the first
    //   zero-copy use, claims the share_id (consume-once), burrow_share_into's
    //   the ring, and records the binding in the data Spoor's dev9p_priv.
    //   Idempotent (a second call returns the cached ring_va without a second
    //   Tweft). hint_va is reserved (v1.0 ignores it; the kernel picks the VA in
    //   the burrow-attach window). -1 on: bad fd / not a dev9p file / Tweft
    //   failure (e.g. a server with no Tweft handler) / a bad share_id / OOM.
    SYS_WEFT_MAP = 82,     // arg: data_fd (x0), hint_va (x1)

    // Overcommit / I-32 (ARCH section 6.5 "The overcommit model"). A DEDICATED
    // lazy-attach syscall (user-voted 2026-06-23 over a flags-on-SYS_BURROW_ATTACH
    // arg, for blast-radius discipline -- the eager SYS_BURROW_ATTACH = 37 stays a
    // 1-arg syscall, byte-identical; the Plan 9 small-syscall idiom).
    //
    // SYS_BURROW_ATTACH_LAZY(length) -> vaddr / -1. The demand-ZERO twin of
    //   SYS_BURROW_ATTACH: reserves a VA + VMA + a sparse BURROW_TYPE_ANON_LAZY
    //   Burrow in the burrow-attach window but commits NO physical pages. Each page
    //   faults in zero-filled (RW/XN, W^X-clean) on first touch, and the I-32
    //   page_count is charged THERE (per page) -- the whole point is a free
    //   reservation, so page_count tracks true RSS. The VMA-count axis (PROC_VMA_MAX)
    //   IS charged at attach, so a free reservation cannot exhaust the vma slab.
    //   -1 on: length == 0 / length > BURROW_ATTACH_MAX / no free gap / OOM. Same
    //   page-rounding as SYS_BURROW_ATTACH.
    SYS_BURROW_ATTACH_LAZY = 83,  // arg: length (x0)

    // SYS_BURROW_DECOMMIT(vaddr, length) -> 0 / -1. The madvise(MADV_DONTNEED)
    //   analog: release the resident pages backing [vaddr, vaddr+length) of a
    //   BURROW_TYPE_ANON_LAZY mapping WITHOUT removing the VMA. Clears each PTE
    //   (+ TLBI before the page frees to the buddy), frees the page, NULLs the sparse
    //   slot, and uncharges page_count. The VMA + reservation stay; a later touch
    //   re-faults a fresh zero page. Idempotent on never-faulted pages. Confined to
    //   the burrow-attach window (like SYS_BURROW_DETACH); rejects a non-ANON_LAZY
    //   VMA / a range outside one VMA. Backs the Go runtime's sysUnused (the GC
    //   shrinks RSS). -1 on a bad range / wrong VMA type.
    SYS_BURROW_DECOMMIT = 84,     // arg: vaddr (x0), length (x1)
};

// SYS_CLOCK_GETTIME clock ids. Values match Linux clockid_t so a future pouch
// boundary-line maps clock_gettime 1:1.
#define T_CLOCK_REALTIME   0
#define T_CLOCK_MONOTONIC  1

// SYS_CLOCK_GETTIME timestamp record. 16 bytes, the musl/arm64 struct timespec
// layout (time_t tv_sec = i64, long tv_nsec = i64). _Static_asserts pin the
// size + offsets so libt / libthyla-rs / a future pouch patch decode a fixed
// record. tv_nsec is in [0, 1e9).
struct t_timespec {
    s64 tv_sec;    // 0: seconds (since the Unix epoch for REALTIME; since boot
                   //    for MONOTONIC)
    s64 tv_nsec;   // 8: nanoseconds within the second, [0, 1000000000)
};
_Static_assert(sizeof(struct t_timespec) == 16,
               "struct t_timespec is a SYS_CLOCK_GETTIME ABI type -- pinned at "
               "16 bytes (the musl/arm64 struct timespec layout)");
_Static_assert(__builtin_offsetof(struct t_timespec, tv_sec)  == 0,
               "t_timespec.tv_sec at ABI offset 0");
_Static_assert(__builtin_offsetof(struct t_timespec, tv_nsec) == 8,
               "t_timespec.tv_nsec at ABI offset 8");

// SYS_PCI_INFO record (pci-1c). The kernel copies a zero-initialized,
// fully-filled t_pci_info out (no uninitialized padding -> no info leak); the
// _Static_asserts pin every field offset so libt / libthyla-rs / a future pouch
// patch decode a fixed layout. All fields are fixed-width + naturally aligned;
// explicit _pad makes the layout deterministic. The PA/size fields name the
// device's own BAR window (the owner already controls it) -- no kernel-VA /
// RAM-PA leak (the BAR window is the host-bridge MMIO aperture, not RAM).
struct t_pci_bar {
    u64 pa;        // 0:  assigned device PA (page-aligned); 0 if !present
    u64 size;      // 8:  decoded BAR size (power of 2); 0 if !present
    u8  present;   // 16: 1 if an implemented, assigned MEM BAR
    u8  is_64;     // 17: 1 if a 64-bit BAR (consumes the following slot)
    u8  _pad[6];   // 18: pad to 24
};
_Static_assert(sizeof(struct t_pci_bar) == 24, "t_pci_bar ABI size 24");
_Static_assert(__builtin_offsetof(struct t_pci_bar, pa)      == 0,  "t_pci_bar.pa @0");
_Static_assert(__builtin_offsetof(struct t_pci_bar, size)    == 8,  "t_pci_bar.size @8");
_Static_assert(__builtin_offsetof(struct t_pci_bar, present) == 16, "t_pci_bar.present @16");
_Static_assert(__builtin_offsetof(struct t_pci_bar, is_64)   == 17, "t_pci_bar.is_64 @17");

struct t_pci_region {
    u32 offset;    // 0: byte offset of the cap structure within bars[bar]
    u32 length;    // 4: byte length (offset + length <= bars[bar].size)
    u8  bar;       // 8: which BAR holds it (< 6, present)
    u8  present;   // 9: 1 if this VIRTIO_PCI_CAP region was resolved
    u8  _pad[2];   // 10: pad to 12
};
_Static_assert(sizeof(struct t_pci_region) == 12, "t_pci_region ABI size 12");
_Static_assert(__builtin_offsetof(struct t_pci_region, offset)  == 0, "t_pci_region.offset @0");
_Static_assert(__builtin_offsetof(struct t_pci_region, length)  == 4, "t_pci_region.length @4");
_Static_assert(__builtin_offsetof(struct t_pci_region, bar)     == 8, "t_pci_region.bar @8");
_Static_assert(__builtin_offsetof(struct t_pci_region, present) == 9, "t_pci_region.present @9");

struct t_pci_info {
    struct t_pci_bar    bars[6];     // 0:   the assigned memory BARs
    struct t_pci_region regions[4];  // 144: VIRTIO_PCI_CAP regions, (cfg_type - 1):
                                     //      [0]=COMMON [1]=NOTIFY [2]=ISR [3]=DEVICE
    u32 notify_off_multiplier;       // 192: from the NOTIFY_CFG cap
    u32 intid;                       // 196: swizzled GIC INTID (valid iff intid_valid)
    u8  intid_valid;                 // 200: 1 if the DTB interrupt-map resolved
    u8  bus;                         // 201
    u8  dev;                         // 202
    u8  fn;                          // 203
    u16 virtio_device_id;            // 204
    u8  _pad[2];                     // 206: pad to 208
};
_Static_assert(sizeof(struct t_pci_info) == 208, "t_pci_info ABI size 208");
_Static_assert(__builtin_offsetof(struct t_pci_info, bars)    == 0,   "t_pci_info.bars @0");
_Static_assert(__builtin_offsetof(struct t_pci_info, regions) == 144, "t_pci_info.regions @144");
_Static_assert(__builtin_offsetof(struct t_pci_info, notify_off_multiplier) == 192,
               "t_pci_info.notify_off_multiplier @192");
_Static_assert(__builtin_offsetof(struct t_pci_info, intid)       == 196, "t_pci_info.intid @196");
_Static_assert(__builtin_offsetof(struct t_pci_info, intid_valid) == 200, "t_pci_info.intid_valid @200");
_Static_assert(__builtin_offsetof(struct t_pci_info, bus)         == 201, "t_pci_info.bus @201");
_Static_assert(__builtin_offsetof(struct t_pci_info, dev)         == 202, "t_pci_info.dev @202");
_Static_assert(__builtin_offsetof(struct t_pci_info, fn)          == 203, "t_pci_info.fn @203");
_Static_assert(__builtin_offsetof(struct t_pci_info, virtio_device_id) == 204,
               "t_pci_info.virtio_device_id @204");

// SYS_WALK_OPEN's FROM_ROOT sentinel: when passed as the spoor_fd, the
// kernel uses the caller's Territory's root_spoor as the walk source
// instead of looking up a handle. (u64)-1 chosen because hidx_t fds are
// non-negative; the cast is unambiguous.
#define SYS_WALK_OPEN_FROM_ROOT  ((u64)(-1))

// SYS_SPAWN_WITH_PERMS perm_flags bits — must mirror the libt / libthyla-rs
// constants. New bits add atomically without an ABI break (a Proc spawned
// by a parent that did not know the new bit behaves identically to today).
//
// SPAWN_PERM_MAY_POST_SERVICE stamps PROC_FLAG_MAY_POST_SERVICE on the
// spawned child (CORVUS-DESIGN.md §6.1). joey grants this to /sbin/corvus
// so corvus may call SYS_POST_SERVICE("corvus") and become the /srv/corvus
// 9P server. NOT a cap because rfork does not propagate it.
#define SPAWN_PERM_MAY_POST_SERVICE  (1u << 0)
// SPAWN_PERM_CONSOLE_TRUSTED (A-4c-2) records the spawned child as the trusted
// login authority -- the target a kernel SAK re-grants the console to
// (proc_set_console_trusted -> g_console_trusted_proc). joey grants this to
// /sbin/corvus. Like every SPAWN_PERM_* bit it is gate-checked at spawn (the
// granting caller must itself be console-attached), so only the console-trust
// chain can designate the SAK re-grant target. NOT a cap (rfork does not
// propagate it).
#define SPAWN_PERM_CONSOLE_TRUSTED   (1u << 1)
// SPAWN_PERM_CONSOLE_OWNER (LS-5) records the spawned child as the console
// OWNER (g_console_owner) -- the Proc that receives the `interrupt` note when
// the human hits Ctrl-C on /dev/cons. This is console-*owner* ("who receives
// Ctrl-C"), strictly DISTINCT from console-*attach* (PROC_FLAG_CONSOLE_ATTACHED
// / SPAWN_PERM_CONSOLE_TRUSTED, the SAK + hostowner-elevation gate, I-27): the
// owner bit NEVER confers attach, so I-27 is untouched. Gated like
// MAY_POST_SERVICE (a console-attached granter OR a Proc that already holds
// MAY_POST_SERVICE), so trusted /sbin/login -- holding MAY_POST_SERVICE from
// joey -- confers it on the session shell `ut`. NOT a cap (rfork does not
// propagate it); the child becomes the owner but, lacking MAY_POST_SERVICE,
// cannot itself re-designate the owner.
#define SPAWN_PERM_CONSOLE_OWNER     (1u << 2)
#define SPAWN_PERM_ALL               (SPAWN_PERM_MAY_POST_SERVICE | \
                                      SPAWN_PERM_CONSOLE_TRUSTED | \
                                      SPAWN_PERM_CONSOLE_OWNER)

// A-1a (docs/IDENTITY-DESIGN.md §9.1): sys_spawn_args.identity_flags bits.
// SPAWN_IDENTITY_SET requests that the child be born with the principal_id
// / primary_gid / supp_gids carried in the struct (instead of inheriting
// the parent's identity). FAIL-CLOSED gate: a SET request from a caller
// lacking CAP_SET_IDENTITY is rejected with -1 (never silently inherited).
// Clear -> the child inherits (the default; no cap needed).
#define SPAWN_IDENTITY_SET           (1u << 0)
#define SPAWN_IDENTITY_FLAGS_ALL     (SPAWN_IDENTITY_SET)

// Menagerie build-arc step 5 (docs/MENAGERIE.md §4; ARCH §28 I-34):
// sys_spawn_args.allowance_flags bits. SPAWN_ALLOWANCE_SET requests that the
// child be born with the NARROWED hardware allowance described by the
// t_allowance_desc at allowance_va (instead of inheriting the parent's), so a
// warden-spawned driver may create KObj_MMIO/IRQ/DMA handles ONLY within its
// device's resources. The grant is a NARROWING: the kernel rejects (-1) a
// conferred set not within the *parent's* own allowance (I-2's hardware-axis
// analog -- a broad parent, allowance == NULL, may confer anything; a narrowed
// parent only a subset of its own). Clear -> the child inherits the parent's
// allowance (the default; the as-built broad-server path). No capability is
// required to NARROW (you cannot gain authority by restricting), so this is a
// perm/identity-shaped spawn-time grant, never an rfork-propagated cap bit.
#define SPAWN_ALLOWANCE_SET          (1u << 0)
#define SPAWN_ALLOWANCE_FLAGS_ALL    (SPAWN_ALLOWANCE_SET)

// SYS_SPAWN_FULL_ARGV hardware-allowance descriptor (Menagerie build-arc step
// 5). The warden fills this in user memory and points sys_spawn_args.
// allowance_va at it with SPAWN_ALLOWANCE_SET; the kernel uaccess-copies it,
// validates the counts, gates it against the parent's allowance, and confers
// it on the child Proc before the child enters EL0 (the proc_confer_allowance
// set-once-before-EL0 contract). The mmio[]/irq[] caps mirror the kernel
// struct Allowance's ALLOWANCE_MMIO_MAX / ALLOWANCE_IRQ_MAX (both 8; a
// _Static_assert in kernel/syscall.c pins the equality). A binding ABI type:
// the layout is offset-pinned below.
//
//   mmio[8]      8 x { u64 base; u64 size; }  -- permitted MMIO PA windows
//   mmio_count   u32  -- 0..8; entries [mmio_count..8) ignored
//   irq_count    u32  -- 0..8; entries [irq_count..8) ignored
//   irq[8]       8 x u32                       -- permitted IRQ INTIDs (SPI>=32)
//   dma_max      u64  -- max bytes per KObj_DMA; 0 = no DMA permitted
//   pci_count    u32  -- 0..8; entries [pci_count..8) ignored
//   pci[8]       8 x u32 (packed bus<<16|dev<<8|fn) -- permitted PCI functions
//   _pad_pci     u32  -- explicit tail pad (216 = 8-aligned; no implicit pad)
struct t_hw_window {
    u64 base;
    u64 size;
};

struct t_allowance_desc {
    struct t_hw_window mmio[8];
    u32 mmio_count;
    u32 irq_count;
    u32 irq[8];
    u64 dma_max;
    u32 pci_count;
    u32 pci[8];
    u32 _pad_pci;
};

_Static_assert(sizeof(struct t_allowance_desc) == 216,
               "struct t_allowance_desc is a SYS_SPAWN_FULL_ARGV ABI type "
               "-- pinned at 216 bytes (mmio[8] 128 + mmio_count 4 + "
               "irq_count 4 + irq[8] 32 + dma_max 8 + pci_count 4 + pci[8] 32 "
               "+ _pad_pci 4); no implicit padding");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, mmio) == 0,
               "t_allowance_desc.mmio at ABI offset 0");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, mmio_count) == 128,
               "t_allowance_desc.mmio_count at ABI offset 128");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, irq_count) == 132,
               "t_allowance_desc.irq_count at ABI offset 132");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, irq) == 136,
               "t_allowance_desc.irq at ABI offset 136");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, dma_max) == 168,
               "t_allowance_desc.dma_max at ABI offset 168");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, pci_count) == 176,
               "t_allowance_desc.pci_count at ABI offset 176");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, pci) == 180,
               "t_allowance_desc.pci at ABI offset 180");

// SYS_SRV_PEER result — the kernel-stamped peer identity of a /srv
// connection (CORVUS-DESIGN.md §6.3). The kernel writes one of these to
// the SYS_SRV_PEER out_va buffer. A syscall-ABI type: the layout is
// pinned by the _Static_asserts so a userspace consumer (corvus, at
// P5-corvus-srv-impl-b) decodes a fixed 40-byte record.
//
// A-1a (docs/IDENTITY-DESIGN.md §9.1) appended the identity fields AFTER
// `alive` (24 -> 40 bytes; existing offsets unchanged). principal_id /
// primary_gid are resolved FRESH per query (same walk as caps + alive);
// a dead/reaped peer fail-closes them to PRINCIPAL_NONE / GID_NONE.
struct srv_peer_info {
    u64 stripes;       // @0  peer Proc's identity tag (0 → unidentifiable peer)
    u64 caps;          // @8  peer's live capability set; 0 when alive == 0
    u32 console;       // @16 1 iff the peer is console-attached, else 0
    u32 alive;         // @20 1 iff an ALIVE Proc still carries `stripes`, else 0
    u32 principal_id;  // @24 A-1a: peer's durable identity; NONE when alive == 0
    u32 primary_gid;   // @28 A-1a: peer's primary group; NONE when alive == 0
    u32 flags;         // @32 A-1a: reserved, 0 at v1.0
    u32 _reserved;     // @36 A-1a: explicit pad to 40; reserved, 0
};

_Static_assert(sizeof(struct srv_peer_info) == 40,
               "struct srv_peer_info is a SYS_SRV_PEER ABI type — pinned "
               "at 40 bytes (A-1a appended principal_id + primary_gid + "
               "flags + _reserved after the live `alive` field), naturally "
               "aligned with no implicit padding");
_Static_assert(__builtin_offsetof(struct srv_peer_info, stripes) == 0,
               "srv_peer_info.stripes at ABI offset 0");
_Static_assert(__builtin_offsetof(struct srv_peer_info, caps) == 8,
               "srv_peer_info.caps at ABI offset 8");
_Static_assert(__builtin_offsetof(struct srv_peer_info, console) == 16,
               "srv_peer_info.console at ABI offset 16");
_Static_assert(__builtin_offsetof(struct srv_peer_info, alive) == 20,
               "srv_peer_info.alive at ABI offset 20");
_Static_assert(__builtin_offsetof(struct srv_peer_info, principal_id) == 24,
               "srv_peer_info.principal_id at ABI offset 24 (after alive@20)");
_Static_assert(__builtin_offsetof(struct srv_peer_info, primary_gid) == 28,
               "srv_peer_info.primary_gid at ABI offset 28");
_Static_assert(__builtin_offsetof(struct srv_peer_info, flags) == 32,
               "srv_peer_info.flags at ABI offset 32");
_Static_assert(__builtin_offsetof(struct srv_peer_info, _reserved) == 36,
               "srv_peer_info._reserved at ABI offset 36");

// SYS_FSTAT result — the Thylacine-native file metadata record (P6-pouch-
// stratumd-boot sub-chunk 16b-gamma). Plan-9-shaped at the core (qid carries
// the 9P identity verbatim), POSIX-shaped at the surface (mode + size +
// timestamps) so pouch's fstat() implementation can fill musl's
// arch-specific `struct stat` from this without a Linux-shaped intermediate.
//
// 80 bytes, naturally aligned; the _Static_asserts pin every field offset
// so a userspace consumer (libt, pouch's fstat patch, libthyla-rs) decodes a
// fixed record. A-2a (IDENTITY-DESIGN.md §9.5) appended uid + gid AFTER the
// 72-byte 16b-gamma tail (existing offsets unchanged), the durable owner +
// group the kernel rwx layer (A-2d) reads. There is no reserved tail today; a
// further field add extends the record again (every consumer rebuilds in
// lockstep -- no persistent on-disk consumer of this ABI exists). devramfs
// reports PRINCIPAL_SYSTEM / GID_SYSTEM (the boot FS is system-owned); dev9p
// reports the server's Rgetattr uid/gid verbatim.
struct t_stat {
    u64 size;            // 0: file size in bytes
    u64 qid_path;        // 8: 9P qid.path (unique within Dev — inode-ish)
    u64 atime_sec;       // 16: access time (epoch seconds; v1.0 = 0)
    u64 mtime_sec;       // 24: modify time (epoch seconds; v1.0 = 0)
    u64 ctime_sec;       // 32: change time (epoch seconds; v1.0 = 0)
    u32 mode;            // 40: POSIX mode bits (S_IFREG|0644 typical)
    u32 nlink;           // 44: link count (1 for regular files at v1.0)
    u32 qid_vers;        // 48: 9P qid.vers (version counter; v1.0 = 0)
    u8  qid_type;        // 52: 9P qid.type (QTFILE / QTDIR / ...)
    u8  _pad_qid[3];     // 53: padding to align blksize
    u32 blksize;         // 56: preferred I/O size hint
    u32 _pad_blksize;    // 60: padding to align blocks
    u64 blocks;          // 64: count of 512-byte blocks
    u32 uid;             // 72: A-2a owner principal-id (PRINCIPAL_SYSTEM/NONE/real)
    u32 gid;             // 76: A-2a owning group (GID_SYSTEM/NONE/real)
};

_Static_assert(sizeof(struct t_stat) == 80,
               "struct t_stat is a SYS_FSTAT ABI type — pinned at 80 bytes "
               "(A-2a appended u32 uid + u32 gid after the 72-byte tail); "
               "any field add updates the size + asserts");
_Static_assert(__builtin_offsetof(struct t_stat, size)      ==  0, "t_stat.size at ABI offset 0");
_Static_assert(__builtin_offsetof(struct t_stat, qid_path)  ==  8, "t_stat.qid_path at ABI offset 8");
_Static_assert(__builtin_offsetof(struct t_stat, atime_sec) == 16, "t_stat.atime_sec at ABI offset 16");
_Static_assert(__builtin_offsetof(struct t_stat, mtime_sec) == 24, "t_stat.mtime_sec at ABI offset 24");
_Static_assert(__builtin_offsetof(struct t_stat, ctime_sec) == 32, "t_stat.ctime_sec at ABI offset 32");
_Static_assert(__builtin_offsetof(struct t_stat, mode)      == 40, "t_stat.mode at ABI offset 40");
_Static_assert(__builtin_offsetof(struct t_stat, nlink)     == 44, "t_stat.nlink at ABI offset 44");
_Static_assert(__builtin_offsetof(struct t_stat, qid_vers)  == 48, "t_stat.qid_vers at ABI offset 48");
_Static_assert(__builtin_offsetof(struct t_stat, qid_type)  == 52, "t_stat.qid_type at ABI offset 52");
_Static_assert(__builtin_offsetof(struct t_stat, blksize)   == 56, "t_stat.blksize at ABI offset 56");
_Static_assert(__builtin_offsetof(struct t_stat, blocks)    == 64, "t_stat.blocks at ABI offset 64");
_Static_assert(__builtin_offsetof(struct t_stat, uid)       == 72, "t_stat.uid at ABI offset 72 (A-2a)");
_Static_assert(__builtin_offsetof(struct t_stat, gid)       == 76, "t_stat.gid at ABI offset 76 (A-2a)");

// SYS_LSEEK whence values (P6-pouch-stratumd-boot sub-chunk 16b-gamma).
// Mirror POSIX SEEK_SET / SEEK_CUR / SEEK_END but stay namespaced so
// the kernel ABI never depends on a libc header. The libt + pouch arms
// surface the standard POSIX names on top of these.
#define T_SEEK_SET  0
#define T_SEEK_CUR  1
#define T_SEEK_END  2

// POSIX-shaped mode bits used in struct t_stat.mode. Subset that v1.0
// devs actually populate (regular file / directory / character device);
// kept inline rather than pulling in a POSIX header so the kernel does
// not learn a libc.
#define T_S_IFMT    0170000u
#define T_S_IFREG   0100000u
#define T_S_IFDIR   0040000u
#define T_S_IFCHR   0020000u

// SYS_WSTAT valid-mask bits (A-2a; IDENTITY-DESIGN.md §9.5). Which of the
// (mode, uid, gid) register arguments the kernel applies. Chosen to equal the
// 9P Tsetattr P9_SETATTR_{MODE,UID,GID} bit values so dev9p_wstat_native maps
// the mask with no translation; the equality is pinned by a _Static_assert in
// dev9p.c (the only TU that sees both). Userspace chmod() sets T_WSTAT_MODE;
// chown() sets T_WSTAT_UID | T_WSTAT_GID (or just one).
#define T_WSTAT_MODE   (1u << 0)
#define T_WSTAT_UID    (1u << 1)
#define T_WSTAT_GID    (1u << 2)
#define T_WSTAT_VALID  (T_WSTAT_MODE | T_WSTAT_UID | T_WSTAT_GID)

// Permission bits a SYS_WSTAT mode may carry (the 9 rwx bits). setuid/setgid/
// sticky (07000) + any bit outside 0777 are rejected -- setuid is explicitly
// unsupported (IDENTITY-DESIGN.md §S5; the kernel never honors a setuid bit).
#define T_WSTAT_MODE_MASK  0777u

// SYS_GETRANDOM flags.
#define GRND_NONBLOCK    1u

// Maximum aname length per SYS_ATTACH_9P call. The aname is a server-
// side path or capability string (typically short — "/", "tcp!host!port",
// "pool/data"). 256 bytes is generous.
#define SYS_ATTACH_ANAME_MAX  256u

// Maximum bytes transferred per SYS_READ / SYS_WRITE call. Userspace
// loops for larger transfers. Kept at PIPE_BUF_SIZE (4 KiB) to match
// the kernel pipe ring buffer; longer single calls would either need
// a heap scratch (avoidable for v1.0) or per-call segmented copy.
#define SYS_RW_MAX  4096u

// Maximum binary name length for SYS_SPAWN. Most callers pass short
// devramfs/`/bin` names, but the on-device Go toolchain (Stage 4c) execs
// absolute tool paths (`/goroot/pkg/tool/thylacine_arm64/compile`) and
// $WORK-relative output paths, so 256 bytes accommodates a deep namespace
// path. Bounds the kernel-stack `name[]` scratch (257 bytes — small).
#define SYS_SPAWN_NAME_MAX  256u

// Maximum ELF blob size for SYS_SPAWN. v1.0 userspace binaries that
// stay near the cap: /pouch-hello-sodium = 276848 bytes at sub-chunk 14
// (libsodium ed25519 + chacha20-poly1305 + SHA-256 + BLAKE2b pulled in
// from libsodium.a). stratumd (sub-chunk 16) will pull in libsodium too
// and is expected to land around 600-800 KiB. kmalloc routes >2 KiB
// requests through alloc_pages so larger sizes just allocate more pages
// — no algorithmic cost — bounded by MAX_ORDER (1 GiB).
#define SYS_SPAWN_BLOB_MAX  1048576u

// Maximum number of fds that can be inherited via SYS_SPAWN_WITH_FDS
// per call. 16 is generous for the v1.0 use case (joey passes 2 to
// stratumd-stub; future per-user stratumd takes 2-3). Bounds the
// kernel-stack scratch for the fd-list copy.
#define SYS_SPAWN_MAX_FDS   16u

// Maximum argc for SYS_SPAWN_FULL_ARGV. Raised from 16 (the original
// stratumd-CLI sizing) to 512 for the on-device Go toolchain (Stage 4c):
// `go build` execs `compile` with one argv entry per source file plus its
// flags, and the widest stdlib package (runtime/arm64) is ~200 .go+.s
// files. 512 leaves headroom. Bounds the argv-pointer array in
// exec_build_init_stack ((argc+1)*8 = ~4 KiB at 512).
#define SYS_SPAWN_ARGV_MAX  512u

// Maximum total bytes of the argv buffer for SYS_SPAWN_FULL_ARGV (the
// concatenated NUL-terminated strings). Raised from 4 KiB to 64 KiB for the
// Go toolchain: a runtime compile passes ~200 $WORK-relative source paths
// (~50 B each) ≈ 25 KiB + flags. The kernel copies it via kmalloc (the
// handler's buffer is heap, NOT a kernel-stack array — the 16 KiB kstack
// could not hold 64 KiB), and the System V init frame (structured + the
// strings region, ≤ ~68 KiB) sits at the top of the 256 KiB user stack
// (EXEC_USER_STACK_SIZE) with ~188 KiB to spare.
#define SYS_SPAWN_ARGV_DATA_MAX  65536u

// Maximum single-component name length for SYS_WALK_OPEN. Matches the
// Plan 9 + 9P2000.L practical cap for a single path element; bounds the
// kernel-stack scratch + the per-byte uaccess loop. The wire codec's
// per-name cap is larger; we choose the smaller bound here because v1.0
// callers (joey, the bringup probes) all walk short server-stamped names.
#define SYS_WALK_OPEN_NAME_MAX  64u

// Maximum full-path length for SYS_OPEN (stalk-1). Bounds the kernel-stack
// path scratch + the per-byte uaccess copy. A path is at most STALK_MAX_DEPTH
// components, each <= SYS_WALK_OPEN_NAME_MAX, plus separators; 1024 comfortably
// covers v1.0 absolute paths (/var/lib/corvus, /home/<user>/...).
#define SYS_OPEN_PATH_MAX  1024u

// Permitted omode bits for SYS_WALK_OPEN. Plan 9 modes: OREAD=0,
// OWRITE=1, ORDWR=2, OEXEC=3 (low 2 bits) plus the OTRUNC modifier
// (0x10). Phase 5+ may extend (OCEXEC=0x20, ORCLOSE=0x40, OEXCL=0x1000)
// as callers materialize. Any bit outside this mask → -1.
//
// FS-delta (IDENTITY-DESIGN.md §9.4): SYS_WALK_OPEN_OPATH = 0x80 is the
// Linux-O_PATH / Plan-9-walk equivalent -- walk to the component but do
// NOT Tlopen it, returning a NON-OPENED, walkable KObj_Spoor. 9P forbids
// Twalk from an OPENED fid, so a normally-opened handle is NOT a valid
// base for creating/walking/renaming/unlinking/fsync'ing CHILDREN; an
// O_PATH handle IS (and is a valid SYS_CHROOT target). When set, the
// access bits are ignored (the handle carries no byte-I/O open).
#define SYS_WALK_OPEN_OPATH        0x80u
#define SYS_WALK_OPEN_OMODE_VALID  0x93u

// SYS_WALK_CREATE perm: the Plan 9 perm word. Low 9 bits are the POSIX
// rwxrwxrwx mode; the DMDIR bit selects directory creation. All other Plan 9
// DM* bits (DMAPPEND 0x40000000, DMEXCL 0x20000000, DMTMP 0x04000000, ...) are
// reserved 0 at v1.0 -- a perm with any bit outside SYS_WALK_CREATE_PERM_VALID
// is rejected with -1 (so a future bit cannot be silently ignored).
#define SYS_WALK_CREATE_DMDIR       0x80000000u
// DMSRVBYTE (Thylacine extension; stalk-3b, STALK-DESIGN.md §5.3 / D6): on a
// CREATE against a /srv directory (a devsrv root), this perm bit posts the new
// service in BYTE mode; its absence posts 9P mode (the Plan 9 DM* perm-bit
// idiom -- mode-of-the-service is an attribute, not an open intent). Bit 25 is
// unused by Plan 9's standard DM set (DMDIR/DMAPPEND/DMEXCL/DMMOUNT/DMAUTH/
// DMTMP occupy the top six bits), so it cannot collide. It is admitted by
// SYS_WALK_CREATE_PERM_VALID so it reaches the devsrv-post branch of
// sys_walk_create_handler; that branch is the ONLY place it is meaningful -- a
// regular (non-/srv) create rejects it (it must not leak into a dev9p Tlcreate
// perm). For a service post the only valid perm bits are {0, DMSRVBYTE}.
#define SYS_WALK_CREATE_DMSRVBYTE   0x02000000u
#define SYS_WALK_CREATE_PERM_VALID  (0x1FFu | SYS_WALK_CREATE_DMDIR | \
                                     SYS_WALK_CREATE_DMSRVBYTE)
_Static_assert((SYS_WALK_CREATE_DMSRVBYTE &
                (0x1FFu | SYS_WALK_CREATE_DMDIR)) == 0,
               "DMSRVBYTE must not collide with the mode bits or DMDIR");

// SYS_UNLINK flags: the only permitted bit at v1.0 is SYS_UNLINK_REMOVEDIR
// (rmdir an empty directory vs unlink a non-directory). Mirrors the wire
// constant P9_UNLINK_AT_REMOVEDIR; passed straight through dev9p_unlink to
// p9_client_unlinkat (a _Static_assert in kernel/syscall.c pins the equality).
// Any other bit in the flags arg is rejected with -1.
#define SYS_UNLINK_REMOVEDIR        0x200u

// Maximum length for a single SYS_BURROW_ATTACH (and the matching
// SYS_BURROW_DETACH). A v1.0 sanity bound: burrow_create_anon allocates
// pages eagerly through the buddy allocator (which tops out at order 18
// = 1 GiB; ARCHITECTURE.md §6.3), so a single attach far larger than any
// v1.0 workload's arena is almost certainly a bug or an overflow. 256
// MiB is generous headroom for stratumd / libsodium yet keeps the eager
// allocation bounded — pouch's mallocng makes many modest attaches,
// never one enormous one. Being page-aligned, it also bounds the
// page-rounding so `length + PAGE_SIZE` cannot overflow.
#define BURROW_ATTACH_MAX  (256u * 1024u * 1024u)

// Overcommit / I-32 (ARCH §6.5): the maximum length for a single LAZY reservation
// (SYS_BURROW_ATTACH_LAZY). DISTINCT from BURROW_ATTACH_MAX because a lazy
// reservation commits NO data pages at attach -- the eager 256-MiB bound (sized for
// the committed allocation) would defeat the whole purpose (Go's stock 64-bit page
// allocator reserves a ~512-MiB page-summary; #321). The real bounds on a lazy
// region's resource use are page_count (charged at FAULT, per touched page) +
// PROC_VMA_MAX (the slab DoS) -- NOT the reservation byte size. 1 GiB is 2x Go-stock
// with headroom; the only eager cost is the sparse `filepages` array (8 B / reserved
// page -> 2 MiB for 1 GiB, a kmalloc -> alloc_pages order-9, within MAX_ORDER 18).
// v1.x SEAM: the flat eager `filepages` array is UNCHARGED kernel memory and does not
// scale to huge reservations -- a per-Proc array-DoS bounded today only by graceful-
// OOM (alloc fails -> attach -1) + PROC_VMA_MAX; the fix is a charged radix/sparse
// metadata structure (the Linux page-table-radix shape), which also lifts this cap.
#define BURROW_RESERVE_MAX  (1024ull * 1024ull * 1024ull)

// SYS_SPAWN_FULL_ARGV argument record (P6-pouch-stratumd-boot sub-chunk
// 16b-alpha). The caller fills this in user memory and passes its
// user-VA in x0; the kernel uaccess-copies the struct, then each
// referenced buffer (name, argv_data, fd_list) by walking the pointers.
//
// The layout is the binding ABI for the syscall — fields are 8-aligned;
// the trailing `_pad_envp` field is reserved as a forward-compatibility
// slot for envp pass-through (a future sub-chunk can wire envp_data_va +
// envp_data_len without breaking the ABI by reusing this slot).
//
// Wire fields (offsets pinned by _Static_assert below):
//   name_va         u64  — user-VA of the binary name (no NUL required)
//   argv_data_va    u64  — user-VA of the argv buffer (concatenated NUL-
//                          terminated strings; argc strings; total bytes
//                          == argv_data_len; the final byte MUST be NUL)
//   fd_list_va      u64  — user-VA of fd_count u32 fd indices to inherit
//   name_len        u32  — 1..SYS_SPAWN_NAME_MAX
//   argv_data_len   u32  — 0..SYS_SPAWN_ARGV_DATA_MAX; 0 iff argc == 0
//   argc            u32  — 0..SYS_SPAWN_ARGV_MAX; NUL count in argv_data
//                          MUST equal argc
//   fd_count        u32  — 0..SYS_SPAWN_MAX_FDS
//   perm_flags      u32  — SPAWN_PERM_* bits; bits outside SPAWN_PERM_ALL
//                          are rejected
//   _pad_envp       u32  — must be 0 at v1.0; reserved for envp_data_len
//                          when envp pass-through lands
//   cap_mask        u64  — caps_t bits; AND'd with parent caps by
//                          rfork_with_caps (I-2 monotonic reduction holds
//                          structurally)
struct sys_spawn_args {
    u64 name_va;
    u64 argv_data_va;
    u64 fd_list_va;
    u32 name_len;
    u32 argv_data_len;
    u32 argc;
    u32 fd_count;
    u32 perm_flags;
    u32 _pad_envp;
    u64 cap_mask;
    // A-1a identity extension (append-only; 56 -> 80). Honored only when
    // identity_flags & SPAWN_IDENTITY_SET; gated FAIL-CLOSED on the
    // caller holding CAP_SET_IDENTITY (else the syscall returns -1).
    u32 principal_id;   // [1, 0xFFFFFFFD] or PRINCIPAL_NONE; INVALID/SYSTEM → -1
    u32 primary_gid;    // [1, 0xFFFFFFFD] or GID_NONE; INVALID/SYSTEM → -1
    u64 supp_gids_va;   // user-VA of supp_gid_count u32 gids (0 → none)
    u32 supp_gid_count; // 0..PROC_SUPP_GIDS_MAX (15)
    u32 identity_flags; // SPAWN_IDENTITY_* bits; outside SPAWN_IDENTITY_FLAGS_ALL → -1
    // Menagerie build-arc step 5 (I-34; append-only, 80 -> 96). Honored only
    // when allowance_flags & SPAWN_ALLOWANCE_SET; the child is conferred the
    // narrowed allowance at allowance_va before it enters EL0. Gated as a
    // NARROWING vs the caller's own allowance (no cap needed). Clear (0) ->
    // the child inherits the parent's allowance (every existing caller, who
    // zero-fills this struct, gets the as-built broad behavior).
    u64 allowance_va;    // user-VA of a struct t_allowance_desc (0 unless SET)
    u32 allowance_flags; // SPAWN_ALLOWANCE_* bits; outside SPAWN_ALLOWANCE_FLAGS_ALL → -1
    u32 _pad_allow;      // must be 0 at v1.0 (8-alignment + forward-compat slot)
};

_Static_assert(sizeof(struct sys_spawn_args) == 96,
               "struct sys_spawn_args is a SYS_SPAWN_FULL_ARGV ABI type "
               "— pinned at 96 bytes (A-1a appended the identity block at "
               "56..80; the Menagerie step-5 allowance block appended at "
               "80..96: allowance_va 8 + allowance_flags 4 + _pad_allow 4); "
               "no implicit padding");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, name_va) == 0,
               "sys_spawn_args.name_va at ABI offset 0");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, argv_data_va) == 8,
               "sys_spawn_args.argv_data_va at ABI offset 8");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, fd_list_va) == 16,
               "sys_spawn_args.fd_list_va at ABI offset 16");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, name_len) == 24,
               "sys_spawn_args.name_len at ABI offset 24");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, argv_data_len) == 28,
               "sys_spawn_args.argv_data_len at ABI offset 28");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, argc) == 32,
               "sys_spawn_args.argc at ABI offset 32");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, fd_count) == 36,
               "sys_spawn_args.fd_count at ABI offset 36");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, perm_flags) == 40,
               "sys_spawn_args.perm_flags at ABI offset 40");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, _pad_envp) == 44,
               "sys_spawn_args._pad_envp at ABI offset 44");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, cap_mask) == 48,
               "sys_spawn_args.cap_mask at ABI offset 48");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, principal_id) == 56,
               "sys_spawn_args.principal_id at ABI offset 56");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, primary_gid) == 60,
               "sys_spawn_args.primary_gid at ABI offset 60");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, supp_gids_va) == 64,
               "sys_spawn_args.supp_gids_va at ABI offset 64");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, supp_gid_count) == 72,
               "sys_spawn_args.supp_gid_count at ABI offset 72");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, identity_flags) == 76,
               "sys_spawn_args.identity_flags at ABI offset 76");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, allowance_va) == 80,
               "sys_spawn_args.allowance_va at ABI offset 80");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, allowance_flags) == 88,
               "sys_spawn_args.allowance_flags at ABI offset 88");
_Static_assert(__builtin_offsetof(struct sys_spawn_args, _pad_allow) == 92,
               "sys_spawn_args._pad_allow at ABI offset 92");

struct exception_context;

// Dispatch entry called from arch/arm64/exception.c::exception_sync_lower_el
// on EC_SVC_AARCH64. Reads nr from ctx->regs[8] and args from
// ctx->regs[0..5]. Writes the result to ctx->regs[0] (clobbered for
// SYS_EXITS which never returns).
//
// SYS_EXITS doesn't return — it transitions through the kernel's exits()
// path and sched()'s context-switch picks another thread. The user
// thread is left in EXITING state until wait_pid reaps it.
void syscall_dispatch(struct exception_context *ctx);

#endif // THYLACINE_SYSCALL_H
