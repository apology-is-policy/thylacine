// devsrv — the /srv service registry Dev (P5-corvus-srv-impl-a2).
//
// Per ARCHITECTURE.md §9.4 + §11.2c + CORVUS-DESIGN.md §6.1. `/srv` is a
// kernel Dev — `devsrv`, Plan 9's `#s` heritage — deliberately distinct
// from `dev9p` (the Dev backing kernel-mounted 9P trees). The separation
// is structural: every Spoor walked out of `/srv` is a KObj_Srv kernel
// object (non-transferable; ARCH §18.2, specs/handles.tla SrvKObjs), so a
// connection's kernel-stamped peer identity (§6.3) cannot be forged by
// transferring the handle.
//
// A userspace 9P server registers a name with SYS_POST_SERVICE; the
// kernel mediates per-connection client access. `corvus` (CORVUS-DESIGN
// §6) is the v1.0 consumer.
//
// At P5-corvus-srv-impl-a2 (this chunk):
//   - The service registry: a fixed-size table of named services, each
//     keyed to its poster Proc by `stripes` (CORVUS-DESIGN.md §6.3 — by
//     value, never a raw Proc*, so a poster's exit is never a UAF).
//   - SYS_POST_SERVICE registers the calling Proc, gated on the one-way
//     joey-stamped PROC_FLAG_MAY_POST_SERVICE bit.
//   - On poster exit the kernel TOMBSTONES the name (srv_proc_exit_notify
//     from exits()): the name stays reserved, re-postable only by a
//     joey-marked Proc — so a malicious Proc cannot race corvus's restart
//     to claim /srv/corvus.
//   - The devsrv Dev itself attaches a /srv root directory Spoor.
//
// Deferred to P5-corvus-srv-impl-a3: the devsrv walk op (a walk of /srv/
// <name> mints a per-connection KObj_Srv Spoor — specs/handles.tla
// WalkDerive), the transport pair, SYS_SRV_ACCEPT, SYS_SRV_PEER.
//
// Spec: specs/corvus.tla — MarkMayPost / PostService / ServiceTombstone,
// invariant ServicePosterEverMarked.

#ifndef THYLACINE_DEVSRV_H
#define THYLACINE_DEVSRV_H

#include <thylacine/types.h>

struct Dev;
struct Proc;

// Maximum service-name length. Service names are short identifiers
// ("corvus"); 32 is generous and bounds the registry entry + the
// SYS_POST_SERVICE kernel-stack name scratch.
#define SRV_NAME_MAX  32u

// Service registry capacity. corvus is the sole v1.0 service (ARCH §9.4);
// 8 is headroom. A post past the cap fails fast.
#define SRV_MAX_SERVICES  8u

// SRV_SERVICE_MAGIC — sentinel at offset 0 of struct SrvService. Lets the
// KObj_Srv handle-release path (P5-corvus-srv-impl-a3, when connection
// objects also become KObj_Srv) discriminate a service object from a
// connection object by the magic word.
#define SRV_SERVICE_MAGIC  0x53525653564300ULL    // 'SRVSVC' || 0x00

// Service-entry lifecycle.
//
// FREE       — slot unused. Zero-initialized storage reads as FREE.
// RESERVING  — a SYS_POST_SERVICE is in flight: the slot + name are
//              claimed but not yet committed (the handle has not been
//              allocated). Transient — bounded by one syscall; never
//              observed as a connectable service.
// LIVE       — posted; the poster Proc is alive and is the 9P server.
// TOMBSTONED — the poster exited. The name stays reserved (not freed):
//              only a joey-marked Proc may rebind it via SYS_POST_SERVICE.
enum srv_state {
    SRV_STATE_FREE       = 0,
    SRV_STATE_RESERVING  = 1,
    SRV_STATE_LIVE       = 2,
    SRV_STATE_TOMBSTONED = 3,
};

// A registered (or tombstoned) service. The poster identity is captured
// BY VALUE (`poster_stripes`, `poster_pid`) — CORVUS-DESIGN.md §6.3: the
// registry holds no raw Proc*, so a poster Proc that exits and is reaped
// never turns a registry read into a use-after-free.
struct SrvService {
    u64            magic;            // SRV_SERVICE_MAGIC
    enum srv_state state;
    u8             name_len;         // 1..SRV_NAME_MAX; bytes valid in name[]
    char           name[SRV_NAME_MAX];
    u64            poster_stripes;   // poster Proc's stripes tag (by value)
    int            poster_pid;       // poster Proc's PID (by value; diagnostics)
};

// The devsrv Dev. dc='s' (Plan 9 #s); registered by dev_init().
extern struct Dev devsrv;

// =============================================================================
// Service registry.
// =============================================================================
//
// SYS_POST_SERVICE is a reserve-then-commit two-phase post so a failing
// handle_alloc rolls back cleanly (the entry is never observably LIVE
// until the handle exists):
//
//   srv_reserve()  — claim a slot + name; the entry goes RESERVING.
//   srv_commit()   — RESERVING -> LIVE (handle allocated; post succeeded).
//   srv_abort()    — RESERVING -> prior (handle_alloc failed; rolled back).

// srv_reserve — phase 1. Claim a registry slot for `name` posted by
// `poster`. Validates name length; takes the registry lock internally.
//
// Returns 0 on success: *svc_out is the reserved entry (state RESERVING),
// *prior_out is the state to restore on abort (FREE for a fresh post,
// TOMBSTONED for a rebind of a tombstoned name).
//
// Returns -1 on: NULL/empty/oversized name; the name already has a LIVE
// or RESERVING entry (no displacing a running or in-flight server); the
// registry is full. On -1, *svc_out / *prior_out are untouched.
//
// The caller must have already passed the SYS_POST_SERVICE post-gate
// (proc_may_post_service) — srv_reserve does NOT re-check it.
int srv_reserve(const char *name, u8 name_len, struct Proc *poster,
                struct SrvService **svc_out, enum srv_state *prior_out);

// srv_commit — phase 2 (success). RESERVING -> LIVE. Takes the registry
// lock. `svc` must be a reservation returned by srv_reserve.
void srv_commit(struct SrvService *svc);

// srv_abort — phase 2 (failure). RESERVING -> `prior` (the value
// srv_reserve returned in *prior_out). Takes the registry lock.
void srv_abort(struct SrvService *svc, enum srv_state prior);

// srv_lookup — find a service by name. Returns the entry for any
// non-FREE state (LIVE / RESERVING / TOMBSTONED) — the caller inspects
// `state`. Returns NULL if no entry matches. Takes the registry lock.
//
// P5-corvus-srv-impl-a3's devsrv walk uses this; a2 exercises it in tests.
struct SrvService *srv_lookup(const char *name, u8 name_len);

// srv_proc_exit_notify — called from exits() for every exiting Proc. Any
// LIVE service whose poster is `p` (matched by stripes) is TOMBSTONED:
// the name stays reserved, awaiting a joey-marked rebind. A Proc that
// never posted is a cheap no-op scan. Maps to specs/corvus.tla
// ServiceTombstone.
void srv_proc_exit_notify(struct Proc *p);

// srv_registry_count — number of non-FREE registry entries. Tests +
// diagnostics; takes the registry lock.
int srv_registry_count(void);

#endif  // THYLACINE_DEVSRV_H
