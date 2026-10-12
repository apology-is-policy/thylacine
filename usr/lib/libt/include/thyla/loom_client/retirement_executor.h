#ifndef THYLA_LOOM_CLIENT_RETIREMENT_EXECUTOR_H
#define THYLA_LOOM_CLIENT_RETIREMENT_EXECUTOR_H
#include <thyla/loom_client/fixed_requests.h>
/* Uninstalled creator-thread owner. No concurrent calls/callback reentry. */
struct t_retirement_executor;
struct t_managed_client{struct t_retirement_executor *executor;size_t slot;};
struct t_retirement_cell{struct t_fixed_driver *driver;unsigned retiring,client_alive;struct t_managed_client client;};
/* The caller's storage: zero it before its first use (static storage is), and
 * reuse it only once the executor and every managed client have dropped. Its
 * fields are the core's own. A cell's managed client lives inside the cell. */
struct t_retirement_executor{struct t_retirement_cell cells[64];size_t capacity,cursor,deadline_cursor,refs;unsigned open;};
struct t_retirement_progress{size_t visited,reclaimed,occupied;};
/* Not -ENOSPC: admit also returns the factory's own errors, and FULL alone
 * means the factory never ran, so the caller still owns what it would consume. */
#define T_RETIREMENT_FULL (-1001)
#define T_RETIREMENT_CAPACITY (-1002)
/* Factory must produce exactly one owned driver on zero, NULL on failure; the
 * driver's storage is the factory's. Capacity and the client cell are secured
 * before the factory runs. */
typedef int (*t_driver_factory)(void *,struct t_fixed_driver **);
int t_retirement_create(struct t_retirement_executor *,size_t,struct t_retirement_executor **);
int t_retirement_admit(struct t_retirement_executor *,t_driver_factory,void *,struct t_managed_client **);
struct t_retirement_progress t_retirement_progress(struct t_retirement_executor *,size_t);
size_t t_retirement_occupied(const struct t_retirement_executor *);
/* Poll timers for attached AND retiring drivers. next_deadline scans at most
 * capacity*64 cells; 1 found (including zero), 0 none. expire visits at most
 * min(ring_budget,capacity)*min(fixed_budget,64) cells with independent rotating
 * cursors, returns newly requested abort count, and never reclaims a driver.
 * Zero budget at either level performs no aborts. Repeat fair positive-budget
 * scans while work remains due; these APIs do not provide a wait/wake primitive.
 * Like other executor-root APIs, valid only until t_retirement_drop consumes it. */
int t_retirement_next_deadline(const struct t_retirement_executor *,uint64_t *);
size_t t_retirement_expire(struct t_retirement_executor *,uint64_t now,size_t ring_budget,size_t fixed_budget);
void t_retirement_drop(struct t_retirement_executor *);
int t_managed_read(struct t_managed_client *,struct t_loom_service_ref,struct t_loom_service_pool_member,struct t_fixed_request **);
int t_managed_write(struct t_managed_client *,struct t_loom_service_ref,struct t_loom_service_pool_member,const void *,size_t,struct t_fixed_request **);
/* Same authenticated CQ + ended local access requirement as t_fixed_receive. */
int t_managed_receive(struct t_managed_client *,const struct t_paired_record *);
void t_managed_drop(struct t_managed_client *);
#endif
