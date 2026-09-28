// /pouch-hello-guard -- the B-1b FAULT child: a pthread's guard page is real.
//
// musl's pthread_create mints a thread's stack with mmap(PROT_NONE) and raises
// the usable part with mprotect(RW) (src/thread/pthread_create.c); until patch
// 0044 the seam minted RW regardless of prot and mprotect was ENOSYS, so the
// "guard" was ordinary writable memory (POUCH-DESIGN.md sec 8.2's limitation).
// A worker thread asks libc where its stack begins and writes the FIRST usable
// byte BEFORE it prints its marker -- the positive control: a boundary
// reported low faults there, and the marker never appears -- then writes the
// byte BELOW it, the last byte of the guard, and dies of snare:segv. joey runs
// this child with pouch_smoke_one_expect_fault: the marker must appear AND the
// exit status must be non-zero, so the fault is proven to be the guard's.
//
// On the old libc the second write succeeds, the thread prints SURVIVED and
// main returns 0 -- exactly the shape joey's expect_fault refuses. Output goes
// through raw write(1), as /pouch-hello-fault does: the fault ends the process
// before any stdio buffer would be flushed.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

static void *guard_worker(void *arg) {
    (void)arg;
    pthread_attr_t at;
    void *addr = NULL;
    size_t size = 0, guard = 0;
    if (pthread_getattr_np(pthread_self(), &at) != 0 ||
        pthread_attr_getstack(&at, &addr, &size) != 0 ||
        pthread_attr_getguardsize(&at, &guard) != 0 || guard == 0 || addr == NULL) {
        static const char no[] = "pouch-hello-guard: no stack attributes for the worker\n";
        (void)write(1, no, sizeof(no) - 1);
        return (void *)1;
    }
    volatile char *s = (volatile char *)addr;
    s[0] = 1;   // the first usable byte, before the marker: a low boundary faults here

    char line[96];
    int n = snprintf(line, sizeof line, POUCH_CENSUS_GUARD " %lx bytes below %p; touching\n",
                     (unsigned long)guard, addr);
    if (n > 0) (void)write(1, line, (size_t)n);

    s[-1] = 1;  // the guard's last byte: PROT_NONE -> EL0 fault -> snare:segv

    // UNREACHABLE on the B-1b libc. Reached on a libc whose mmap ignores prot.
    static const char survived[] = "pouch-hello-guard: SURVIVED the guard write (no guard)\n";
    (void)write(1, survived, sizeof(survived) - 1);
    return (void *)2;
}

int main(void) {
    pthread_t t;
    void *r = NULL;
    if (pthread_create(&t, NULL, guard_worker, NULL) != 0) {
        static const char no[] = "pouch-hello-guard: pthread_create failed\n";
        (void)write(1, no, sizeof(no) - 1);
        return 3;
    }
    (void)pthread_join(t, &r);
    // A clean exit here means the guard did not fault: joey's expect_fault
    // refuses status 0, so the regression is loud whichever way it comes.
    return 0;
}
