#ifndef THYLACINE_DEVPROC_H
#define THYLACINE_DEVPROC_H

#include <thylacine/types.h>

struct Proc;
struct Spoor;
struct Thread;

// Called from kernel/devctl.c (no header held it before).
bool devproc_none_walled(const struct Proc *caller, const struct Proc *target);

// Called from kernel/devctl.c (no header held it before).
bool devproc_owner_or_hostowner(const struct Proc *caller, const struct Proc *target);

#ifdef KERNEL_TESTS
// The in-kernel tests' reach into devproc.c (.claude/rules/kernel-code.md,
// "Cross-file declarations"): hooks compiled only with the tests, and
// functions that are static in a production build (VISIBLE_IF_KERNEL_TESTS).
bool devproc_all_threads_parked(struct Proc *target);
bool devproc_debug_authorized(const struct Proc *caller, const struct Proc *target);
int devproc_debug_stop_state_for_test(int pid, struct Spoor *ctl);
bool devproc_extract_authorized(const struct Proc *caller, const struct Proc *target);
struct Thread *devproc_focus_thread(struct Proc *target);
size_t devproc_imperium_read_gated(const struct Proc *caller, struct Proc *target,
                                   char *buf, size_t cap, bool *denied);
bool devproc_kill_authorized(const struct Proc *caller, const struct Proc *target);
bool devproc_maps_code_visible(const struct Proc *caller, const struct Proc *target);
bool devproc_park_state_is_settled(bool registered, bool on_cpu, int state);
bool devproc_sched_authorized(const struct Proc *caller, const struct Proc *target);
size_t devproc_sched_read_gated(const struct Proc *caller, struct Proc *target,
                                char *buf, size_t cap, bool *denied);
long devproc_step_result_for_test(int ev, long n);
int devproc_wait_state_for_test(int pid, struct Proc *caller, struct Spoor *ctl);
int devproc_wait_verdict_for_test(int state);
#endif // KERNEL_TESTS

#endif // THYLACINE_DEVPROC_H
