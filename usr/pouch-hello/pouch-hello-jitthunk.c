// /pouch-hello-jitthunk -- the witness for the bounded write thunk
// (usr/lib/thylajit/thyla_jit_thunk.h), the encoding JavaScriptCore writes its
// code through (usr/ports/webkit/patches/0002), run against the kernel.
//
// Acquires CAP_JIT by the SELF form, creates a 64 KiB region, seals the thunk,
// then:
//   in bounds  writes at the region's edges through the thunk and reads each back
//              through the exec alias: 8 bytes 8-aligned at the end (one store),
//              7 bytes at offset 1 (an unaligned word, then a byte tail), 4 bytes
//              at the end, and 0 bytes at offset == size (no trap, no write);
//   out of     `pouch-hello-jitthunk <case>` (case 0..3) makes ONE call the bounds
//   bounds     must refuse, after announcing it, and exits 42 only if the thunk
//              returned. One invocation per case: a Proc with a live JIT region
//              cannot fork (clone_one_vma refuses CODE). The kernel names the death
//              on the console: `reason="snare:brk"` is the thunk's trap; `snare:segv`
//              would mean the store ran outside the region (the gate fails on it).
// With no argument the last line is "jitthunk: in bounds done", or a FAIL line.

#define THYLA_CAPJIT_SELF_ONLY
#include <thyla_jit_thunk.h>
#include <thyla_capjit.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define REGION ((uint64_t)64 * 1024)

static thyla_jit_write_fn g_write;
static const volatile uint8_t *g_exec;

static int fail(const char *what)
{
    printf("jitthunk: FAIL %s\n", what);
    return 1;
}

// Writes n bytes at off through the thunk and reads them back through the
// exec alias.
static int round_trip(int64_t off, size_t n, uint8_t seed, const char *what)
{
    uint8_t src[16];
    for (size_t i = 0; i < sizeof src; i++)
        src[i] = (uint8_t)(seed + 17 * i);
    g_write(off, src, n);
    for (size_t i = 0; i < n; i++) {
        if (g_exec[off + (int64_t)i] != src[i])
            return fail(what);
    }
    printf("jitthunk: in bounds %s ok\n", what);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (thyla_acquire_cap_jit("jitthunk") != 0)
        return fail("CAP_JIT not acquired");

    struct thyla_jit_region region;
    long rc = thyla_jit_create(REGION, &region);
    if (rc)
        return fail("SYS_JIT_CREATE");
    uint64_t thunk_va = 0;
    rc = thyla_jit_thunk_create(region.writer_va, REGION, &thunk_va);
    if (rc)
        return fail("SYS_JIT_CREATE_SEALED");
    g_write = (thyla_jit_write_fn)(uintptr_t)thunk_va;
    g_exec = (const volatile uint8_t *)(uintptr_t)region.exec_va;

    if (argc > 1) {
        static const struct { int64_t off; size_t n; const char *what; } oob[] = {
            { (int64_t)REGION,     1,          "1@end" },
            { (int64_t)REGION - 4, 5,          "5@end-4" },
            { -8,                  8,          "8@-8" },
            { 0,                   REGION + 1, "size+1@0" },
        };
        static const uint8_t junk[16];
        int c = argv[1][0] - '0';
        if (argv[1][1] != '\0' || c < 0 || c > 3)
            return fail("case must be 0..3");
        printf("jitthunk: out of bounds %s calling\n", oob[c].what);
        g_write(oob[c].off, junk, oob[c].n);
        printf("jitthunk: FAIL out of bounds %s returned\n", oob[c].what);
        return 42;
    }

    if (round_trip((int64_t)REGION - 8, 8, 0x31, "8@end-8")
        || round_trip(1, 7, 0x52, "7@1")
        || round_trip((int64_t)REGION - 4, 4, 0x73, "4@end-4"))
        return 1;
    uint8_t before = g_exec[REGION - 1];
    g_write((int64_t)REGION, "x", 0);
    if (g_exec[REGION - 1] != before)
        return fail("0@end wrote");
    printf("jitthunk: in bounds 0@end ok\n");

    printf("jitthunk: in bounds done\n");
    return 0;
}
