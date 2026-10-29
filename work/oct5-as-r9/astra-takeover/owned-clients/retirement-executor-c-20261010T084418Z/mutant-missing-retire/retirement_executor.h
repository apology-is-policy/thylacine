#ifndef THYLA_RETIREMENT_EXECUTOR_H
#define THYLA_RETIREMENT_EXECUTOR_H
#include "fixed_requests.h"
/* Uninstalled creator-thread owner. No concurrent calls/callback reentry. */
struct t_retirement_executor;struct t_managed_client;
struct t_retirement_progress{size_t visited,reclaimed,occupied;};
#define T_RETIREMENT_FULL (-1001)
#define T_RETIREMENT_CAPACITY (-1002)
/* Factory must produce exactly one owned driver on zero, NULL on failure.
 * Capacity and client allocation are secured before the factory runs. */
typedef int (*t_driver_factory)(void *,struct t_fixed_driver **);
int t_retirement_create(size_t,struct t_retirement_executor **);
int t_retirement_admit(struct t_retirement_executor *,t_driver_factory,void *,struct t_managed_client **);
struct t_retirement_progress t_retirement_progress(struct t_retirement_executor *,size_t);
size_t t_retirement_occupied(const struct t_retirement_executor *);
void t_retirement_drop(struct t_retirement_executor *);
int t_managed_read(struct t_managed_client *,struct t_loom_service_ref,struct t_loom_service_pool_member,struct t_fixed_request **);
int t_managed_write(struct t_managed_client *,struct t_loom_service_ref,struct t_loom_service_pool_member,const void *,size_t,struct t_fixed_request **);
/* Same authenticated CQ + ended local access requirement as t_fixed_receive. */
int t_managed_receive(struct t_managed_client *,const struct t_paired_record *);
void t_managed_drop(struct t_managed_client *);
#endif
