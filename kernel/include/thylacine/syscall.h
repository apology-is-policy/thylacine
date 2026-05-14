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
};

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

// Maximum ELF blob size for SYS_SPAWN. v1.0 userspace binaries fit
// comfortably under 32 KiB (joey: 12744, hello: 12752, pipe-probe:
// 16760, attach-probe: 15096). Bump if needed; kmalloc routes >2 KiB
// requests through alloc_pages so larger sizes are merely heavier.
#define SYS_SPAWN_BLOB_MAX  32768u

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
