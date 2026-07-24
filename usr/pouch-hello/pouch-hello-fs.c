// /pouch-hello-fs -- the CL-1a FS/process boundary-line prover (Clade arc,
// docs/LLVM-DESIGN.md sub-chunk CL-1a). Exercises every pouch FS/process
// wire 0024 added, END TO END through the POSIX API (not the raw syscalls),
// so a runtime regression in any translation surfaces as a labelled FAIL.
//
// Runs POST-pivot from a WRITABLE root (spawned by joey after the Stratum
// pivot; the boot-chain identity owns the pool root). It creates its own
// working directory, chdir's in, and drives the full create/write/rename/
// stat/readdir/unlink/rmdir cycle with cwd-relative paths -- which also
// proves chdir + getcwd + the SYS_open cwd-join (a relative path resolving
// against the per-Proc cwd, LS-4).
//
// The wires proven (Linux/POSIX call -> Thylacine kernel syscall):
//   getpid          -> SYS_GETPID
//   getcwd / chdir  -> SYS_GETCWD / SYS_CHDIR
//   mkdir           -> SYS_WALK_CREATE (DMDIR)
//   open(O_CREAT)   -> SYS_WALK_CREATE (regular file)
//   write / read    -> SYS_WRITE / SYS_READ (pre-existing; the round-trip check)
//   ftruncate       -> SYS_WSTAT (SIZE)
//   fchmod          -> SYS_WSTAT (MODE)
//   access          -> SYS_STAT (existence + owner-rwx)
//   rename          -> SYS_RENAME
//   readdir         -> SYS_READDIR (9P-stream -> struct dirent translation)
//   unlink / rmdir  -> SYS_UNLINK (+REMOVEDIR)
//
// On success: "pouch-hello-fs: ALL WIRES PASS" + exit 0. Any wire failing
// prints "pouch-hello-fs: <wire> FAIL ..." and exits non-zero so joey's
// reap sees it. fd 1 is a pipe joey relays to the boot-log UART.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

static int fail(const char *wire, const char *why) {
	printf("pouch-hello-fs: %s FAIL (%s, errno=%d)\n", wire, why, errno);
	return 1;
}

