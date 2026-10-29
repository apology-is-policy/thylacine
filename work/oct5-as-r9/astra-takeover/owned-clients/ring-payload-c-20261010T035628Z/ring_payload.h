/* Uninstalled full-ring opaque payload core. Trusted backend owns immutable,
 * disjoint eager buffers; zero RETURN alone permits reuse. Pool destroy success
 * proves references ended. Bounded, non-reentrant callbacks preserve leased
 * bytes. Whole close retains independent kernel pins beyond backing destruction.
 * Single creator consumer; native registration/CQ/future integration pending. */
#ifndef THYLA_RING_PAYLOAD_H
#define THYLA_RING_PAYLOAD_H
#include "lease_ledger.h"
struct t_ring_backend{void *context;size_t(*count)(void *);size_t(*length)(void *,size_t);const unsigned char *(*buffer)(void *,size_t);int64_t(*return_buffer)(void *,struct t_loom_service_buffer_receipt);void(*retire_pool)(void *,struct t_loom_service_ref);int64_t(*destroy_pool)(void *,struct t_loom_service_ref);void(*begin_retire)(void *);int(*retired)(void *);void(*close)(void *);void(*destroy)(void *);};
struct t_ring_owner;struct t_ring_client;struct t_ring_pool;struct t_ring_payload;
/* Consumes well-formed backend even on geometry/allocation failure. */
int t_ring_create(struct t_ring_backend,struct t_ring_owner **,struct t_ring_client **);
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
#endif
