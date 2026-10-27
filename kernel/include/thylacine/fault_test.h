#ifndef THYLACINE_FAULT_TEST_H
#define THYLACINE_FAULT_TEST_H

#include <thylacine/types.h>

// Called from kernel/main.c (no header held it before).
void fault_test_run(void);

#endif // THYLACINE_FAULT_TEST_H
