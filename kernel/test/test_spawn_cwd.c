// SYS_SPAWN_FULL_ARGV's cwd tail (STALK-DESIGN 4.3; dec-2026-10-06-spawn-cwd):
// the spawner resolves the child's cwd before any child exists, and the child
// is born with the landed name.
//
//   spawn_cwd.child_born_in_cwd    a held child's dot is the named cwd, absolute
//                                  or relative to the spawner's, and the
//                                  spawner's own dot does not move; with no
//                                  cwd the child inherits the spawner's
//   spawn_cwd.image_joins_child_cwd a relative image name is joined to the
//                                  child's cwd, not the spawner's
//   spawn_cwd.refusals             a bad cwd answers chdir's errno and makes no
//                                  child; no spawn moves the spawner's cwd
//
// The handler's half -- the tail read from user memory only when ext_flags
// announces it -- is joey's spawn-cwd probe, which drives the real ABI. The
// resolver's own errno classes over a fixture are stalk.dir_landed_name.

#include "test.h"

#include <thylacine/caps.h>
#include <thylacine/errno.h>
#include <thylacine/proc.h>
#include <thylacine/sched.h>
#include <thylacine/spoor.h>
#include <thylacine/syscall.h>
#include <thylacine/territory.h>
#include <thylacine/thread.h>
#include <thylacine/types.h>

struct spawn_allowance;
extern int sys_spawn_full_argv_cwd_for_proc(
        struct Proc *p, const char *name, size_t name_len,
        const char *argv_data, u32 argv_data_len, u32 argc,
        const u32 *fds, u32 fd_count,
        caps_t cap_mask, u32 perm_flags,
        bool set_identity, u32 principal_id, u32 primary_gid,
        const u32 *supp_gids, u32 supp_gid_count,
        const struct spawn_allowance *want_allowance,
        u32 req_budget, u32 pheno_flags, u32 debug_flags,
        const char *cwd, u32 cwd_len);
extern struct Spoor *exec_resolve_from_namespace(struct Proc *p, const char *name,
                                                 size_t name_len, size_t *size_out);

void test_spawn_cwd_child_born_in_cwd(void);
void test_spawn_cwd_image_joins_child_cwd(void);
void test_spawn_cwd_refusals(void);

#define SC_BUDGET_NS (6ull * 1000ull * 1000ull * 1000ull)

