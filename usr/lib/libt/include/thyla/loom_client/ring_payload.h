/* Uninstalled full-ring opaque payload core. Trusted backend owns immutable,
 * disjoint eager buffers; zero RETURN alone permits reuse. Pool destroy success
 * proves references ended. Bounded, non-reentrant callbacks preserve leased
 * bytes. Whole close retains independent kernel pins beyond backing destruction.
 * Single creator consumer; native registration/CQ/future integration pending. */
#ifndef THYLA_LOOM_CLIENT_RING_PAYLOAD_H
#define THYLA_LOOM_CLIENT_RING_PAYLOAD_H
#include <thyla/loom_client/lease_ledger.h>
/* Backing is initialized and writable; fixed terminal proves all local access
 * ended. abort_scope is bounded and never grants buffer reuse. */
struct t_ring_backend{void *context;size_t(*count)(void *);size_t(*length)(void *,size_t);const unsigned char *(*buffer)(void *,size_t);int64_t(*return_buffer)(void *,struct t_loom_service_buffer_receipt);void(*retire_pool)(void *,struct t_loom_service_ref);int64_t(*destroy_pool)(void *,struct t_loom_service_ref);void(*begin_retire)(void *);int(*retired)(void *);void(*close)(void *);void(*destroy)(void *);int64_t(*register_pool)(void *,const struct t_loom_service_pool_member *,size_t,struct t_loom_service_ref *);void(*abort_scope)(void *,struct t_loom_service_ref);};
struct t_ring_core;struct t_ring_pool_state;
struct t_ring_owner{struct t_ring_core *core;};struct t_ring_client{struct t_ring_core *core;};struct t_ring_pool{struct t_ring_pool_state *pool;};
struct t_ring_payload{struct t_ring_pool_state *pool;struct t_lease_token token;unsigned active,pending;};
struct t_ring_pool_state{struct t_ring_core *core;struct t_loom_service_ref identity;struct t_lease_ledger ledger;size_t count,live;unsigned used,handle_alive,closing;struct t_ring_pool handle;};
struct t_ring_fixed{struct t_ring_core *core;struct t_loom_service_pool_member range;struct t_loom_service_ref scope;uint64_t cookie;unsigned phase;int32_t result;uint64_t deadline;unsigned write,alive,abandoned,has_deadline,abort_requested;};
/* One ring's storage, the caller's: zero it before its first use (static storage
 * is), and reuse it only once t_ring_idle answers 1. Its fields are the core's
 * own. Every table is sized by the ABI: 64 registered buffers, 64 pool members
 * across all pools (so at most 64 pools), one fixed cell per buffer. A pool's
 * header and member cells are claimed before its kernel registration. */
struct t_ring_core{struct t_ring_fixed fixed[64];size_t fixed_handles,deadline_cursor;struct t_ring_backend b;struct t_ring_pool_state *pools[64],*quarantine;struct t_ring_pool_state pool_cells[64];struct t_lease_bank bank;struct t_ring_payload leases[64];uint64_t last[64];size_t members,live,pool_handles;unsigned in_use,closing,closed,root_alive,client_alive;struct t_ring_owner root;struct t_ring_client client;};
/* Consumes well-formed backend even on geometry failure. */
int t_ring_create(struct t_ring_backend,struct t_ring_core *,struct t_ring_owner **,struct t_ring_client **);
/* 1 once every handle has dropped and the backend is destroyed. */
int t_ring_idle(const struct t_ring_core *);
/* Backend register: zero means created; negative plus zero output proves no
 * surviving registration/pins. Any other result forces close with quarantined
 * preclaimed metadata. Callbacks cannot reenter or submit before publication.
 * Exact negative errno is returned for definite rejection. Uncertain outcomes
 * return T_RING_UNCERTAIN, with native rc in *native_rc (also set on success).
 * *native_rc is zero when local preflight fails; no backend call occurred.
 * More than 64 members across the ring's pools is T_LOOM_CLIENT_ENOSPC.
 * UNCERTAIN lies outside the POSIX errno range, which a definite rejection
 * returns: -100 would be read as ENETDOWN. */
#define T_RING_UNCERTAIN (-1003)
_Static_assert(T_RING_UNCERTAIN<=-1000,"T_RING_UNCERTAIN must lie below every errno");
int64_t t_ring_register_pool(struct t_ring_client *,const struct t_loom_service_pool_member *,size_t,struct t_ring_pool **,int64_t *native_rc);
/* Native adapter must already own exact kernel pool and error cleanup. Serialized
 * provisional registration/preflight is still to be wired before public use. */
int t_ring_attach(struct t_ring_client *,struct t_loom_service_ref,const struct t_loom_service_pool_member *,size_t,struct t_ring_pool **);
/* Input must be authenticated paired READ for this pool. */
int t_ring_accept(struct t_ring_pool *,int32_t,uint32_t,struct t_loom_service_buffer_receipt,struct t_ring_payload **);
/* End every view use before return/abandon. Never use consumed handles again. */
const unsigned char *t_ring_bytes(const struct t_ring_payload *,size_t *);
int64_t t_ring_return(struct t_ring_payload *);
void t_ring_abandon(struct t_ring_payload *);
void t_ring_pool_drop(struct t_ring_pool *);
void t_ring_client_drop(struct t_ring_client *);
int t_ring_progress(struct t_ring_owner *);
size_t t_ring_members(const struct t_ring_owner *);
size_t t_ring_live(const struct t_ring_owner *);
size_t t_ring_pending(const struct t_ring_owner *);
void t_ring_owner_drop(struct t_ring_owner *);
/* Close once while retaining root bookkeeping for terminal/retirement polling. */
void t_ring_close(struct t_ring_owner *);
/* Exclusive fixed-I/O ownership, one preallocated reservation per registered
 * buffer. Cookie comes from this ring's request book. Do not retain/use aliases
 * after dropping a handle. No view is available during submitted I/O.
 * publish is paired with request activation immediately before SQ release;
 * finish is only for an authenticated terminal CQ after local access ends.
 * Failed ENTER and requested ABORT do not authorize finish or buffer reuse. */
int t_ring_fixed_reserve(struct t_ring_client *,uint64_t,struct t_loom_service_ref,struct t_loom_service_pool_member,int,struct t_ring_fixed **);
int t_ring_fixed_write(struct t_ring_fixed *,const void *,size_t);
int t_ring_fixed_publish(struct t_ring_fixed *);
/* Set/clear absolute local deadline before publication only. enabled distinguishes
 * a deadline of zero from no timer; this is not the native ABI encoding.
 * cancel: 1 requested, 0 already requested/terminal/closed, negative if prepared.
 * Scope-wide abort may affect siblings; neither API releases active storage.
 * expire visits min(budget,64) rotating cells, returns newly requested count.
 * next_deadline: 1 found, 0 none. Both exclude terminal/cancelled requests. */
int t_ring_fixed_deadline(struct t_ring_fixed *,int enabled,uint64_t deadline);
int t_ring_fixed_cancel(struct t_ring_fixed *);
int t_ring_next_deadline(const struct t_ring_owner *,uint64_t *);
size_t t_ring_expire(struct t_ring_owner *,uint64_t now,size_t budget);
int t_ring_fixed_finish(struct t_ring_owner *,uint64_t,int32_t);
int64_t t_ring_fixed_bytes(const struct t_ring_fixed *,const unsigned char **,size_t *);
void t_ring_fixed_drop(struct t_ring_fixed *);
size_t t_ring_fixed_count(const struct t_ring_owner *);
#endif
