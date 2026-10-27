// The deliberate-fault witnesses (fault_test.c): a kernel built with
// THYLACINE_FAULT_TEST_<name> fires exactly one hardening protection at boot,
// and tools/test-fault.sh checks the extinction it causes.

#ifndef THYLACINE_FAULT_TEST_H
#define THYLACINE_FAULT_TEST_H

#include <thylacine/types.h>

// boot_main's call, before the success line; returns at once in a kernel built
// without a fault-test define.
void fault_test_run(void);

#endif // THYLACINE_FAULT_TEST_H
