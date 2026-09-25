// /pouch-hello-dlopen -- the B-1d prover: a Pouch program loads a Pouch-built
// .so on the device (ARCHITECTURE.md sec 6.5 "Dynamic loading"; DISTRO D-3 and
// D-4; patches 0047 and 0048). It is a dynamic PIE, so before main exec ran its
// PT_INTERP, /lib/libc.so, which mapped this program through
// SYS_BURROW_MAP_FILE.
//
// Legs. Each runs even after an earlier one failed; the census line (what
// joey matches) prints only when all of them passed.
//   interp    this program's loader is /lib/libc.so.
//   noexec    DENY: in a child whose namespace marks /lib's device MNOEXEC,
//             the plugin does not load, and the reason is EACCES (the vouching
//             rule on its text window; natively not EPERM, which the kernel
//             cannot return: errno.h), not a failed lookup.
//   confined  DENY: in a child pivoted to a root with no /lib, neither the bare
//             name, the absolute path, nor a ../ escape finds the plugin
//             (ENOENT): the loader resolves names in the Proc's own namespace.
//             The child works from / on both sides of the pivot, so the
//             relative name's .. meets the resolver's floor: before the
//             pivot the same name opens the plugin, and after it ".." alone
//             opens the new root (the leg's two controls).
//   load      the CONTROL for both: here, dlopen("libdlprobe.so") finds
//             /lib/libdlprobe.so by bare name, and its constructor has run.
//   segments  the plugin has the four PT_LOADs (R, R+X, RW, RW), a RELRO range
//             and a bss tail past its file bytes, and its rodata, text, data
//             and bss each read and take writes as their shape promises.
//   relro     a child that writes the plugin's RELRO table dies of the fault;
//             one that writes its .data exits 0.
// Every child is this binary, spawned through posix_spawn: a dynamic program
// spawning a dynamic program.
//
// Output stays inside joey's 2048-byte capture window.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <thyla/syscall.h>
#include <unistd.h>

#define PLUGIN     "libdlprobe.so"
#define GREETING   "libdlprobe: loaded"
#define PG         4096ul

extern char **environ;

static int g_fails;
static const char *g_self = "pouch-hello-dlopen";

static void ok(const char *leg) {
    printf("pouch-hello-dlopen: %s ok\n", leg);
}

static void bad(const char *leg, const char *what) {
    printf("pouch-hello-dlopen: %s FAIL: %s\n", leg, what);
    g_fails++;
}

#define CHECK(cond, what) do { if (!(cond)) { bad(LEG, what); return; } } while (0)

// ---- the children ---------------------------------------------------------
// Exit codes: 0 the leg's expectation held; 1 is the kernel's, a fault (v1.0
// exit status); 2 and up, the child's own named failures.

static int dlerror_says(int err) {
    const char *e = dlerror();
    if (e && strstr(e, strerror(err))) return 1;
    printf("pouch-hello-dlopen:   dlerror: %s\n", e ? e : "(none)");
    return 0;
}

static int child_noexec(void) {
    // Graft /lib's directory at /proc, MNOEXEC. The flag binds the DEVICE
    // instance, so every file of it -- the plugin under /lib included --
    // stops being executable in this namespace; /lib itself is untouched.
    int d = open("/lib", O_RDONLY);
    if (d < 0) return 2;
    if (syscall(T_SYS_MOUNT, "/proc", 5L, (long)d, (long)(T_MREPL | T_MNOEXEC)) != 0)
        return 3;
    if (dlopen(PLUGIN, RTLD_NOW)) return 4;
    return dlerror_says(EACCES) ? 0 : 5;
}

static int child_confined(void) {
    // The prover inherits joey's /bin. From there "../lib/..." would miss at
    // "bin" in the new root, before its ".." is ever walked.
    if (chdir("/") != 0) return 6;
    int c = open("../lib/" PLUGIN, O_RDONLY);
    if (c < 0) return 7;
    close(c);
    // /srv is a directory in every boot namespace and holds no /lib.
    int d = open("/srv", O_RDONLY);
    if (d < 0) return 2;
    if (syscall(T_SYS_PIVOT_ROOT, (long)d) != 0) return 3;
    // ".." opens the new root, so the ../ name below meets the floor rather
    // than missing at a "bin" this root does not have.
    int up = open("..", O_RDONLY);
    if (up < 0) return 8;
    close(up);
    static const char *const names[] = {
        PLUGIN, "/lib/" PLUGIN, "../lib/" PLUGIN,
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (dlopen(names[i], RTLD_NOW)) return 4;
        if (!dlerror_says(ENOENT)) return 5;
    }
    return 0;
}

static int child_write(int relro) {
    void *h = dlopen(PLUGIN, RTLD_NOW);
    if (!h) return 2;
    if (relro) {
        const char *volatile *t = dlsym(h, "dlprobe_table");
        if (!t) return 3;
        t[0] = GREETING;        // a RELRO page: the kernel ends this process here
        return 4;
    }
    volatile int *s = dlsym(h, "dlprobe_seed");
    if (!s) return 3;
    *s = 7;
    return *s == 7 ? 0 : 5;
}

