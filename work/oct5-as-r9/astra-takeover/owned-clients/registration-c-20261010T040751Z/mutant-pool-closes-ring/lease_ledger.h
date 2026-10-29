/* Internal metadata only. Not the public opaque lease API or storage owner. */
#ifndef THYLA_OWNED_LEASE_LEDGER_H
#define THYLA_OWNED_LEASE_LEDGER_H
#include <stdint.h>
#include <stddef.h>
#include <thyla/loom_service.h>
enum t_lease_error { T_LEASE_GEOMETRY=-1,T_LEASE_RECEIPT=-2,T_LEASE_BUSY=-3,
    T_LEASE_STALE=-4,T_LEASE_STATE=-5,T_LEASE_EXHAUSTED=-6 };
struct t_lease_token { uint64_t owner;struct t_loom_service_buffer_receipt receipt;size_t length; };
struct t_lease_member { struct t_loom_service_pool_member range;uint64_t last;unsigned state; };
struct t_lease_ledger { uint64_t owner;struct t_loom_service_ref pool;
    struct t_lease_member members[64];size_t count,outstanding;unsigned bound; };
int t_lease_init(struct t_lease_ledger *,struct t_loom_service_ref,
    const struct t_loom_service_pool_member *,size_t,const uint64_t *,size_t);
int t_lease_prepare(struct t_lease_ledger *,const struct t_loom_service_pool_member *,size_t,const uint64_t *,size_t);
int t_lease_bind(struct t_lease_ledger *,struct t_loom_service_ref);
/* 1 transfers metadata token,0 is nonpayload terminal,negative rejects unchanged. */
int t_lease_accept_read(struct t_lease_ledger *,int32_t,uint32_t,
    struct t_loom_service_buffer_receipt,struct t_lease_token *);
int t_lease_begin_return(struct t_lease_ledger *,const struct t_lease_token *);
int t_lease_finish_return(struct t_lease_ledger *,const struct t_lease_token *,int64_t);
#endif
