// SYS_SPAWN_FULL_ARGV's environment tail (ARCH 9.7; dec-2026-10-28-spawn-env):
// the spawner builds the child's environment before any child exists, and the
// child installs it before exec projects it.
//
//   spawn_env.block_parse        env_from_block: the records become the group,
//                                a later record replaces an earlier one, and
//                                each malformed or oversized block answers its
//                                errno
//   spawn_env.install_refills    env_install empties and refills an existing
//                                group in place (same struct, same devno, fresh
//                                ids) and publishes into a Proc with none
//   spawn_env.child_gets_block   a held child's /env is exactly the block; with
//                                no tail it is a copy of the spawner's; an empty
//                                block is an empty environment; the spawner's
//                                own /env never changes
//   spawn_env.refusals           a refused block makes no child and leaves the
//                                spawner's /env alone
//
// The handler's half -- the tail read from user memory only when ext_flags
// announces it, at the offset the earlier tails leave -- is joey's spawn-env
// probe, which drives the real ABI.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/env.h>
#include <thylacine/errno.h>
#include <thylacine/notes.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>
#include <thylacine/exec.h>

#include "../../mm/slub.h"          // kmalloc: the over-long value

#define SE_BUDGET_NS (6ull * 1000ull * 1000ull * 1000ull)
#define SE_RENDER_MAX 512

static struct Proc *semk(void) { return proc_alloc(); }
static void sedrop(struct Proc *p) { if (!p) return; p->state = PROC_STATE_ZOMBIE; proc_free(p); }

