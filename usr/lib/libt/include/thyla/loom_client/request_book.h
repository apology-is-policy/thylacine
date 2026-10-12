#ifndef THYLA_LOOM_CLIENT_REQUEST_BOOK_H
#define THYLA_LOOM_CLIENT_REQUEST_BOOK_H
#include <thyla/loom_client/paired_cq.h>
/* Internal metadata only. Caller retains actual I/O/pool/scope owners. Exclusive
 * creator consumer, no copying/reentry. Never expose user-chosen cookies. */
enum t_request_kind { T_REQ_CONTROL, T_REQ_FIXED_READ, T_REQ_FIXED_WRITE, T_REQ_POOLED_READ };
struct t_request_spec { enum t_request_kind kind;uint32_t limit,multi;struct t_loom_service_ref pool; };
struct t_request_delivery { uint64_t cookie;struct t_loom_service_ref scope;struct t_request_spec spec;unsigned terminal,abandoned;struct t_paired_record record; };
enum t_request_error { T_REQ_GEOMETRY=-1,T_REQ_FULL=T_LOOM_CLIENT_ENOSPC,T_REQ_EXHAUSTED=-3,T_REQ_STALE=-4,T_REQ_STATE=-5,T_REQ_RECORD=-6 };
/* One entry per CQ slot, in the caller's array; its fields are the book's own. */
struct t_request_entry { uint64_t cookie;struct t_loom_service_ref scope;struct t_request_spec spec;unsigned phase,abandoned;struct t_owned_cqe terminal; };
struct t_request_book { size_t count;struct t_request_entry *entries; };
/* Zeroes and adopts cq_entries entries, a power of two up to 8192. On failure,
 * and after destroy, the book is dead: it holds nothing, refuses reserve with
 * T_REQ_GEOMETRY and finds no cookie (T_REQ_STALE). */
int t_request_create(struct t_request_book *,struct t_request_entry *,size_t cq_entries);
/* Caller must prove whole-owner retirement before destroying occupied book. */
void t_request_destroy(struct t_request_book *);
int t_request_reserve(struct t_request_book *,struct t_loom_service_ref,struct t_request_spec,uint64_t *);
/* Only before SQ publication. Failed ENTER never permits unpublished rollback. */
int t_request_cancel_unpublished(struct t_request_book *,uint64_t);
int t_request_publish(struct t_request_book *,uint64_t);
/* Paired record from owned mapping only. Reject leaves ownership unchanged;
 * retire outer owner on malformed CQ. Pool ledger still checks lease/geometry. */
int t_request_accept(struct t_request_book *,const struct t_paired_record *,struct t_request_delivery *);
/* 1: first pending drop requests returned scope-wide abort;0: no new abort. */
int t_request_abandon(struct t_request_book *,uint64_t,struct t_loom_service_ref *);
/* Consume after payload transfer/fixed-owner settlement; no payload return. */
int t_request_take_terminal(struct t_request_book *,uint64_t,struct t_owned_cqe *);
/* Requires proven whole-owner retirement, detached consumer and settled local
 * fixed/payload borrowers; no future completion can be delivered. */
void t_request_clear_retired(struct t_request_book *);
size_t t_request_occupied(const struct t_request_book *);
#ifdef T_REQUEST_TEST
int t_request_exhausted_test(void);
#endif
#endif