// Spawn this binary as `role`; its exit status, or -1 if it did not run.
static int run_child(const char *role) {
    char *argv[] = { (char *)g_self, (char *)role, NULL };
    pid_t pid;
    if (posix_spawn(&pid, g_self, NULL, NULL, argv, environ) != 0) return -1;
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

// ---- the parent's legs ----------------------------------------------------

struct layout {
    int seen_libc, found;
    unsigned nload, shape_ok, tail;
    uintptr_t relro_lo, relro_hi;
};

static int visit(struct dl_phdr_info *info, size_t size, void *arg) {
    (void)size;
    struct layout *l = arg;
    const char *n = info->dlpi_name ? info->dlpi_name : "";
    if (!strcmp(n, "/lib/libc.so")) l->seen_libc = 1;
    size_t nl = strlen(n), pl = strlen(PLUGIN);
    if (nl < pl || strcmp(n + nl - pl, PLUGIN)) return 0;
    l->found = 1;
    static const unsigned want[4] = { PF_R, PF_R | PF_X, PF_R | PF_W, PF_R | PF_W };
    l->shape_ok = 1;
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_GNU_RELRO) {
            l->relro_lo = info->dlpi_addr + ph->p_vaddr;
            l->relro_hi = l->relro_lo + ph->p_memsz;
        }
        if (ph->p_type != PT_LOAD) continue;
        if (l->nload >= 4 || (ph->p_flags & (PF_R | PF_W | PF_X)) != want[l->nload])
            l->shape_ok = 0;
        l->nload++;
        // The loader maps [end of the file bytes' page, end of the segment)
        // anonymously only when that range is not empty.
        uintptr_t fend = (ph->p_vaddr + ph->p_filesz + PG - 1) & ~(PG - 1);
        uintptr_t mend = (ph->p_vaddr + ph->p_memsz + PG - 1) & ~(PG - 1);
        if ((ph->p_flags & PF_W) && mend > fend) l->tail = 1;
    }
    return 0;
}

static void leg_interp(void) {
#define LEG "interp"
    struct layout l = {0};
    dl_iterate_phdr(visit, &l);
    CHECK(l.seen_libc, "no loaded object is /lib/libc.so");
    ok(LEG);
#undef LEG
}

static void leg_child(const char *leg, const char *role, int want) {
    int st = run_child(role);
    if (st == want) { ok(leg); return; }
    char what[64];
    snprintf(what, sizeof what, "child %s exited %d, want %d", role, st, want);
    bad(leg, what);
}

static void *g_h;

static void leg_load(void) {
#define LEG "load"
    g_h = dlopen(PLUGIN, RTLD_NOW);
    CHECK(g_h, dlerror());
    int *inited = dlsym(g_h, "dlprobe_inited");
    CHECK(inited && *inited == 1, "the constructor did not run");
    ok(LEG);
#undef LEG
}

static void leg_segments(void) {
#define LEG "segments"
    CHECK(g_h, "not loaded");
    struct layout l = {0};
    dl_iterate_phdr(visit, &l);
    CHECK(l.found, "the plugin is not among the loaded objects");
    CHECK(l.nload == 4 && l.shape_ok, "the plugin is not the R, R+X, RW, RW layout");
    CHECK(l.relro_hi > l.relro_lo, "the plugin has no RELRO range");
    CHECK(l.tail, "no writable segment runs past its file bytes");

    const char *(*greeting)(void) = (const char *(*)(void))dlsym(g_h, "dlprobe_greeting");
    int (*add)(int, int) = (int (*)(int, int))dlsym(g_h, "dlprobe_add");
    unsigned long (*len)(const char *) = (unsigned long (*)(const char *))dlsym(g_h, "dlprobe_len");
    int (*bss_zero)(void) = (int (*)(void))dlsym(g_h, "dlprobe_bss_zero");
    const char *const *table = dlsym(g_h, "dlprobe_table");
    volatile int *seed = dlsym(g_h, "dlprobe_seed");
    CHECK(greeting && add && len && bss_zero && table && seed, "an export is missing");
    CHECK(!strcmp(greeting(), GREETING), "the rodata did not read back");
    CHECK(add(2, 3) == 5, "the text did not run");
    CHECK(len("abcd") == 4, "the call into libc.so did not return");
    CHECK(table[0] == greeting(), "the RELRO table was not relocated");
    uintptr_t ta = (uintptr_t)table;
    CHECK(ta >= l.relro_lo && ta < l.relro_hi, "the table is not in the RELRO range");
    CHECK(*seed == 0x5eed1d, "the data copy lost its file bytes");
    *seed = 9;
    CHECK(*seed == 9, "the data copy refused a write");
    CHECK(bss_zero() == 1, "the bss did not read zero");
    CHECK(bss_zero() == 0, "the bss did not take a write");
    ok(LEG);
#undef LEG
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 0 && argv[0] && argv[0][0]) g_self = argv[0];
    if (argc > 1) {
        if (!strcmp(argv[1], "noexec"))      return child_noexec();
        if (!strcmp(argv[1], "confined"))    return child_confined();
        if (!strcmp(argv[1], "relro-write")) return child_write(1);
        if (!strcmp(argv[1], "data-write"))  return child_write(0);
        return 99;
    }
    leg_interp();
    // The deny legs come BEFORE this process loads the plugin, and run in
    // children: musl keeps a loaded object by name, and a child spawned
    // afterwards inherits nothing, but the parent's own lookups would hit it.
    leg_child("noexec", "noexec", 0);
    leg_child("confined", "confined", 0);
    leg_load();
    leg_segments();
    leg_child("relro-data", "data-write", 0);
    leg_child("relro", "relro-write", 1);
    if (g_fails) {
        printf("pouch-hello-dlopen: %d leg(s) FAILED\n", g_fails);
        return 1;
    }
    puts(POUCH_CENSUS_DLOPEN);
    return 0;
}