static bool se_bytes_eq(const char *a, const char *b, long n) {
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

// env_from_block over `len` bytes of `block`, installed into a fresh Proc and
// rendered back into `out`. The rendered length, or -1 when the block is
// refused (its errno in *err).
static long se_parse_render(const char *block, u32 len, int *err, char *out) {
    *err = 0;
    struct Env *e = env_from_block(block, len, err);
    if (!e) return -1;
    struct Proc *p = semk();
    if (!p) { env_discard(e); return -2; }
    env_install(p, e);
    long n = env_render_environ(p, 0, out, SE_RENDER_MAX);
    sedrop(p);
    return n;
}

// =============================================================================
// spawn_env.block_parse
// =============================================================================

// n records "Vk=1" for k = 0..n-1 into buf; the block's length.
static u32 se_many(char *buf, int n) {
    u32 o = 0;
    for (int k = 0; k < n; k++) {
        buf[o++] = 'V';
        if (k >= 10) buf[o++] = (char)('0' + k / 10);
        buf[o++] = (char)('0' + k % 10);
        buf[o++] = '=';
        buf[o++] = '1';
        buf[o++] = '\0';
    }
    return o;
}

void test_spawn_env_block_parse(void) {
    char out[SE_RENDER_MAX];
    int err;

    static const char two[] = "A=1\0BB=22\0";
    long n = se_parse_render(two, sizeof(two) - 1, &err, out);
    TEST_EXPECT_EQ(n, (long)sizeof(two) - 1, "two records render back whole");
    TEST_ASSERT(n > 0 && se_bytes_eq(out, two, n), "in record order");

    n = se_parse_render(NULL, 0, &err, out);
    TEST_EXPECT_EQ(n, 0L, "an empty block is an empty environment, not a refusal");

    static const char dup[] = "A=1\0B=2\0A=3\0";
    static const char dup_want[] = "A=3\0B=2\0";
    n = se_parse_render(dup, sizeof(dup) - 1, &err, out);
    TEST_EXPECT_EQ(n, (long)sizeof(dup_want) - 1, "a repeated name is one entry");
    TEST_ASSERT(n > 0 && se_bytes_eq(out, dup_want, n),
                "the later value wins, in the earlier record's place");

    static const char eqv[] = "A=b=c\0E=\0";
    n = se_parse_render(eqv, sizeof(eqv) - 1, &err, out);
    TEST_EXPECT_EQ(n, (long)sizeof(eqv) - 1, "a value may hold '=' and may be empty");
    TEST_ASSERT(n > 0 && se_bytes_eq(out, eqv, n), "first '=' ends the name");

    // Each refusal below is one variable away from an accepted control.
    static const char noeq[] = "A\0";
    TEST_EXPECT_EQ(se_parse_render(noeq, sizeof(noeq) - 1, &err, out), -1L, "no '=' refused");
    TEST_EXPECT_EQ(err, -(int)T_E_INVAL, "no '=' -> EINVAL");
    static const char noname[] = "=1\0";
    TEST_EXPECT_EQ(se_parse_render(noname, sizeof(noname) - 1, &err, out), -1L, "empty name refused");
    TEST_EXPECT_EQ(err, -(int)T_E_INVAL, "empty name -> EINVAL");
    static const char slash[] = "A/B=1\0";
    TEST_EXPECT_EQ(se_parse_render(slash, sizeof(slash) - 1, &err, out), -1L, "'/' in a name refused");
    TEST_EXPECT_EQ(err, -(int)T_E_INVAL, "'/' in a name -> EINVAL");
    static const char unterm[] = "A=1";
    TEST_EXPECT_EQ(se_parse_render(unterm, sizeof(unterm) - 1, &err, out), -1L, "unterminated refused");
    TEST_EXPECT_EQ(err, -(int)T_E_INVAL, "unterminated -> EINVAL");
    TEST_EXPECT_EQ(se_parse_render(NULL, 4, &err, out), -1L, "NULL block with a length refused");
    TEST_EXPECT_EQ(err, -(int)T_E_INVAL, "NULL block -> EINVAL");

    // Name length: ENV_NAME_MAX - 1 bytes fit (the name keeps its NUL); one more
    // does not.
    char nm[ENV_NAME_MAX + 4];
    for (u32 k = 0; k < ENV_NAME_MAX - 1; k++) nm[k] = 'N';
    nm[ENV_NAME_MAX - 1] = '=';
    nm[ENV_NAME_MAX] = '\0';
    n = se_parse_render(nm, ENV_NAME_MAX + 1, &err, out);
    TEST_EXPECT_EQ(n, (long)ENV_NAME_MAX + 1, "a name of ENV_NAME_MAX - 1 bytes is accepted");
    for (u32 k = 0; k < ENV_NAME_MAX; k++) nm[k] = 'N';
    nm[ENV_NAME_MAX] = '=';
    nm[ENV_NAME_MAX + 1] = '\0';
    TEST_EXPECT_EQ(se_parse_render(nm, ENV_NAME_MAX + 2, &err, out), -1L,
                   "a name of ENV_NAME_MAX bytes is refused");
    TEST_EXPECT_EQ(err, -(int)T_E_INVAL, "an over-long name -> EINVAL");

    // Value length: ENV_VALUE_MAX fits, one more is E2BIG. (Rendered into no
    // buffer: the accepted block is checked by its parse alone.)
    char *big = kmalloc(ENV_VALUE_MAX + 8, 0);
    TEST_ASSERT(big != NULL, "premise: the value buffer");
    big[0] = 'B'; big[1] = '=';
    for (u32 k = 0; k < ENV_VALUE_MAX; k++) big[2 + k] = 'v';
    big[2 + ENV_VALUE_MAX] = '\0';
    struct Env *ok = env_from_block(big, ENV_VALUE_MAX + 3, &err);
    TEST_ASSERT(ok != NULL, "a value of ENV_VALUE_MAX bytes is accepted");
    env_discard(ok);
    big[2 + ENV_VALUE_MAX] = 'v';
    big[3 + ENV_VALUE_MAX] = '\0';
    TEST_ASSERT(env_from_block(big, ENV_VALUE_MAX + 4, &err) == NULL,
                "a value of ENV_VALUE_MAX + 1 bytes is refused");
    TEST_EXPECT_EQ(err, -(int)T_E_2BIG, "an over-long value -> E2BIG");
    kfree(big);

    // Entry count: ENV_MAX_ENTRIES names fit, a duplicate among them is no new
    // entry, and one more name is E2BIG.
    char many[(ENV_MAX_ENTRIES + 2) * 8];
    u32 ml = se_many(many, ENV_MAX_ENTRIES);
    ok = env_from_block(many, ml, &err);
    TEST_ASSERT(ok != NULL, "ENV_MAX_ENTRIES names are accepted");
    env_discard(ok);
    static const char again[] = "V0=2";
    for (u32 k = 0; k < sizeof(again); k++) many[ml + k] = again[k];
    ok = env_from_block(many, ml + sizeof(again), &err);
    TEST_ASSERT(ok != NULL, "a repeat of a name already present is no new entry");
    env_discard(ok);
    ml = se_many(many, ENV_MAX_ENTRIES + 1);
    TEST_ASSERT(env_from_block(many, ml, &err) == NULL, "ENV_MAX_ENTRIES + 1 names are refused");
    TEST_EXPECT_EQ(err, -(int)T_E_2BIG, "one name too many -> E2BIG");
}

// =============================================================================
// spawn_env.install_refills
// =============================================================================

void test_spawn_env_install_refills(void) {
    char out[SE_RENDER_MAX];
    int err;
    struct Proc *p = semk();
    TEST_ASSERT(p != NULL, "proc_alloc");
    u64 a = env_create(p, "A", 1);
    u64 z = env_create(p, "Z", 1);
    TEST_ASSERT(a != 0 && z != 0, "premise: an existing group A, Z");
    (void)env_write(p, a, 0, "old", 3);
    (void)env_write(p, z, 0, "zz", 2);
    struct Env *before = p->env;
    u64 devno = env_devno(p);
    u64 last_id = before->next_id - 1;

    static const char blk[] = "B=2\0A=1\0";
    struct Env *e = env_from_block(blk, sizeof(blk) - 1, &err);
    TEST_ASSERT(e != NULL, "premise: the block parses");
    env_install(p, e);

    long n = env_render_environ(p, 0, out, SE_RENDER_MAX);
    TEST_EXPECT_EQ(n, (long)sizeof(blk) - 1, "the group is exactly the block");
    TEST_ASSERT(n > 0 && se_bytes_eq(out, blk, n), "in the block's order; Z is gone");
    TEST_ASSERT(p->env == before, "the same Env struct: refilled, never swapped");
    TEST_EXPECT_EQ(env_devno(p), devno, "the group keeps its device number");
    TEST_EXPECT_EQ(p->env->count, 2, "two live entries");
    TEST_ASSERT(env_lookup(p, "A", 1) > last_id && env_lookup(p, "B", 1) > last_id,
                "new entries take ids past every id the group issued");
    TEST_EXPECT_EQ(env_lookup(p, "Z", 1), (u64)0, "the old entry is gone by name");
    char v[8];
    TEST_EXPECT_EQ(env_read(p, a, 0, v, sizeof(v)), -1L,
                   "the old A's id no longer resolves (monotonic ids)");
    sedrop(p);

    // A Proc with no group adopts the built one.
    struct Proc *q = semk();
    TEST_ASSERT(q != NULL && q->env == NULL, "premise: a Proc with no group");
    e = env_from_block(blk, sizeof(blk) - 1, &err);
    TEST_ASSERT(e != NULL, "premise: the block parses again");
    env_install(q, e);
    TEST_ASSERT(q->env == e, "the built group is published as the Proc's");
    n = env_render_environ(q, 0, out, SE_RENDER_MAX);
    TEST_EXPECT_EQ(n, (long)sizeof(blk) - 1, "and holds the block");
    sedrop(q);
}

// =============================================================================
// spawn_env.child_gets_block and spawn_env.refusals
// =============================================================================

// A held spawn of /bin/hello with an environment tail (env_set false: none). The
// child parks before its first instruction, after its thunk installed the group
// and exec projected it.
static int se_spawn_held(bool env_set, const char *blk, u32 len) {
    return sys_spawn_full_argv_env_for_proc(current_thread()->proc,
            "/bin/hello", 10, NULL, 0u, 0u, NULL, 0u, CAP_NONE, 0u,
            false, 0u, 0u, NULL, 0u, NULL, 0u, 0u, SPAWN_DEBUG_HELD,
            NULL, 0u, 0u, env_set, blk, len);
}

// The held child's environment rendered into out; -1 when it is gone.
static long se_child_env(int pid, char *out) {
    struct Proc *c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    return c ? env_render_environ(c, 0, out, SE_RENDER_MAX) : -1;
}

// Release the held child and reap it within the budget; one still unreaped then
// is killed. The reaped pid, or <= 0.
static int se_release_and_reap(int pid) {
    if (pid <= 0) return -1;
    struct Proc *c = proc_find_by_pid(pid);
    if (c) {
        irq_state_t s = proc_table_lock_acquire();
        proc_birth_hold_release_locked(c);
        proc_debug_resume(c);
        proc_table_lock_release(s);
    }
    u64 deadline = timer_now_ns() + SE_BUDGET_NS;
    for (int killed = 0;;) {
        int st = -1;
        int r = wait_pid_for(pid, WAIT_WNOHANG, &st);
        if (r != 0) return r;
        if (timer_now_ns() >= deadline) {
            if (killed) return 0;
            killed = 1;
            c = proc_find_by_pid(pid);
            irq_state_t s = proc_table_lock_acquire();
            if (c && c->state == PROC_STATE_ALIVE) proc_group_terminate(c, NOTE_SIG_KILL, "killed");
            proc_table_lock_release(s);
            deadline = timer_now_ns() + SE_BUDGET_NS;
        }
        sched();
    }
}

// Give the spawner a variable of its own, so "the child has the spawner's
// environment" and "the child has the block" cannot read alike. Its id, or 0.
static u64 se_spawner_mark(struct Proc *self) {
    u64 id = env_create(self, "SE_SPAWNER", 10);
    if (id != 0 && env_write(self, id, 0, "here", 4) != 4) return 0;
    return id;
}

void test_spawn_env_child_gets_block(void) {
    struct Proc *self = current_thread()->proc;
    char mine[SE_RENDER_MAX], mine_after[SE_RENDER_MAX];
    char c_blk[SE_RENDER_MAX], c_inh[SE_RENDER_MAX], c_emp[SE_RENDER_MAX], c_dup[SE_RENDER_MAX];
    u64 mark = se_spawner_mark(self);
    long ml = env_render_environ(self, 0, mine, SE_RENDER_MAX);

    static const char blk[] = "SE_A=1\0SE_B=two\0";
    int p_blk = se_spawn_held(true, blk, sizeof(blk) - 1);
    long n_blk = se_child_env(p_blk, c_blk);
    int r_blk = se_release_and_reap(p_blk);

    int p_inh = se_spawn_held(false, NULL, 0u);
    long n_inh = se_child_env(p_inh, c_inh);
    int r_inh = se_release_and_reap(p_inh);

    int p_emp = se_spawn_held(true, NULL, 0u);
    long n_emp = se_child_env(p_emp, c_emp);
    int r_emp = se_release_and_reap(p_emp);

    static const char dup[] = "SE_A=1\0SE_A=2\0";
    static const char dup_want[] = "SE_A=2\0";
    int p_dup = se_spawn_held(true, dup, sizeof(dup) - 1);
    long n_dup = se_child_env(p_dup, c_dup);
    int r_dup = se_release_and_reap(p_dup);

    long ma = env_render_environ(self, 0, mine_after, SE_RENDER_MAX);
    if (mark) (void)env_unset(self, "SE_SPAWNER", 10);

    TEST_ASSERT(mark != 0 && ml > 0, "premise: the spawner holds SE_SPAWNER=here");
    TEST_ASSERT(p_blk > 0, "a spawn with a block makes a child");
    TEST_EXPECT_EQ(n_blk, (long)sizeof(blk) - 1, "the child's environment is the block's size");
    TEST_ASSERT(n_blk > 0 && se_bytes_eq(c_blk, blk, n_blk),
                "and exactly the block: nothing of the spawner's carried over");
    TEST_ASSERT(p_inh > 0, "the control: a spawn with no tail makes a child");
    TEST_EXPECT_EQ(n_inh, ml, "with no tail the child's environment is the spawner's size");
    TEST_ASSERT(n_inh > 0 && se_bytes_eq(c_inh, mine, n_inh), "and a copy of it");
    TEST_ASSERT(p_emp > 0, "an empty block makes a child");
    TEST_EXPECT_EQ(n_emp, 0L, "whose environment is empty");
    TEST_ASSERT(p_dup > 0, "a block repeating a name makes a child");
    TEST_EXPECT_EQ(n_dup, (long)sizeof(dup_want) - 1, "holding the name once");
    TEST_ASSERT(n_dup > 0 && se_bytes_eq(c_dup, dup_want, n_dup), "with the later value");
    TEST_EXPECT_EQ(r_blk + r_inh + r_emp + r_dup, p_blk + p_inh + p_emp + p_dup,
                   "every child was reaped");
    TEST_ASSERT(ma == ml && se_bytes_eq(mine_after, mine, ml),
                "no spawn changed the spawner's own environment");
}

void test_spawn_env_refusals(void) {
    struct Proc *self = current_thread()->proc;
    char mine[SE_RENDER_MAX], mine_after[SE_RENDER_MAX];
    u64 mark = se_spawner_mark(self);
    long ml = env_render_environ(self, 0, mine, SE_RENDER_MAX);

    static const char noeq[] = "SE_A\0";
    static const char slash[] = "SE/A=1\0";
    static const char unterm[] = "SE_A=1";
    int r_noeq   = se_spawn_held(true, noeq, sizeof(noeq) - 1);
    int r_slash  = se_spawn_held(true, slash, sizeof(slash) - 1);
    int r_unterm = se_spawn_held(true, unterm, sizeof(unterm) - 1);
    // Past the data bound: refused on the length alone, before the block is read.
    static const char tiny[] = "SE_A=1\0";
    int r_big    = se_spawn_held(true, tiny, SYS_SPAWN_ENV_DATA_MAX + 1u);
    // The control, one variable away: the same tiny block at its true length.
    int ok       = se_spawn_held(true, tiny, sizeof(tiny) - 1);
    int r_ok     = se_release_and_reap(ok);

    int leaked = 0;
    int refused[4] = { r_noeq, r_slash, r_unterm, r_big };
    for (int i = 0; i < 4; i++)
        if (refused[i] > 0) { (void)se_release_and_reap(refused[i]); leaked++; }

    long ma = env_render_environ(self, 0, mine_after, SE_RENDER_MAX);
    if (mark) (void)env_unset(self, "SE_SPAWNER", 10);

    TEST_ASSERT(mark != 0 && ml > 0, "premise: the spawner holds SE_SPAWNER=here");
    TEST_EXPECT_EQ(r_noeq, -(int)T_E_INVAL, "a record with no '=' fails the spawn with EINVAL");
    TEST_EXPECT_EQ(r_slash, -(int)T_E_INVAL, "a name with '/' fails the spawn with EINVAL");
    TEST_EXPECT_EQ(r_unterm, -(int)T_E_INVAL, "an unterminated block fails the spawn with EINVAL");
    TEST_EXPECT_EQ(r_big, -(int)T_E_2BIG, "a block past SYS_SPAWN_ENV_DATA_MAX fails with E2BIG");
    TEST_ASSERT(ok > 0, "the control: the same block at its length spawns");
    TEST_EXPECT_EQ(r_ok, ok, "the control child was reaped");
    TEST_EXPECT_EQ(leaked, 0, "no refused spawn left a child to reap");
    TEST_ASSERT(ma == ml && se_bytes_eq(mine_after, mine, ml),
                "no spawn, refused or not, changed the spawner's environment");
}
