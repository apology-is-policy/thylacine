#!/usr/bin/env python3
"""Compile actual SrvConn lifecycle bodies with deterministic allocator seams.

This checks admission/rollback, not ARM layout, spinlocks or SMP execution.
The ordinary kernel boot also exercises the real structures and allocator.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
source = (ROOT / "kernel/srvconn.c").read_text()


def function(name):
    # Kernel functions have an unindented closing brace; fail if the shape
    # changes rather than quietly compiling an obsolete copied implementation.
    match = re.search(r"^(?:static )?(?:bool|void|struct SrvConn \*)\s*"
                      + name + r"\([^;]*?\) \{.*?^}\n", source, re.M | re.S)
    assert match, name
    return match.group()


bodies = "\n".join(function(n) for n in (
    "srvconn_reserve", "srvconn_unreserve", "srvconn_create",
    "srvconn_ref", "srvconn_teardown", "srvconn_unref"))
constants = []
for path, names in (
    ("devsrv.h", ["SRV_MAX_CONNS"]),
    ("srvconn.h", ["SRVCONN_MSIZE", "SRVCONN_BULK_MSIZE", "SRV_CONN_MAGIC"]),
):
    header = (ROOT / "kernel/include/thylacine" / path).read_text()
    for name in names:
        match = re.search(r"^#define\s+" + name + r"\s+[^\n]+", header, re.M)
        assert match, name
        constants.append(match.group())

fixture = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
#define KP_ZERO 0
#define SRVCONN_STATE_LIVE 1
#define SRVCONN_STATE_TORN 2
#define CHECK(x, why) do { if (!(x)) { fputs(why "\n", stderr); exit(1); } } while (0)
struct channel { void *buf; int lock, rendez, wrendez, role_waiters; bool eof; };
struct SrvConn {
    u64 magic, peer_stripes, server_stripes, client_deadline_ns;
    int ref, lock, state, peer_pid, poll_list;
    bool peer_console, client_timed_out;
    u32 msize;
    struct channel c2s, s2c;
};
static u64 g_srvconn_created, g_srvconn_freed;
static u32 g_srvconn_reserved;
static unsigned allocations, blocks, fail_at, linked;
static void (*allocation_hook)(void), (*free_hook)(void);
static void extinction(const char *why) { fputs(why, stderr); exit(2); }
static void *kmalloc(size_t n, unsigned flags) {
    (void)flags;
    if (allocation_hook) {
        void (*hook)(void) = allocation_hook; allocation_hook = NULL; hook();
    }
    if (++allocations == fail_at) return NULL;
    void *p = calloc(1, n); CHECK(p, "host fixture allocation failed"); blocks++; return p;
}
static void kfree(void *p) {
    if (!p) return;
    if (free_hook) { void (*hook)(void) = free_hook; free_hook = NULL; hook(); }
    CHECK(blocks > 0, "double free"); blocks--; free(p);
}
static void chan_init(struct channel *c, void *b, u32 cap) { (void)cap; c->buf = b; }
static void spin_lock_init(int *p) { *p = 0; }
static void spin_lock(int *p) { CHECK(!*p, "recursive fixture lock"); *p = 1; }
static void spin_unlock(int *p) { CHECK(*p, "unheld fixture lock"); *p = 0; }
static void poll_waiter_list_init(int *p) { *p = 0; }
static void poll_waiter_list_wake(int *p) { (void)p; }
static void wakeup(int *p) { (void)p; }
static void srvconn_ctl_link(struct SrvConn *p) { (void)p; linked++; }
static void srvconn_ctl_unlink(struct SrvConn *p) { (void)p; CHECK(linked, "unlink imbalance"); linked--; }
static int t_atomic_fetch_add_relaxed_int(int *p, int n) { return __atomic_fetch_add(p, n, __ATOMIC_RELAXED); }
static int t_atomic_fetch_sub_acqrel_int(int *p, int n) { return __atomic_fetch_sub(p, n, __ATOMIC_ACQ_REL); }
'''

tests = r'''
static struct SrvConn *make(void) { return srvconn_create(1, 1, false, 2, SRVCONN_MSIZE); }
static void expect_full(void) {
    unsigned before = allocations;
    CHECK(make() == NULL, "in-flight or retained storage lost its reservation");
    CHECK(allocations == before, "capacity refusal allocated storage");
}
static void empty(void) {
    CHECK(blocks == 0 && linked == 0, "storage or diagnostic-list leak");
    CHECK(g_srvconn_created == g_srvconn_freed, "lifecycle counter imbalance");
    CHECK(g_srvconn_reserved == 0, "reservation leaked after rollback or destruction");
}
int main(void) {
    CHECK(srvconn_create(1, 1, false, 2, 0) == NULL, "invalid class admitted");
    CHECK(allocations == 0, "invalid class allocated"); empty();
    for (unsigned failure = 1; failure <= 3; failure++) {
        fail_at = allocations + failure;
        CHECK(make() == NULL, "injected allocation failure admitted");
        empty(); fail_at = 0;
        struct SrvConn *retry = make(); CHECK(retry, "rollback lost capacity");
        srvconn_unref(retry); empty();
    }
    struct SrvConn *held[SRV_MAX_CONNS];
    for (unsigned i = 0; i < SRV_MAX_CONNS - 1; i++) {
        held[i] = make(); CHECK(held[i], "capacity exhausted early");
    }
    // Reenter while the last constructor is between admission and allocation.
    // No threads or scheduler timing are needed to exercise this interval.
    allocation_hook = expect_full;
    held[SRV_MAX_CONNS - 1] = make();
    CHECK(held[SRV_MAX_CONNS - 1], "last slot refused"); expect_full();
    srvconn_ref(held[0]); srvconn_teardown(held[0]); srvconn_teardown(held[0]);
    srvconn_unref(held[0]); expect_full();
    // Reenter during final freeing: capacity may return only AFTER storage.
    free_hook = expect_full; srvconn_unref(held[0]);
    held[0] = srvconn_create(1, 1, false, 2, SRVCONN_BULK_MSIZE);
    CHECK(held[0], "final unref lost capacity"); expect_full();
    for (unsigned i = 0; i < SRV_MAX_CONNS; i++) srvconn_unref(held[i]);
    empty(); puts("PASS: allocation rollback, in-flight admission, retained teardown, final free");
}
'''

mutants = {
    "ignored-admission-refusal": (
        "if (!srvconn_reserve()) return NULL;", "(void)srvconn_reserve();",
        "in-flight or retained storage lost its reservation"),
    "missing-reservation": (
        "if (!srvconn_reserve()) return NULL;", "/* missing reservation */",
        "corrupt admission count"),
    "struct-rollback-leak": (
        "if (!cn) {\n        srvconn_unreserve();",
        "if (!cn) {", "reservation leaked after rollback"),
    "ring-rollback-leak": (
        "kfree(cn);\n        srvconn_unreserve();",
        "kfree(cn);", "reservation leaked after rollback"),
    "premature-release": (
        "srvconn_ctl_unlink(cn);",
        "srvconn_unreserve();\n    srvconn_ctl_unlink(cn);",
        "in-flight or retained storage lost its reservation"),
}

with tempfile.TemporaryDirectory(prefix="srvconn-admission-") as directory:
    path = Path(directory)
    for name in ["clean", *mutants]:
        actual = bodies
        if name != "clean":
            old, new, expected = mutants[name]
            assert actual.count(old) == 1, name
            actual = actual.replace(old, new)
            if name == "premature-release":
                tail = "__atomic_fetch_add(&g_srvconn_freed, 1u, __ATOMIC_RELAXED);\n    srvconn_unreserve();"
                assert actual.count(tail) == 1
                actual = actual.replace(tail, "__atomic_fetch_add(&g_srvconn_freed, 1u, __ATOMIC_RELAXED);")
        (path / "fixture.c").write_text("\n".join(constants) + "\n" + fixture + actual + tests)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-function", str(path / "fixture.c"),
                        "-o", str(path / "fixture")], check=True)
        result = subprocess.run([str(path / "fixture")], capture_output=True, text=True)
        if name == "clean":
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip(), flush=True)
        else:
            assert result.returncode != 0 and expected in result.stderr, (name, result)
            print(f"EXPECTED FAILURE {name}: {result.stderr.strip()}", flush=True)