int main(void) {
	// --- getpid ---
	pid_t pid = getpid();
	if (pid <= 0) return fail("getpid", "non-positive pid");

	// --- getcwd (initial) ---
	char cwd0[256];
	if (!getcwd(cwd0, sizeof cwd0)) return fail("getcwd", "returned NULL");
	if (cwd0[0] != '/') return fail("getcwd", "not absolute");

	// --- getcwd with a PATH_MAX buffer (CL-1c-2 audit F1 regression) ---
	// GNU make / clang / git call getcwd(buf, PATH_MAX). The kernel handler used
	// to REJECT any buffer > SYS_OPEN_PATH_MAX+1 (1025) -> EIO -> `make: getcwd:
	// I/O error`. The 256-byte cwd0 above (<= 1025) masked it; this exercises the
	// large-buffer path the fix repairs. It must succeed + agree with cwd0.
	char cwdbig[4096];
	if (!getcwd(cwdbig, sizeof cwdbig))
		return fail("getcwd-pathmax", "PATH_MAX buffer rejected (F1 regression)");
	if (strcmp(cwdbig, cwd0) != 0)
		return fail("getcwd-pathmax", "PATH_MAX cwd disagrees with small-buf cwd");

	// --- mkdir a working dir at root (absolute) ---
	const char *wdir = "/pouch-fs-probe";
	(void)rmdir(wdir);                 // clean a stale run (best-effort)
	if (mkdir(wdir, 0755) != 0) return fail("mkdir", "create working dir");

	// --- chdir + getcwd round-trip ---
	if (chdir(wdir) != 0) return fail("chdir", "into working dir");
	char cwd1[256];
	if (!getcwd(cwd1, sizeof cwd1)) return fail("getcwd", "post-chdir NULL");
	if (strcmp(cwd1, wdir) != 0) return fail("chdir/getcwd", "cwd mismatch");

	// --- open(O_CREAT) a regular file (RELATIVE path -> cwd-join) ---
	int fd = open("a.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) return fail("open(O_CREAT)", "create a.txt");

	// --- write ---
	static const char payload[] = "clade-cl1a\n";
	const size_t plen = sizeof(payload) - 1;
	if (write(fd, payload, plen) != (ssize_t)plen) { close(fd); return fail("write", "short write"); }

	// --- ftruncate SHRINK a freshly-written file (11 -> 4 = "clad") -- the
	// SYS_WSTAT SIZE wire proof; the read-back below verifies the new length.
	// Tested on a fresh file (not the extend-then-shrink sequence, which
	// exercises a below-wire Stratum sparse-truncate edge -- see the extend
	// test's note). ---
	if (ftruncate(fd, 4) != 0) { close(fd); return fail("ftruncate", "shrink fresh 11->4"); }
	if (close(fd) != 0) return fail("close", "after write");

	// --- open(O_CREAT) without O_EXCL on an EXISTING file: must open, not fail ---
	int fd2 = open("a.txt", O_RDONLY | O_CREAT, 0644);
	if (fd2 < 0) return fail("open(O_CREAT existing)", "reopen a.txt");

	// --- read-back: exactly the 4 bytes ftruncate left ("clad") ---
	char rb[64];
	ssize_t n = read(fd2, rb, sizeof rb);
	close(fd2);
	if (n != 4) return fail("read", "wrong length after shrink");
	if (memcmp(rb, "clad", 4) != 0) return fail("read", "content mismatch");

	// --- ftruncate EXTEND on a SEPARATE fresh file (the lld FileOutputBuffer
	// pattern: create -> ftruncate to the total size -> write). Verified via
	// stat, then cleaned up. This is the load-bearing build-tool use of
	// ftruncate; the SHRINK above and this EXTEND together prove both
	// directions of the wire on fresh files. ---
	int fe = open("ext.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fe < 0) return fail("open(ext)", "create ext.txt");
	if (ftruncate(fe, 64) != 0) { close(fe); return fail("ftruncate", "extend fresh 0->64"); }
	close(fe);
	struct stat stx;
	if (stat("ext.txt", &stx) != 0) return fail("stat", "ext.txt after extend");
	if (stx.st_size != 64) return fail("ftruncate", "extend size not 64");
	if (unlink("ext.txt") != 0) return fail("unlink", "ext.txt cleanup");

	// --- O_EXCL on an existing file: must be EEXIST (mkstemp's contract) ---
	int fdx = open("a.txt", O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fdx >= 0) { close(fdx); return fail("open(O_EXCL)", "did not reject existing"); }
	if (errno != EEXIST) return fail("open(O_EXCL)", "errno not EEXIST");

	// --- fchmod (path form) via chmod ---
	if (chmod("a.txt", 0600) != 0) return fail("chmod", "set 0600");

	// --- access: exists + readable ---
	if (access("a.txt", F_OK) != 0) return fail("access", "F_OK on existing");
	if (access("a.txt", R_OK) != 0) return fail("access", "R_OK on 0600 owner");
	if (access("nope.txt", F_OK) == 0) return fail("access", "F_OK on absent succeeded");

	// --- rename ---
	if (rename("a.txt", "b.txt") != 0) return fail("rename", "a.txt -> b.txt");
	if (access("a.txt", F_OK) == 0) return fail("rename", "old name still present");
	if (access("b.txt", F_OK) != 0) return fail("rename", "new name absent");

	// --- readdir: the working dir must contain exactly b.txt (+ . / ..) ---
	DIR *d = opendir(".");
	if (!d) return fail("opendir", "cwd");
	int saw_b = 0, saw_a = 0, others = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		if (!strcmp(de->d_name, "b.txt")) saw_b = 1;
		else if (!strcmp(de->d_name, "a.txt")) saw_a = 1;
		else others++;
	}
	closedir(d);
	if (!saw_b) return fail("readdir", "b.txt not enumerated");
	if (saw_a)  return fail("readdir", "renamed-away a.txt still enumerated");
	if (others) return fail("readdir", "unexpected extra entry");

	// --- unlink ---
	if (unlink("b.txt") != 0) return fail("unlink", "b.txt");
	if (access("b.txt", F_OK) == 0) return fail("unlink", "b.txt still present");

	// --- rmdir (chdir back to root first; a dir cannot be removed as cwd) ---
	if (chdir("/") != 0) return fail("chdir", "back to root");
	if (rmdir(wdir) != 0) return fail("rmdir", "working dir");
	if (access(wdir, F_OK) == 0) return fail("rmdir", "working dir still present");

	printf("pouch-hello-fs: pid=%d cwd0=%s -- ALL WIRES PASS\n", (int)pid, cwd0);
	return 0;
}
