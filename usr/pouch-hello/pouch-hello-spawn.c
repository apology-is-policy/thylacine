// /pouch-hello-spawn -- the CL-1b core process-lifecycle prover (Clade arc,
// docs/LLVM-DESIGN.md). Exercises the full toolchain spawn/reap path end to
// end through the POSIX API (patch 0026): pipe2 -> posix_spawn with a stdout
// redirect file_action -> the child runs and writes -> the parent captures its
// output over the pipe -> waitpid decodes WIFEXITED/WEXITSTATUS. This is the
// exact shape the clang driver uses to run cc1/lld (posix_spawn one child,
// wait4 it), so a regression in the static file_actions resolver, the spawn
// fd_list mapping, or the wait status translation surfaces as a labelled FAIL.
//
// The binary is BOTH the parent and the child (self-respawn via argv[0]):
//   no args        -> PARENT: runs the test battery.
//   argv[1]=="ok"  -> CHILD:  writes "SPAWNCHILD-OK\n" to fd 1, exit 0.
//   argv[1]=="fail"-> CHILD:  exit 3 (#91: the child's real exit byte survives
//                             to the parent -> WEXITSTATUS == 3, no collapse).
//   argv[1]=="envchk"   -> CHILD:  exit with one bit per variable it holds.
//   argv[1]=="envcount" -> CHILD:  exit with its environment's entry count.
// The child reading its own argv[1] also proves argv pass-through end to end.
//
// On success: "pouch-hello-spawn: ... ALL SPAWN TESTS PASS" + exit 0. Any
// failure prints a labelled FAIL + non-zero exit so joey's reap sees it.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

static int fail(const char *why, int extra) {
	printf("pouch-hello-spawn: %s FAIL (errno=%d, x=%d)\n", why, errno, extra);
	return 1;
}

// envchk's bits: what the child's environment holds.
#define ENV_MARK   1    // PHS_MARK=rich
#define ENV_DUP    2    // PHS_DUP=last (a later record replaces an earlier one)
#define ENV_PARENT 4    // PHS_PARENT, in the parent's /env but not its environ
#define ENV_SET    8    // PHS_SET, which the parent only setenv'd

// One env leg's verdict: a wrong leg prints its FAIL and the run goes on, so
// every leg a regression breaks shows in one boot.
static int env_leg(int ok, const char *what, int got, int rc) {
	if (ok) return 0;
	errno = rc;
	fail(what, got);
	return 1;
}

// Spawn self in `mode` with `envp`; the child's exit byte, or -1 with *rc set
// when posix_spawn refused, or -2 when the reap went wrong.
static int env_child(const char *self, const char *mode, char *const envp[],
                     int *rc) {
	char *av[] = { (char *)self, (char *)mode, NULL };
	pid_t pid = -1;
	*rc = posix_spawn(&pid, self, NULL, NULL, av, envp);
	if (*rc != 0) return -1;
	int st = 0;
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return -2;
	return WEXITSTATUS(st);
}

