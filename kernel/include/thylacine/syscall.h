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
    //   - blob exceeds SYS_SPAWN_BLOB_MAX (1 MiB)
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

    // P5-hostowner-b-b: SYS_CAP_GRANT(cap_mask, target_stripes) and
    // SYS_CAP_USE(cap_mask) — userspace bridges to the kernel `cap` device
    // (CORVUS-DESIGN.md §5.5.1). The Dev write op (devcap_write on
    // /cap/grant or /cap/use) is the eventual production path through a
    // future namespace-aware open syscall; at v1.0 we lack t_open, so the
    // two writers (corvus → grant, the console-attached redeemer → use)
    // reach the cores directly via these syscalls. Same gate semantics
    // as the Dev op (CAP_GRANT_HOSTOWNER for grant; PROC_FLAG_CONSOLE_-
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
    // The returned handle has RIGHT_READ | RIGHT_WRITE | RIGHT_TRANSFER —
    // matching SYS_ATTACH_9P's envelope. The underlying fid's omode is
    // what the server actually enforces; a SYS_WRITE on an OREAD-only fid
    // gets -1 from the server's Rlerror, not from a rights gate here.
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
    // SYS_THREAD_SPAWN(entry_va, sp_va, arg, tls_va) → tid / -errno
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
    //            sp_va / out-of-bound tls_va / caller is kproc
    //   -ENOMEM  kstack alloc fail / Thread cache alloc fail
    //
    // The new Thread inherits no user state from the caller — the entry
    // function is the responsible adult. fd inheritance is implicit
    // (single handle table per Proc); register state is the four parked
    // args + zeros.
    SYS_THREAD_SPAWN = 41,   // arg: entry_va, sp_va, arg, tls_va

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
    //      parent's child_done (mirrors exits() with status 0).
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

    // SYS_POST_SERVICE_BYTE — register a /srv service in RAW BYTE MODE.
    // P6-pouch-sockets (sub-chunk 12). Identical wire shape to
    // SYS_POST_SERVICE (name_va, name_len → KObj_Srv listener handle)
    // but the service entry's transport mode is SRV_MODE_BYTE instead
    // of SRV_MODE_9P. Effects propagate to every connection minted
    // against this listener:
    //   - SYS_srv_connect SKIPS the 9P handshake (Tversion + Tattach +
    //     Tlopen) and REFUSES a non-empty path. The kernel returns a
    //     KObj_Srv handle ready for raw byte I/O the moment the
    //     SrvConn is enqueued on the accept backlog.
    //   - sys_read/write_for_proc's KObj_SRV arm routes through
    //     srvconn_client_send/recv (raw c2s/s2c) instead of
    //     srvconn_client_read/write (9P Tread/Twrite).
    //
    // Why a separate syscall instead of an `int mode` arg on
    // SYS_POST_SERVICE: the existing svc dispatcher reads x0/x1 only.
    // Adding x2 for mode would interpret uninitialized x2 (corvus's
    // wrapper passes nothing) as garbage mode — a latent ABI hazard.
    // A new syscall number is the safer extension.
    //
    // Same post-gate as SYS_POST_SERVICE: PROC_FLAG_MAY_POST_SERVICE
    // is required.
    //
    //   arg: name_va (x0), name_len (x1)
    //   returns: KObj_Srv listener handle (>=0) on success, -1 on
    //            gate fail / bad name / duplicate / OOM / registry full
    SYS_POST_SERVICE_BYTE = 43,

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
    //   - devramfs binary not found OR oversize (> SYS_SPAWN_BLOB_MAX)
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
    //   x0 = srv_fd      (hidx_t; must be a byte-mode KOBJ_SRV handle the
    //                    caller holds, with RIGHT_READ + RIGHT_WRITE -- the
    //                    kernel 9P client writes Twalk/Tread/Twrite and
    //                    reads Rwalk/Rread/Rwrite over the rings)
    //   x1 = aname_va    (user-VA pointer to the attach name string;
    //                    NUL not required -- aname_len authoritative)
    //   x2 = aname_len   (bytes; <= SYS_ATTACH_ANAME_MAX = 256;
    //                    zero-length aname is permitted)
    //   x3 = n_uname     (u32; 0 for no-auth attach at v1.0)
    //
    // Parallel to SYS_ATTACH_9P but the transport is a byte-mode
    // SrvConn (from pouch sockets' SYS_POST_SERVICE_BYTE + the client's
    // SYS_SRV_CONNECT) rather than a Spoor pair. The composition:
    //   1. handle_get(srv_fd) -> KObj_Srv slot
    //   2. atomic-ACQUIRE on cn->byte_mode; reject if 9P-mode (the
    //      embedded p9_client owns the rings -- a second p9_client
    //      would race / produce wire corruption)
    //   3. kmalloc + p9_srvconn_transport_init (takes 1 srvconn_ref)
    //   4. p9_attached_create -> drives Tversion + Tattach on the
    //      kernel-owned p9_client wrapped by the adapter
    //   5. p9_attached_root_spoor -> dev9p Spoor pointing at the bound
    //      root_fid
    //   6. handle_alloc KOBJ_SPOOR with RIGHT_READ | WRITE | TRANSFER
    //      (same rights envelope as SYS_ATTACH_9P)
    //
    // The returned KOBJ_SPOOR fd can be passed to SYS_MOUNT to graft
    // the resulting 9P tree at a target path in the caller's territory.
    //
    // Lifetime: caller's KOBJ_SRV handle reference is INDEPENDENT of
    // the adapter's reference. Userspace closing the source srv_fd
    // does NOT tear down the SrvConn while the returned KOBJ_SPOOR is
    // still alive (the p9_attached holds the adapter holds its own
    // srvconn_ref). The SrvConn lives until ALL holders unref --
    // matches the SYS_ATTACH_9P discipline for transport-Spoor refs
    // (p9_attached_install_transport).
    //
    // Returns: x0 = new fd (>=0) on success; -1 on:
    //   - invalid srv_fd (not KOBJ_SRV / out-of-range / corrupted)
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
    // the RX ring. No args. Returns the fd (>= 0) or -1. v1.0 ungates the open
    // (devcons is single-reader-guarded: a 2nd concurrent read returns -1); a
    // console-open capability gate is a v1.x seam if untrusted Procs ever run.
    SYS_CONSOLE_OPEN = 64,
};

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
#define SPAWN_PERM_ALL               (SPAWN_PERM_MAY_POST_SERVICE | \
                                      SPAWN_PERM_CONSOLE_TRUSTED)

