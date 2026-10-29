// The deliberate-fault witnesses (fault_test.c): a kernel built with
// THYLACINE_FAULT_TEST_<name> fires exactly one hardening protection at boot,
// and tools/test-fault.sh checks the extinction it causes.

#ifndef THYLACINE_FAULT_TEST_H
#define THYLACINE_FAULT_TEST_H

#include <thylacine/types.h>

// boot_main's call, before the success line; returns at once in a kernel built
// without a fault-test define.
void fault_test_run(void);

#if defined(THYLACINE_FAULT_TEST_vbar_pre_reloc) || defined(THYLACINE_FAULT_TEST_vbar_pre_mmu)
// An unaligned load, called with stage 1 off: kaslr_init's (image unrelocated)
// and build_page_tables' (relocated) sites.
void fault_test_unaligned_load(void);
#endif

#endif // THYLACINE_FAULT_TEST_H
