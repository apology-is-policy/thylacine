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
    // SYS_MOUNT(source_spoor_fd, target_path_id, flags) → 0/-1
    //   x0 = source_spoor_fd (hidx_t; must be a KOBJ_SPOOR handle)
    //   x1 = target_path_id (u32; abstract path token at v1.0 — the
    //        same numeric ID used by bind/unbind in the existing
    //        PgrpBind / PgrpMount C-API. String-path resolution lands
    //        with the fd-syscall walk subsystem in a later chunk.)
    //   x2 = flags (u32; MREPL / MBEFORE / MAFTER / MCREATE)
    // Returns: 0 on success, -1 on:
    //   - invalid source_spoor_fd (not KOBJ_SPOOR, out-of-range)
    //   - missing RIGHT_READ (the mount holder needs to be able to
    //     consume the source's tree — without READ, a mount has no
    //     value at v1.0; this is a defense-in-depth check, not a
    //     deep correctness requirement)
    //   - flags has bits outside the MREPL|MBEFORE|MAFTER|MCREATE set
    //   - territory mount table full (PGRP_MAX_MOUNTS reached)
    //
    // Lifecycle (per ARCH §9.6.6): `mount` bumps the Spoor's refcount
    // (the mount-table entry holds its own ref). The caller can close
    // their source_spoor_fd afterward; the mount table keeps the Spoor
    // alive. unmount() (or Territory destruction) drops the per-entry
    // ref; if it was the last ref, the Spoor's Dev close runs (which,
    // for dev9p-backed Spoors set up by SYS_ATTACH_9P, tears down the
    // entire 9P session).
    SYS_MOUNT       = 14,   // arg: source_spoor_fd, target_path_id, flags

    // SYS_UNMOUNT(target_path_id) → 0/-1
    //   x0 = target_path_id (u32; same abstract token as SYS_MOUNT)
    // Returns: 0 on success, -1 on:
    //   - no entry at target_path_id in the caller's Territory
    //
    // Drops the per-entry Spoor ref; the Spoor's Dev close runs if
    // this was the last ref.
    SYS_UNMOUNT     = 15,   // arg: target_path_id

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
    //   - binary not found in devramfs
    //   - blob exceeds SYS_SPAWN_BLOB_MAX (32 KiB)
    //   - kmalloc / rfork OOM
    //   - exec_setup failure (child exits "fail-exec" → parent's
    //     SYS_WAIT_PID observes non-zero status)
    // The child inherits no capabilities (CAP_ALL & 0u = 0). v1.0 model
    // is "the child fully describes its needs"; future SYS_RFORK with a
    // cap-mask argument lands at P5-spawn-caps when needed.
    SYS_SPAWN        = 21,   // arg: name_va (x0), name_len (x1)

    // SYS_WAIT_PID(status_out_va) → reaped_pid / -1
    //   x0 = status_out_va (user-VA; 4-byte int destination, or 0 to
    //                       skip the write)
    // Block until a child Proc enters ZOMBIE, then reap. Writes the
    // child's exit_status to *status_out_va if non-zero. Returns the
    // reaped PID. Returns -1 immediately if the caller has no children.
    // Mirrors kernel/proc.c::wait_pid (Plan 9 wait(2) shape; no waitpid
    // selector at v1.0).
    SYS_WAIT_PID     = 22,   // arg: status_out_va (x0)

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

    // P5-corvus-srv-impl-a2: SYS_POST_SERVICE(name_va, name_len) →
    // service_handle / -1
    //   x0 = name_va    user-VA pointer to the service-name bytes
    //   x1 = name_len   bytes; 1..SRV_NAME_MAX (32; <thylacine/devsrv.h>)
    // Register the calling Proc as the 9P server for /srv/<name> in the
    // kernel service registry (CORVUS-DESIGN.md §6.1; ARCH §9.4 / §11.2c)
    // and return a KObj_Srv service handle. The kernel creates and owns
    // all transport (invariant C-23). corvus calls SYS_POST_SERVICE
    // ("corvus") at startup.
    //
    // Gated on the one-way joey-stamped PROC_FLAG_MAY_POST_SERVICE bit:
    // an unmarked Proc cannot post or hijack a name. A name with a live
    // server is not displaceable; a TOMBSTONED name (its prior poster
    // exited) is re-postable only by a marked Proc.
    //
    // Returns the service handle (hidx ≥ 0) on success, -1 on:
    //   - caller lacks PROC_FLAG_MAY_POST_SERVICE
    //   - name_len out of range / name_va bound violation
    //   - name contains a non-identifier byte (allowed: 0x21..0x7e, no '/')
    //   - the name already has a live or in-flight server
    //   - the service registry is full / handle table full
    SYS_POST_SERVICE = 26,   // arg: name_va (x0), name_len (x1)

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

    // P5-corvus-srv-impl-b2: SYS_SRV_CONNECT(name_va, name_len, path_va,
    //                                        path_len) → KObj_Srv handle
    //   x0 = name_va        user-VA pointer to the service name bytes
    //                       (e.g. "corvus")
    //   x1 = name_len       1..SRV_NAME_MAX = 31
    //   x2 = path_va        user-VA pointer to the path bytes within the
    //                       service's 9P namespace (e.g. "ctl"). May be 0
    //                       (with path_len == 0) to leave the kernel-side
    //                       open at the 9P root fid (Tversion + Tattach
    //                       only, no Twalk).
    //   x3 = path_len       0..SRVCONN_PATH_MAX = 64
    //
    // The client-open path for the `/srv` mechanism (CORVUS-DESIGN.md §6.2).
    // Composes the existing srv_conn_open_for_proc (which mints the SrvConn,
    // enqueues it on the service's accept backlog, and installs a non-
    // transferable KObj_Srv handle in the caller's table) with the new
    // srvconn_drive_client_handshake (Tversion + Tattach + optional Twalk
    // + Tlopen on the SrvConn's kernel-owned p9_client). On success, the
    // returned fd has the open `client_fid` ready for SYS_READ / SYS_WRITE
    // (the kernel translates those to Tread / Twrite at that fid).
    //
    // Per-Proc cap: a Proc may hold at most one /srv client connection at
    // v1.0 (CORVUS-DESIGN.md §6.2 — "One connection per Proc"). A second
    // SYS_SRV_CONNECT from a Proc that still holds one returns -1.
    //
    // Returns -1 on:
    //   - bad name_len / path_len bounds
    //   - the caller already holds a /srv client connection
    //   - global SRV_MAX_CONNS cap reached
    //   - the named service is not LIVE (missing / RESERVING / TOMBSTONED)
    //   - accept-backlog full
    //   - handle-table full
    //   - 9P handshake failure (server crashed / hung / Rlerror)
    SYS_SRV_CONNECT  = 30,   // arg: name_va, name_len, path_va, path_len

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
};