static u32 sc_len(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static bool sc_str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// A held spawn of `name` with cwd `cwd` (NULL: no tail). The child parks before
// its first instruction, so its dot is read with nothing of its own having run.
static int sc_spawn_held(const char *name, const char *cwd) {
    return sys_spawn_full_argv_cwd_for_proc(current_thread()->proc,
            name, sc_len(name), NULL, 0u, 0u, NULL, 0u, CAP_NONE, 0u,
            false, 0u, 0u, NULL, 0u, NULL, 0u, 0u, SPAWN_DEBUG_HELD,
            cwd, cwd ? sc_len(cwd) : 0u);
}

// Read the held child's cwd into `buf` ("" if it is gone).
static void sc_child_dot(int pid, char *buf, u64 cap) {
    buf[0] = '\0';
    struct Proc *c = (pid > 0) ? proc_find_by_pid(pid) : NULL;
    if (c && c->territory && territory_getdot(c->territory, buf, cap) < 0)
        buf[0] = '\0';
}

// Release the held child, as the owner's start verb does, and reap it within the
// budget; a child still unreaped then is killed. Returns the reaped pid, or <= 0.
static int sc_release_and_reap(int pid) {
    if (pid <= 0) return -1;
    struct Proc *c = proc_find_by_pid(pid);
    if (c) {
        irq_state_t s = proc_table_lock_acquire();
        proc_birth_hold_release_locked(c);
        proc_debug_resume(c);
        proc_table_lock_release(s);
    }
    u64 deadline = timer_now_ns() + SC_BUDGET_NS;
    for (int killed = 0;;) {
        int st = -1;
        int r = wait_pid_for(pid, WAIT_WNOHANG, &st);
        if (r != 0) return r;
        if (timer_now_ns() >= deadline) {
            if (killed) return 0;
            killed = 1;
            c = proc_find_by_pid(pid);
            irq_state_t s = proc_table_lock_acquire();
            if (c && c->state == PROC_STATE_ALIVE) proc_group_terminate(c, "killed");
            proc_table_lock_release(s);
            deadline = timer_now_ns() + SC_BUDGET_NS;
        }
        sched();
    }
}

// =============================================================================
// spawn_cwd.child_born_in_cwd
// =============================================================================

void test_spawn_cwd_child_born_in_cwd(void) {
    struct Proc *self = current_thread()->proc;
    char before[SYS_OPEN_PATH_MAX + 1], after[SYS_OPEN_PATH_MAX + 1];
    char d_abs[SYS_OPEN_PATH_MAX + 1], d_rel[SYS_OPEN_PATH_MAX + 1];
    char d_inh[SYS_OPEN_PATH_MAX + 1], saved[SYS_OPEN_PATH_MAX + 1];
    // The spawner stands in /bin for the test, whatever an earlier test left.
    int gs = territory_getdot(self->territory, saved, sizeof(saved));
    int sd = territory_setdot(self->territory, "/bin");
    int gb = territory_getdot(self->territory, before, sizeof(before));

    // Absolute: the root, which is not the spawner's dot.
    int p_abs = sc_spawn_held("/bin/hello", "/");
    sc_child_dot(p_abs, d_abs, sizeof(d_abs));
    int r_abs = sc_release_and_reap(p_abs);

    // Relative to the spawner's dot: "." from /bin is /bin. A resolver that
    // ignored the spawner's dot would answer "/", as for ".." it could not.
    int p_rel = sc_spawn_held("/bin/hello", ".");
    sc_child_dot(p_rel, d_rel, sizeof(d_rel));
    int r_rel = sc_release_and_reap(p_rel);

    // No tail: the child inherits the spawner's dot -- the control that the two
    // above saw the tail and not an inheritance that happened to match.
    int p_inh = sc_spawn_held("/bin/hello", NULL);
    sc_child_dot(p_inh, d_inh, sizeof(d_inh));
    int r_inh = sc_release_and_reap(p_inh);

    int ga = territory_getdot(self->territory, after, sizeof(after));
    if (gs > 0) (void)territory_setdot(self->territory, saved);

    TEST_ASSERT(gs > 0 && sd == 0 && gb > 0 && sc_str_eq(before, "/bin"),
                "premise: the spawner stands in /bin");
    TEST_ASSERT(p_abs > 0, "a held spawn with cwd \"/\" returned a child");
    TEST_ASSERT(sc_str_eq(d_abs, "/"), "the child is born in the absolute cwd \"/\"");
    TEST_EXPECT_EQ(r_abs, p_abs, "the released child was reaped");
    TEST_ASSERT(p_rel > 0, "a held spawn with cwd \".\" returned a child");
    TEST_ASSERT(sc_str_eq(d_rel, "/bin"),
                "a relative cwd is resolved from the spawner's: \".\" from /bin is \"/bin\"");
    TEST_EXPECT_EQ(r_rel, p_rel, "the second released child was reaped");
    TEST_ASSERT(p_inh > 0, "a held spawn with no cwd tail returned a child");
    TEST_ASSERT(sc_str_eq(d_inh, "/bin"), "with no tail the child inherits the spawner's cwd");
    TEST_EXPECT_EQ(r_inh, p_inh, "the third released child was reaped");
    TEST_ASSERT(ga > 0 && sc_str_eq(after, "/bin"), "the spawner's own cwd never moved");
}

// =============================================================================
// spawn_cwd.image_joins_child_cwd
// =============================================================================

void test_spawn_cwd_image_joins_child_cwd(void) {
    struct Proc *self = current_thread()->proc;
    char saved[SYS_OPEN_PATH_MAX + 1];
    size_t sz = 0;
    struct Spoor *at_root = exec_resolve_from_namespace(self, "/hello", 6, &sz);
    if (at_root) spoor_clunk(at_root);
    int gs = territory_getdot(self->territory, saved, sizeof(saved));

    // Each leg stands the spawner where a join to ITS cwd gives the other answer.
    // From /bin, "hello" names /bin/hello; joined to a child cwd of "/" it names
    // /hello, which does not exist.
    int sb = territory_setdot(self->territory, "/bin");
    int p_root = sc_spawn_held("hello", "/");
    int r_root = (p_root > 0) ? sc_release_and_reap(p_root) : p_root;
    // From "/", "hello" names the missing /hello; joined to a child cwd of "/bin"
    // it names /bin/hello.
    int sr = territory_setdot(self->territory, "/");
    int p_bin  = sc_spawn_held("hello", "/bin");
    int r_bin  = sc_release_and_reap(p_bin);
    if (gs > 0) (void)territory_setdot(self->territory, saved);

    TEST_ASSERT(at_root == NULL, "premise: no /hello at the root");
    TEST_ASSERT(gs > 0 && sb == 0 && sr == 0, "premise: the spawner stood in /bin, then in /");
    TEST_EXPECT_EQ(p_root, -1,
        "a relative image is joined to the child's cwd: \"hello\" under \"/\" is not found");
    (void)r_root;
    TEST_ASSERT(p_bin > 0, "the same name under the child cwd \"/bin\" spawns /bin/hello");
    TEST_EXPECT_EQ(r_bin, p_bin, "the /bin/hello child was reaped");
}

// =============================================================================
// spawn_cwd.refusals
// =============================================================================

void test_spawn_cwd_refusals(void) {
    struct Proc *self = current_thread()->proc;
    char before[SYS_OPEN_PATH_MAX + 1], after[SYS_OPEN_PATH_MAX + 1];
    int gb = territory_getdot(self->territory, before, sizeof(before));
    // Stand where no spawn below names: the harness's kproc stands in /bin
    // (joey_root_kproc_at_devramfs), the control's cwd, and a spawn that moved
    // the spawner there would look like one that left it alone.
    int sr = territory_setdot(self->territory, "/");

    int nf = sc_spawn_held("/bin/hello", "/no-such-cwd");
    int nd = sc_spawn_held("/bin/hello", "/bin/hello");
    int ok = sc_spawn_held("/bin/hello", "/bin");
    int r_ok = sc_release_and_reap(ok);
    // A tail with an empty path (the handler refuses cwd_len 0 before this; the
    // entry refuses it on its own).
    int em = sys_spawn_full_argv_cwd_for_proc(self, "/bin/hello", 10, NULL, 0u, 0u,
            NULL, 0u, CAP_NONE, 0u, false, 0u, 0u, NULL, 0u, NULL, 0u, 0u,
            SPAWN_DEBUG_HELD, "", 0u);
    int r_nf = (nf > 0) ? sc_release_and_reap(nf) : 0;
    int r_nd = (nd > 0) ? sc_release_and_reap(nd) : 0;
    int r_em = (em > 0) ? sc_release_and_reap(em) : 0;
    int ga = territory_getdot(self->territory, after, sizeof(after));
    if (gb > 0) (void)territory_setdot(self->territory, before);

    TEST_ASSERT(gb > 0 && sr == 0, "premise: the spawner stood in /, which no spawn names");
    TEST_EXPECT_EQ(nf, -(int)T_E_NOENT, "a missing cwd fails the spawn with ENOENT");
    TEST_EXPECT_EQ(nd, -(int)T_E_NOTDIR, "a cwd naming a file fails the spawn with ENOTDIR");
    TEST_EXPECT_EQ(em, -(int)T_E_INVAL, "an empty cwd fails the spawn with EINVAL");
    TEST_ASSERT(ok > 0, "the control: an existing directory spawns");
    TEST_EXPECT_EQ(r_ok, ok, "the control child was reaped");
    TEST_EXPECT_EQ(r_nf + r_nd + r_em, 0, "no refused spawn left a child to reap");
    TEST_ASSERT(ga > 0 && sc_str_eq(after, "/"),
                "no spawn, refused or not, moved the spawner's cwd");
}