int main(int argc, char **argv) {
	// --- CHILD paths (self-respawn) ---
	if (argc >= 2) {
		if (!strcmp(argv[1], "ok")) {
			static const char msg[] = "SPAWNCHILD-OK\n";
			(void)write(1, msg, sizeof msg - 1);
			return 0;
		}
		if (!strcmp(argv[1], "fail"))
			return 3;   // -> WEXITSTATUS 3 (#91: the real exit byte survives)
		if (!strcmp(argv[1], "envchk")) {
			const char *m = getenv("PHS_MARK"), *d = getenv("PHS_DUP");
			int bits = 0;
			if (m && !strcmp(m, "rich")) bits |= ENV_MARK;
			if (d && !strcmp(d, "last")) bits |= ENV_DUP;
			if (getenv("PHS_PARENT"))    bits |= ENV_PARENT;
			if (getenv("PHS_SET"))       bits |= ENV_SET;
			return bits;
		}
		if (!strcmp(argv[1], "envcount")) {
			int n = 0;
			while (environ[n] && n < 200) n++;
			return n;
		}
		return 99;
	}

	// joey spawns the PARENT via SYS_SPAWN_WITH_FDS (the legacy surface),
	// which delivers no argv, so argv[0] is NULL here -- hardcode the self
	// name for the re-spawn. The CHILD, spawned below via our posix_spawn
	// (SYS_SPAWN_FULL_ARGV), DOES receive a real argv -- reading argv[1] is
	// exactly what proves argv pass-through end to end.
	const char *self = "pouch-hello-spawn";

	// === dup2 micro-test: old==new returns new (the clear-CLOEXEC idiom, the
	//     only dup2 case Thylacine supports -- onto-target has no kernel
	//     primitive; a negative fd is EBADF). old==new on a live fd is NOT
	//     validated beyond old>=0 (no rights-independent fd-existence probe). ===
	if (dup2(1, 1) != 1) return fail("dup2(1,1)!=1", 0);
	errno = 0;
	if (dup2(-1, -1) != -1 || errno != EBADF) return fail("dup2(-1,-1) not EBADF", 0);
	// onto-target (old != new) has no kernel primitive -> ENOSYS, deterministic
	// regardless of whether 3/4 are open (the resolver never runs).
	errno = 0;
	if (dup2(3, 4) != -1 || errno != ENOSYS) return fail("dup2 onto-target not ENOSYS", 0);

	// === test 1: pipe2(O_CLOEXEC) + posix_spawn("ok") redirecting the child's
	//     stdout into the pipe; capture + reap; expect WEXITSTATUS 0 ===
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0) return fail("pipe2", 0);

	posix_spawn_file_actions_t fa;
	if (posix_spawn_file_actions_init(&fa) != 0) return fail("fa_init", 0);
	// child fd 1 <- the pipe write end; the child holds neither raw pipe fd.
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_addclose(&fa, p[1]);
	posix_spawn_file_actions_addclose(&fa, p[0]);

	char *okargv[] = { (char *)self, (char *)"ok", NULL };
	pid_t pid = -1;
	int rc = posix_spawn(&pid, self, &fa, NULL, okargv, environ);
	posix_spawn_file_actions_destroy(&fa);
	(void)close(p[1]);   // parent drops the write end -> EOF when the child exits
	if (rc != 0) return fail("posix_spawn(ok)", rc);
	if (pid <= 0) return fail("posix_spawn(ok) pid", (int)pid);

	// read the child's stdout to EOF
	char buf[64];
	ssize_t n = 0, t;
	while (n < (ssize_t)sizeof buf - 1 &&
	       (t = read(p[0], buf + n, sizeof buf - 1 - n)) > 0)
		n += t;
	(void)close(p[0]);
	buf[n] = 0;
	if (strcmp(buf, "SPAWNCHILD-OK\n") != 0) return fail("child stdout mismatch", (int)n);

	int st = -1;
	if (waitpid(pid, &st, 0) != pid) return fail("waitpid(ok)", 0);
	if (!WIFEXITED(st)) return fail("ok child not WIFEXITED", st);
	if (WEXITSTATUS(st) != 0) return fail("WEXITSTATUS(ok)!=0", WEXITSTATUS(st));

	// === test 2: posix_spawn("fail") inheriting std streams; reap via a
	//     WNOHANG poll; expect WIFEXITED + WEXITSTATUS 3 (#91: the child's real
	//     exit byte survives to the parent, no longer collapsed to 1) ===
	char *failargv[] = { (char *)self, (char *)"fail", NULL };
	pid_t pid2 = -1;
	int rc2 = posix_spawn(&pid2, self, NULL, NULL, failargv, environ);
	if (rc2 != 0) return fail("posix_spawn(fail)", rc2);

	int st2 = 0;
	pid_t r2 = 0;
	for (long spin = 0; spin < 100000000L; spin++) {
		r2 = waitpid(pid2, &st2, WNOHANG);
		if (r2 == pid2) break;                 // reaped
		if (r2 < 0) return fail("waitpid(WNOHANG) err", 0);
		// r2 == 0: child not done yet -- keep polling (proves WNOHANG != -1)
	}
	if (r2 != pid2) return fail("fail child never reaped", 0);
	if (!WIFEXITED(st2)) return fail("fail child not WIFEXITED", st2);
	if (WEXITSTATUS(st2) != 3) return fail("WEXITSTATUS(fail)!=3", WEXITSTATUS(st2));

	// === test 3: envp is the child's whole environment (the SPAWN_EXT_ENV
	//     tail). PHS_PARENT goes into this Proc's /env after the crt read it
	//     into environ, so a child that inherited /env would hold it and a
	//     child given an envp never does. ===
	int pcount = 0;
	while (environ[pcount]) pcount++;
	if (getenv("PHS_MARK") || getenv("PHS_DUP") || getenv("PHS_SET") || getenv("PHS_SET2") ||
	    getenv("PHS_PARENT"))
		return fail("env premise: PHS_* already set", pcount);
	int pfd = open("/env/PHS_PARENT", O_WRONLY | O_CREAT, 0644);
	if (pfd < 0) return fail("env premise: create /env/PHS_PARENT", 0);
	ssize_t pw = write(pfd, "here", 4);
	(void)close(pfd);
	char pbuf[8] = { 0 };
	pfd = open("/env/PHS_PARENT", O_RDONLY);
	ssize_t pr = pfd >= 0 ? read(pfd, pbuf, sizeof pbuf - 1) : -1;
	if (pfd >= 0) (void)close(pfd);
	if (pw != 4 || pr != 4 || strcmp(pbuf, "here") != 0)
		return fail("env premise: /env/PHS_PARENT reads back", (int)pr);

	int erc = 0, bad = 0;
	char *blk[] = { (char *)"PHS_MARK=rich", (char *)"PHS_DUP=first",
	                (char *)"PHS_DUP=last", NULL };
	int got = env_child(self, "envchk", blk, &erc);
	bad |= env_leg(got == (ENV_MARK | ENV_DUP), "env block (want 3)", got, erc);

	// Two setenvs: a child that inherited /env instead would hold pcount + 1
	// (PHS_PARENT), so only the sent environ counts pcount + 2.
	if (setenv("PHS_SET", "yes", 1) != 0 || setenv("PHS_SET2", "yes", 1) != 0)
		return fail("setenv", 0);
	got = env_child(self, "envchk", environ, &erc);
	bad |= env_leg(got == ENV_SET, "env environ (want 8)", got, erc);
	got = env_child(self, "envcount", environ, &erc);
	bad |= env_leg(got == pcount + 2, "env environ count", got, erc);

	got = env_child(self, "envcount", NULL, &erc);
	bad |= env_leg(got == 0, "env NULL envp not empty", got, erc);

	// Refusals: each makes no child and names its errno. The kernel refuses
	// the first three; the nine 4010-byte records (past 32 KiB) never leave
	// this process.
	static char big[sizeof "PHS_BIG=" + 4097];
	memcpy(big, "PHS_BIG=", 8);
	memset(big + 8, 'x', 4097);
	static char huge[sizeof "PHS_HUGE=" + 4000];
	memcpy(huge, "PHS_HUGE=", 9);
	memset(huge + 9, 'y', 4000);
	char *noeq[] = { (char *)"PHS_NOEQUALS", NULL };
	char *slash[] = { (char *)"PHS/SLASH=1", NULL };
	char *bigv[] = { big, NULL };
	char *hugev[] = { huge, huge, huge, huge, huge, huge, huge, huge, huge, NULL };
	struct { const char *what; char **envp; int want; } refuse[] = {
		{ "env no '='", noeq, EINVAL }, { "env '/' in a name", slash, EINVAL },
		{ "env value past 4096", bigv, E2BIG }, { "env block past 32 KiB", hugev, E2BIG },
	};
	for (unsigned i = 0; i < sizeof refuse / sizeof refuse[0]; i++) {
		got = env_child(self, "envcount", refuse[i].envp, &erc);
		bad |= env_leg(got == -1 && erc == refuse[i].want, refuse[i].what, got, erc);
	}
	int stz = 0;
	errno = 0;
	pid_t left = waitpid(-1, &stz, WNOHANG);
	bad |= env_leg(left == -1 && errno == ECHILD, "env refusal made a child", (int)left, errno);
	if (bad) return 1;

	printf("pouch-hello-spawn: pipe+spawn+wait ok; WEXITSTATUS ok=0 fail=3; "
	       "env block/environ/empty ok, 4 refusals -- ALL SPAWN TESTS PASS\n");
	return 0;
}