// SYS_SPAWN_WITH_PERMS perm_flags bits — must mirror the libt / libthyla-rs
// constants. New bits add atomically without an ABI break (a Proc spawned
// by a parent that did not know the new bit behaves identically to today).
//
// SPAWN_PERM_MAY_POST_SERVICE stamps PROC_FLAG_MAY_POST_SERVICE on the
// spawned child (CORVUS-DESIGN.md §6.1). joey grants this to /sbin/corvus
// so corvus may call SYS_POST_SERVICE("corvus") and become the /srv/corvus
// 9P server. NOT a cap because rfork does not propagate it.
#define SPAWN_PERM_MAY_POST_SERVICE  (1u << 0)
#define SPAWN_PERM_ALL               (SPAWN_PERM_MAY_POST_SERVICE)

// SYS_SRV_PEER result — the kernel-stamped peer identity of a /srv
// connection (CORVUS-DESIGN.md §6.3). The kernel writes one of these to
// the SYS_SRV_PEER out_va buffer. A syscall-ABI type: the layout is
// pinned by the _Static_asserts so a userspace consumer (corvus, at
// P5-corvus-srv-impl-b) decodes a fixed 24-byte record.
struct srv_peer_info {
    u64 stripes;     // peer Proc's identity tag (0 → unidentifiable peer)
    u64 caps;        // peer's live capability set; 0 when alive == 0
    u32 console;     // 1 iff the peer is console-attached, else 0
    u32 alive;       // 1 iff an ALIVE Proc still carries `stripes`, else 0
};

_Static_assert(sizeof(struct srv_peer_info) == 24,
               "struct srv_peer_info is a SYS_SRV_PEER ABI type — pinned "
               "at 24 bytes (u64 stripes + u64 caps + u32 console + u32 "
               "alive), naturally aligned with no implicit padding");
_Static_assert(__builtin_offsetof(struct srv_peer_info, stripes) == 0,
               "srv_peer_info.stripes at ABI offset 0");
_Static_assert(__builtin_offsetof(struct srv_peer_info, caps) == 8,
               "srv_peer_info.caps at ABI offset 8");
_Static_assert(__builtin_offsetof(struct srv_peer_info, console) == 16,
               "srv_peer_info.console at ABI offset 16");
_Static_assert(__builtin_offsetof(struct srv_peer_info, alive) == 20,
               "srv_peer_info.alive at ABI offset 20");

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

// Maximum binary name length for SYS_SPAWN. Names are devramfs entries
// ("hello", "stratumd-stub", "corvus" — all short). 64 bytes is more
// than enough; bounds the kernel-stack scratch buffer at a small cap.
#define SYS_SPAWN_NAME_MAX  64u

// Maximum ELF blob size for SYS_SPAWN. v1.0 userspace binaries that
// stay near the cap: corvus = 111440 bytes at P5-corvus-bringup-c
// (Argon2id + AEGIS-256 + heap allocator pull in ~100 KiB of compiled
// crypto). Future daemons embedding hybrid PQC primitives (ML-KEM-768)
// will continue growing; 256 KiB gives several chunks of headroom.
// kmalloc routes >2 KiB requests through alloc_pages so larger sizes
// just allocate more pages — no algorithmic cost.
#define SYS_SPAWN_BLOB_MAX  262144u

// Maximum number of fds that can be inherited via SYS_SPAWN_WITH_FDS
// per call. 16 is generous for the v1.0 use case (joey passes 2 to
// stratumd-stub; future per-user stratumd takes 2-3). Bounds the
// kernel-stack scratch for the fd-list copy.
#define SYS_SPAWN_MAX_FDS   16u

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
