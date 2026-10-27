// VISIBLE_IF_KERNEL_TESTS: a function the in-kernel tests call from another
// file (operator vote 19; .claude/rules/kernel-code.md, "Cross-file
// declarations"). It is static in a production build, so nothing outside its
// file can call it there; with the tests compiled in it is external, and its
// module's header declares it under #ifdef KERNEL_TESTS. Linux KUnit's
// VISIBLE_IF_KUNIT is the precedent.

#ifndef THYLACINE_VISIBILITY_H
#define THYLACINE_VISIBILITY_H

#ifdef KERNEL_TESTS
#define VISIBLE_IF_KERNEL_TESTS
#else
#define VISIBLE_IF_KERNEL_TESTS static
#endif

#endif // THYLACINE_VISIBILITY_H
