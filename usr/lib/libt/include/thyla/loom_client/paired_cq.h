/* Internal single-consumer paired reader; not an owned public C API. */
#ifndef THYLA_LOOM_CLIENT_PAIRED_CQ_H
#define THYLA_LOOM_CLIENT_PAIRED_CQ_H
#include <stdatomic.h>
#include <thyla/loom_client/base.h>
struct t_owned_cqe { uint64_t user_data; int32_t result; uint32_t flags; };
_Static_assert(sizeof(struct t_owned_cqe)==16,"CQE ABI");
_Static_assert(offsetof(struct t_owned_cqe,result)==8,"CQE result ABI");
_Static_assert(offsetof(struct t_owned_cqe,flags)==12,"CQE flags ABI");
enum{T_CQ_GEOMETRY=-1401,T_CQ_CORRUPT=-1402};
_Static_assert(T_LOOM_CLIENT_LOCAL(T_CQ_GEOMETRY,1400)&&T_LOOM_CLIENT_LOCAL(T_CQ_CORRUPT,1400),"CQ codes lie in their block");
struct t_paired_record { struct t_owned_cqe cqe;struct t_loom_service_buffer_receipt receipt; };
struct t_paired_cq { _Atomic uint32_t *head,*tail;const struct t_owned_cqe *cq;
 const struct t_loom_service_buffer_receipt *receipts;uint32_t entries,next; };
/* Caller owns aligned live mapping, one exclusive reader, release-tail producer
 * that respects acquire-head reuse. Do not copy active reader or expose legacy
 * reap. Validate SQ regions separately. Failure (T_CQ_GEOMETRY) leaves output unchanged. */
int t_paired_init(struct t_paired_cq *,void *,size_t,size_t,size_t,size_t,uint32_t);
/* 1 copies both then acknowledges,0 empty,T_CQ_CORRUPT corrupt. out must not alias mapping
 * or reader; caller retains the payload separately before allowing its return. */
int t_paired_reap(struct t_paired_cq *,struct t_paired_record *);
#endif
