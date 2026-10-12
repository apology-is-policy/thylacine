#ifndef THYLA_LOOM_CLIENT_FIXED_REQUESTS_H
#define THYLA_LOOM_CLIENT_FIXED_REQUESTS_H
#include <thyla/loom_client/ring_payload.h>
#include <thyla/loom_client/request_book.h>
/* Uninstalled single-creator typed controller. No callback reentry. SQ publisher,
 * event wakeup/executor and native adapter remain absent. No extra close owner. */
struct t_fixed_storage;struct t_fixed_request;
struct t_fixed_driver{struct t_fixed_storage *storage;struct t_ring_owner *owner;struct t_ring_client *client;struct t_request_book *book;size_t refs;unsigned failed,alive;};
struct t_fixed_read_result{struct t_fixed_request *allocation;struct t_ring_fixed *fixed;struct t_owned_cqe cqe;};
struct t_fixed_request{struct t_fixed_driver *driver;struct t_ring_fixed *fixed;uint64_t cookie;unsigned used,published,write;struct t_fixed_read_result result;};
/* One ring client's storage (docs/ASYNC-SERVICE-ABI.md, "Native C client
 * storage"): the driver, its ring core, and a request cell per registered
 * buffer, each holding its buffer's fixed handle until completion or drop. The
 * request book's entries come with it from T_FIXED_STORAGE. Zero it before its
 * first use (static storage is), and reuse it only once t_fixed_storage_idle
 * answers 1. Its fields are the core's own. */
struct t_fixed_storage{struct t_fixed_driver driver;struct t_ring_core ring;struct t_request_book book;struct t_fixed_request requests[64];};
/* entries is the ring's CQ size: one book entry per CQ slot. */
#define T_FIXED_STORAGE(entries) struct{struct t_fixed_storage base;struct t_request_entry book[(entries)];}
#define T_FIXED_CREATE(backend,storage,out) t_fixed_create((backend),&(storage)->base,(storage)->book,sizeof((storage)->book)/sizeof((storage)->book[0]),(out))
#define T_FIXED_CLOSING (-1000)
/* Consumes backend on every path; the storage must be idle. */
int t_fixed_create(struct t_ring_backend,struct t_fixed_storage *,struct t_request_entry *,size_t,struct t_fixed_driver **);
/* 1 once the driver, every request and read result, and the ring have ended. */
int t_fixed_storage_idle(const struct t_fixed_storage *);
/* A full request book answers T_REQ_FULL (T_LOOM_CLIENT_ENOSPC). */
int t_fixed_read(struct t_fixed_driver *,struct t_loom_service_ref,struct t_loom_service_pool_member,struct t_fixed_request **);
int t_fixed_write(struct t_fixed_driver *,struct t_loom_service_ref,struct t_loom_service_pool_member,const void *,size_t,struct t_fixed_request **);
/* Integrator activates immediately before release-SQ-tail, with no fallible
 * work after success. Returned cookie is mandatory. Native publisher is parked. */
int t_fixed_publish(struct t_fixed_request *,uint64_t *);
/* Requires authenticated paired CQ from this exact exclusive mapping, after
 * final local buffer access. Never synthesize from ENTER or ABORT return code. */
int t_fixed_receive(struct t_fixed_driver *,const struct t_paired_record *);
/* 0 pending,1 complete (consumes request),negative controller error (retains).
 * Remote I/O errno is completion.result and never means remote rollback. */
int t_fixed_read_complete(struct t_fixed_request *,struct t_fixed_read_result **);
int t_fixed_write_complete(struct t_fixed_request *,struct t_owned_cqe *);
int32_t t_fixed_read_status(const struct t_fixed_read_result *);
int64_t t_fixed_read_bytes(const struct t_fixed_read_result *,const unsigned char **,size_t *);
/* End all result view uses before drop; never reuse a consumed handle. */
void t_fixed_read_drop(struct t_fixed_read_result *);
/* Cancellation is scope-wide, idempotent per request, and never completion.
 * A later successful WRITE remains success; no remote rollback is implied.
 * set_deadline only before publish; enabled=0 clears, enabled=1 includes zero.
 * next_deadline/expire are caller-driven bounded polling, no native wake adapter.
 * Due work remaining after a partial/zero budget must be serviced before sleep.
 * See ring_payload.h for return values. Handles remain owned until completion/drop. */
int t_fixed_set_deadline(struct t_fixed_request *,int enabled,uint64_t deadline);
int t_fixed_cancel(struct t_fixed_request *);
int t_fixed_next_deadline(const struct t_fixed_driver *,uint64_t *);
size_t t_fixed_expire(struct t_fixed_driver *,uint64_t now,size_t budget);
void t_fixed_request_drop(struct t_fixed_request *);
void t_fixed_close(struct t_fixed_driver *);
void t_fixed_begin_retire(struct t_fixed_driver *);
int t_fixed_progress(struct t_fixed_driver *);
size_t t_fixed_occupied(const struct t_fixed_driver *);
void t_fixed_driver_drop(struct t_fixed_driver *);
#endif
