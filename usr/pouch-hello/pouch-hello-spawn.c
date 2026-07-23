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
//   argv[1]=="fail"-> CHILD:  exit 3 (the kernel collapses any non-zero exit
//                             to exit_status 1 at v1.0 -> WEXITSTATUS == 1).
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

int main(int argc, char **argv) {
	// --- CHILD paths (self-respawn) ---
	if (argc >= 2) {
		if (!strcmp(argv[1], "ok")) {
			static const char msg[] = "SPAWNCHILD-OK\n";
			(void)write(1, msg, sizeof msg - 1);
			return 0;
		}
		if (!strcmp(argv[1], "fail"))
			return 3;   // -> WEXITSTATUS 1 (v1.0 non-zero collapse)
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
	//     WNOHANG poll; expect WIFEXITED + WEXITSTATUS 1 (status translation) ===
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
	if (WEXITSTATUS(st2) != 1) return fail("WEXITSTATUS(fail)!=1", WEXITSTATUS(st2));

	printf("pouch-hello-spawn: pipe+spawn+wait ok; WEXITSTATUS ok=0 fail=1 "
	       "-- ALL SPAWN TESTS PASS\n");
	return 0;
}
