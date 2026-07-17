// The pts registry (PTY-1c) -- the kernel-side pts identity that makes the
// PTY signal seam realizable (PTY-DESIGN.md section 3, round-1 F1/F2 +
// round-2 R2-F1/R2-F4).
//
// The design names this "KObj_Pts". The realization is a gen-stamped kernel
// REGISTRY (the Weft share_id shape, kernel/weft.c), NOT a handle-table
// kobj kind: no userspace-holdable pts handle exists, so there is nothing
// to dup / transfer / leak -- the pts is named EITHER by a `pts_id` the
// kernel returned to the registered server (ptyfs) OR by resolving a
// slave/master fd the caller already holds (the grant-is-the-share
// correlation). This is the recorded ABI narrowing, flagged at signoff.
//
// The correlation model (server-mediated, at both ends -- R2-F1):
//
//   ptyfs (server)                          the kernel
//   --------------                          ----------
//   serves a /dev/ptmx open                 SYS_PTY_REGISTER(MINT,
//     -> mints pty pair N,                    conn_fd, master_qid)
//        rebinds the fid onto                 -> pts_mint: stamp a registry
//        the master qid                          entry { server_pid,
//                                                 (conn, master_qid) },
//                                                 return pts_id (gen|idx)
//   serves a /dev/pts/<n> open              SYS_PTY_REGISTER(SLAVE,
//     -> replies Rlopen with the              conn_fd, slave_qid, pts_id)
//        slave qid                            -> pts_bind_slave: add the
//                                                (conn, slave_qid) binding
//
//   a slave-fd holder (the shell)           pts_resolve_spoor(fd's Spoor):
//     calls SYS_TTY_ACQUIRE /                 fd -> dev9p client -> the
//     SET_FG / GET_FG (PTY-1d)                underlying SrvConn (transport
//                                             downcast) + the Spoor's qid
//                                             -> registry match -> pts_id
//
// Both endpoints of one connection share ONE `struct SrvConn` (the server's
// accepted conn Spoor and the client mount's dev9p transport wrap the same
// object), so pointer identity is the correlation key. Each binding HOLDS a
// srvconn_ref, so a registered pointer can never be freed-and-reused while
// bound (no ABA); the resolve only ever POINTER-COMPARES a candidate against
// ref-held bindings -- it never dereferences it -- so a non-matching or
// stale candidate fails closed.
//
// Authority (R2-F4): minting is gated at the syscall on the caller being a
// SERVER-endpoint conn-Spoor holder + carrying PROC_FLAG_MAY_POST_SERVICE
// (the trusted-service tier -- the Weft-7 F1 registry-squat lesson; only a
// flag holder can post a service and therefore be a 9P server at all, so
// the check is defense-in-depth). SLAVE binds and FREE are gated on the
// MINTING server's pid (u32, monotonically allocated, never reused) -- a
// second server, or a restarted ptyfs under a new pid, cannot mutate
// another instance's entries; reclaim after a server death is the torn-conn
// lazy GC at mint (a dead server's conns are TORN by its handle-close
// teardown, #841's server-endpoint-always-tears-down).
//
// The gen (round-1 F11 / SA-8): every returned pts_id carries the slot's
// generation; free bumps it before the index is reusable, so a stale id --
// held across a free + re-mint -- fails every later lookup instead of
// mis-routing to the new occupant (the netd slot-gen / net-3d F1 lesson).
//
// Controlling-terminal state (ct_sid / fg_pgid) lives ON the entry --
// kernel state, never server-held (the F1 seam) -- zeroed at mint; PTY-1d's
// acquisition / tcsetpgrp syscalls are the only mutators.
//
// Locking: one leaf `g_pts_lock` spinlock covers the whole registry.
// srvconn_ref (atomic) is taken under it; srvconn_unref is NEVER called
// under it (the last unref tears down + frees, which takes chan/slab locks
// -- the #847 leaf discipline): drops are staged and run after release.
// srvconn_is_live (reads cn->state, a leaf) nests under g_pts_lock; nothing
// in srvconn/devsrv takes g_pts_lock, so the order is acyclic.

#ifndef THYLACINE_PTS_H
#define THYLACINE_PTS_H

#include <thylacine/types.h>

struct Proc;
struct SrvConn;
struct Spoor;

// Registry bounds. PTS_MAX matches WEFT_MAX_SHARES (64 concurrent ptys is
// ample for v1.0 -- tmux-scale); PTS_BINDINGS_MAX covers the master + the
// slave binding on the v1.0 shared mount, with slack for per-user mounts
// (a slave open served on a second connection binds a second row).
#define PTS_MAX          64
#define PTS_BINDINGS_MAX 4

// A pts_id encodes (gen << 16 | idx); gen is never 0, so a valid id is
// always > 0 and the error space (negative -T_E_*) never collides.
#define PTS_IDX_BITS     16

// Mint a pts: record the master-side (cn, master_qid) binding + the minting
// server, return the gen-stamped pts_id (> 0). Fails: -T_E_INVAL (bad args;
// master_qid 0 is reserved -- it is the dev9p attach-root qid), -T_E_EXIST
// ((cn, master_qid) already bound to a live pts), -T_E_AGAIN (registry full
// and no torn-conn entry to reclaim). Takes a srvconn_ref on `cn` for the
// binding's lifetime.
s64 pts_mint(struct Proc *server, struct SrvConn *cn, u64 master_qid);

// Bind a slave-side (cn, slave_qid) to an existing pts. Idempotent for an
// identical re-bind (the same slave re-opened on the same connection has
// the same qid). Fails: -T_E_INVAL (bad args / stale or malformed pts_id),
// -T_E_ACCES (caller is not the minting server), -T_E_EXIST ((cn, qid)
// bound to a DIFFERENT live pts -- a qid resolves to at most one pts),
// -T_E_NOMEM (the entry's binding rows are full).
int pts_bind_slave(struct Proc *server, struct SrvConn *cn, u64 slave_qid,
                   u64 pts_id);

// Free a pts: drop every binding's srvconn_ref, bump the slot gen (stale
// ids fail from here on), clear the entry. Fails: -T_E_INVAL (stale or
// malformed id), -T_E_ACCES (not the minting server).
int pts_free(struct Proc *server, u64 pts_id);

// Resolve a (connection, qid) to its live pts_id, or -T_E_NOENT. Pure
// registry scan -- `cn` is only pointer-compared against ref-held bindings,
// never dereferenced, so any candidate pointer is safe. `is_master_out`
// (optional) reports which side matched: PTY-1d's acquisition requires a
// SLAVE binding (POSIX acquisition happens at slave open), while tcgetpgrp
// is legal from either end.
s64 pts_resolve_conn_qid(struct SrvConn *cn, u64 qid, bool *is_master_out);

// Resolve an fd's Spoor to its pts: dev9p Spoor -> the kernel 9P client ->
// the underlying SrvConn (srvconn-transport downcast; NULL for loopback /
// spoor-backed clients, which can never carry a pts) -> the (cn, qid)
// lookup above. -T_E_INVAL for a non-dev9p Spoor; -T_E_NOENT for no match.
s64 pts_resolve_spoor(struct Spoor *sp, bool *is_master_out);

#endif // THYLACINE_PTS_H
