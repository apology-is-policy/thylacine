// /ctl (devctl.c): the kernel's control and introspection files. Its Dev is
// declared with the others in dev.h; nothing outside devctl.c calls into it
// but the in-kernel tests.

#ifndef THYLACINE_DEVCTL_H
#define THYLACINE_DEVCTL_H

#include <thylacine/types.h>

struct Proc;

#ifdef KERNEL_TESTS
// The in-kernel tests' reach into devctl.c (.claude/rules/kernel-code.md,
// "Cross-file declarations"): hooks compiled only with the tests, and
// functions that are static in a production build (VISIBLE_IF_KERNEL_TESTS).
size_t devctl_format_9p_sessions_for_test(const struct Proc *reader, char *buf, size_t cap);
size_t devctl_format_procs_for_test(const struct Proc *reader, char *buf, size_t cap);
size_t devctl_format_kstack_for_test(const struct Proc *reader, char *buf, size_t cap);
size_t devctl_format_kstack_budget_for_test(char *buf, size_t cap, u32 budget_words);
u32 devctl_kstack_budget_words_for_test(void);
u32 devctl_kstack_visit_words_for_test(void);
u64 devctl_procs_snaps_live_for_test(void);   // /ctl/procs snapshots not yet freed
bool devctl_kernel_base_readable(const struct Proc *caller);
bool devctl_system_counters_readable(const struct Proc *reader);
#endif // KERNEL_TESTS

#endif // THYLACINE_DEVCTL_H
