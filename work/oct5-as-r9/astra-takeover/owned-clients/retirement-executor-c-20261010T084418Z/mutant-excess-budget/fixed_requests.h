#ifndef THYLA_FIXED_REQUESTS_H
#define THYLA_FIXED_REQUESTS_H
#include "ring_payload.h"
#include "request_book.h"
/* Uninstalled single-creator typed controller. No callback reentry. SQ publisher,
 * event wakeup/executor and native adapter remain absent. No extra close owner. */
struct t_fixed_driver;struct t_fixed_request;struct t_fixed_read_result;
#define T_FIXED_CLOSING (-1000)
int t_fixed_create(struct t_ring_backend,size_t,struct t_fixed_driver **);
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
void t_fixed_request_drop(struct t_fixed_request *);
void t_fixed_close(struct t_fixed_driver *);
void t_fixed_begin_retire(struct t_fixed_driver *);
int t_fixed_progress(struct t_fixed_driver *);
size_t t_fixed_occupied(const struct t_fixed_driver *);
void t_fixed_driver_drop(struct t_fixed_driver *);
#endif