// A-1a (docs/IDENTITY-DESIGN.md §9.1): sys_spawn_args.identity_flags bits.
// SPAWN_IDENTITY_SET requests that the child be born with the principal_id
// / primary_gid / supp_gids carried in the struct (instead of inheriting
// the parent's identity). FAIL-CLOSED gate: a SET request from a caller
// lacking CAP_SET_IDENTITY is rejected with -1 (never silently inherited).
// Clear -> the child inherits (the default; no cap needed).
#define SPAWN_IDENTITY_SET           (1u << 0)
#define SPAWN_IDENTITY_FLAGS_ALL     (SPAWN_IDENTITY_SET)

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

// Maximum binary name length for SYS_SPAWN. Names are devramfs entries
// ("hello", "stratumd-stub", "corvus" — all short). 64 bytes is more
// than enough; bounds the kernel-stack scratch buffer at a small cap.
#define SYS_SPAWN_NAME_MAX  64u

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

// Maximum argc for SYS_SPAWN_FULL_ARGV (P6-pouch-stratumd-boot sub-chunk
// 16b-alpha). 16 is well above the v1.0 callers' needs: stratumd's full
// CLI shape is ~8 args (binary-path + --keyfile + path + --listen + path
// + optional --read-only + optional --bind-pool-serial + hex). Bounds the
// kernel argv-pointer array sizing in exec_build_init_stack.
#define SYS_SPAWN_ARGV_MAX  16u

// Maximum total bytes of the argv buffer for SYS_SPAWN_FULL_ARGV (the
// concatenated NUL-terminated strings). One page is generous for the
// v1.0 callers (stratumd's longest CLI is well under 200 bytes). Bounds
// the kernel-side kmalloc + the user-stack region that holds the strings.
// At the maximum (4 KiB strings + 16 argv pointers * 8 + 152 fixed-frame
// bytes), the System V frame fits comfortably under EXEC_USER_STACK_SIZE
// (256 KiB).
#define SYS_SPAWN_ARGV_DATA_MAX  4096u

// Maximum single-component name length for SYS_WALK_OPEN. Matches the
// Plan 9 + 9P2000.L practical cap for a single path element; bounds the
// kernel-stack scratch + the per-byte uaccess loop. The wire codec's
// per-name cap is larger; we choose the smaller bound here because v1.0
// callers (joey, the bringup probes) all walk short server-stamped names.
#define SYS_WALK_OPEN_NAME_MAX  64u

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
// rwxrwxrwx mode; the DMDIR bit selects directory creation. All other DM*
// bits (DMAPPEND 0x40000000, DMEXCL 0x20000000, DMTMP 0x04000000, ...) are
// reserved 0 at v1.0 -- a perm with any bit outside SYS_WALK_CREATE_PERM_VALID
// is rejected with -1 (so a future bit cannot be silently ignored).
#define SYS_WALK_CREATE_DMDIR       0x80000000u
#define SYS_WALK_CREATE_PERM_VALID  (0x1FFu | SYS_WALK_CREATE_DMDIR)

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
};

_Static_assert(sizeof(struct sys_spawn_args) == 80,
               "struct sys_spawn_args is a SYS_SPAWN_FULL_ARGV ABI type "
               "— pinned at 80 bytes (A-1a appended the identity block: "
               "principal_id 4 + primary_gid 4 + supp_gids_va 8 + "
               "supp_gid_count 4 + identity_flags 4); no implicit padding");
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
