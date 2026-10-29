/* Uninstalled single-consumer owned-payload core; native adapter pending.
 * All callbacks are trusted, bounded, non-reentrant and never call this API.
 * Backing is exclusive, stable, disjoint by buffer index and never mutated while
 * leased. Nonzero RETURN preserves bytes. begin_retire stops new claims;
 * retired proves all local writers ended. close preserves independent kernel
 * pins/charges until physical retirement even after destroy releases mappings.
 * Final adapter must share full-ring retirement authority and enforce total64
 * members/cross-pool exclusions. This core alone is not that integration. */
#ifndef THYLA_PAYLOAD_OWNER_H
#define THYLA_PAYLOAD_OWNER_H
#include "lease_ledger.h"
struct t_payload_backend {
 void *context;
 size_t (*count)(void *);size_t (*length)(void *,size_t);
 const unsigned char *(*buffer)(void *,size_t);
 int64_t (*return_buffer)(void *,struct t_loom_service_buffer_receipt);
 void (*begin_retire)(void *);int (*retired)(void *);
 void (*close)(void *);void (*destroy)(void *);
};
struct t_payload_owner;struct t_payload_pool;struct t_payload_lease;
/* Valid backend/context and nonnull distinct outputs required. Consumes backend
 * on success AND geometry/allocation failure, closing before destroy on error. */
int t_payload_create(struct t_payload_backend,struct t_loom_service_ref,
 const struct t_loom_service_pool_member *,size_t,struct t_payload_owner **,struct t_payload_pool **);
/* Input must be the authenticated paired READ record for this pool. */
int t_payload_accept(struct t_payload_pool *,int32_t,uint32_t,
 struct t_loom_service_buffer_receipt,struct t_payload_lease **);
/* View remains valid until this lease is returned/abandoned. No mutable view.
 * C caller must end all pointer use before release, and never use a lease handle
 * after successful RETURN/abandon. Reused slot addresses do not revive handles. */
const unsigned char *t_payload_bytes(const struct t_payload_lease *,size_t *);
/* Zero consumes lease; nonzero exact syscall error retains it for retry/view. */
int64_t t_payload_return(struct t_payload_lease *);
/* Consumes lease, queues failed return in pre-reserved member cell. */
void t_payload_abandon(struct t_payload_lease *);
void t_payload_pool_drop(struct t_payload_pool *);
/* At most64 attempts; true only closing + no live views + local retired. */
int t_payload_progress(struct t_payload_owner *);
size_t t_payload_outstanding(const struct t_payload_owner *);
size_t t_payload_pending(const struct t_payload_owner *);
/* Whole controller teardown; existing live leases retain backing. */
void t_payload_owner_drop(struct t_payload_owner *);
#endif
