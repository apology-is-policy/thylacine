/* Internal metadata only. Not the public opaque lease API or storage owner. */
#ifndef THYLA_LOOM_CLIENT_LEASE_LEDGER_H
#define THYLA_LOOM_CLIENT_LEASE_LEDGER_H
#include <thyla/loom_client/base.h>
enum t_lease_error { T_LEASE_GEOMETRY=-1101,T_LEASE_RECEIPT=-1102,T_LEASE_BUSY=-1103,
    T_LEASE_STALE=-1104,T_LEASE_STATE=-1105,T_LEASE_EXHAUSTED=-1106 };
_Static_assert(T_LOOM_CLIENT_LOCAL(T_LEASE_GEOMETRY,1100)&&T_LOOM_CLIENT_LOCAL(T_LEASE_RECEIPT,1100)&&
    T_LOOM_CLIENT_LOCAL(T_LEASE_BUSY,1100)&&T_LOOM_CLIENT_LOCAL(T_LEASE_STALE,1100)&&
    T_LOOM_CLIENT_LOCAL(T_LEASE_STATE,1100)&&T_LOOM_CLIENT_LOCAL(T_LEASE_EXHAUSTED,1100),"lease codes lie in their block");
struct t_lease_token { uint64_t owner;struct t_loom_service_buffer_receipt receipt;size_t length; };
/* One cell per pool member, shared by every pool of a ring as the kernel's pool
 * bank is (at most 64 members across all pools). Owner zero marks a free cell;
 * otherwise owner and ordinal name the ledger and member that hold it. */
struct t_lease_member { uint64_t owner;uint32_t ordinal;unsigned state;struct t_loom_service_pool_member range;uint64_t last; };
struct t_lease_bank { struct t_lease_member cells[64]; };
struct t_lease_ledger { uint64_t owner;struct t_loom_service_ref pool;struct t_lease_bank *bank;size_t count,outstanding;unsigned bound; };
/* Claims one bank cell per range, or answers T_LOOM_CLIENT_ENOSPC with the bank
 * and ledger unchanged. Zero the ledger before its first use. A live ledger is
 * released before it is initialized again: a well-formed init over one ends the
 * process, and a malformed one is refused with the ledger unchanged. */
int t_lease_init(struct t_lease_ledger *,struct t_lease_bank *,struct t_loom_service_ref,
    const struct t_loom_service_pool_member *,size_t,const uint64_t *,size_t);
int t_lease_prepare(struct t_lease_ledger *,struct t_lease_bank *,const struct t_loom_service_pool_member *,size_t,const uint64_t *,size_t);
int t_lease_bind(struct t_lease_ledger *,struct t_loom_service_ref);
/* 1 transfers metadata token,0 is nonpayload terminal,negative rejects unchanged. */
int t_lease_accept_read(struct t_lease_ledger *,int32_t,uint32_t,
    struct t_loom_service_buffer_receipt,struct t_lease_token *);
int t_lease_begin_return(struct t_lease_ledger *,const struct t_lease_token *);
int t_lease_finish_return(struct t_lease_ledger *,const struct t_lease_token *,int64_t);
/* Bank index of a live member, for storage kept beside the bank. */
size_t t_lease_cell(const struct t_lease_ledger *,uint32_t);
/* Frees every cell the ledger holds, charged or not; it then accepts nothing. */
void t_lease_release(struct t_lease_ledger *);
#endif
