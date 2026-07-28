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
//   readlink        -> open+read of /proc/{self,<pid>}/{exe,cwd}, else the
//                      truthful EINVAL/ENOENT (V-4b-4, patch 0031)
//   realpath        -> no wire of its own: musl resolves in userspace atop
//                      readlink, so it works only once readlink tells the truth
//   stat            -> SYS_STAT on the synthetic Devs: /ctl + /env stat as the
//                      directories they are, and a /proc entry's S_IFMT lets
//                      S_ISDIR/S_ISREG classify it (V-4b-5)
//
// On success: "pouch-hello-fs: ALL WIRES PASS" + exit 0. Any wire failing
// prints "pouch-hello-fs: <wire> FAIL ..." and exits non-zero so joey's
// reap sees it. fd 1 is a pipe joey relays to the boot-log UART.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
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

	// --- readlink on the /proc link-shaped files (V-4b-4, patch 0031) ---
	// Linux serves /proc/{self,<pid>}/{exe,cwd} as SYMLINKS; Thylacine serves
	// them as regular files whose CONTENTS are the path. The boundary-line
	// translates. These run against NATIVE /proc (no diorama in this Proc's
	// namespace), which is the point: the shim must not depend on one.
	char lnk[256];
	ssize_t ln = readlink("/proc/self/exe", lnk, sizeof lnk - 1);
	if (ln < 0) return fail("readlink(exe)", "/proc/self/exe failed");
	if (ln == 0) return fail("readlink(exe)", "empty target");
	lnk[ln] = '\0';                    // readlink never NUL-terminates
	if (lnk[0] != '/') return fail("readlink(exe)", "target not absolute");
	if (!strstr(lnk, "pouch-hello-fs"))
		return fail("readlink(exe)", "target is not this program");

	// `self` must resolve to OUR pid, not the mounter's -- the shim rewrites
	// self -> getpid() rather than passing it through, so the numeric spelling
	// must produce a byte-identical answer.
	char pidpath[64], lnk2[256];
	snprintf(pidpath, sizeof pidpath, "/proc/%d/exe", (int)pid);
	ssize_t ln2 = readlink(pidpath, lnk2, sizeof lnk2 - 1);
	if (ln2 < 0) return fail("readlink(exe)", "numeric /proc/<pid>/exe failed");
	lnk2[ln2] = '\0';
	if (strcmp(lnk, lnk2) != 0)
		return fail("readlink(exe)", "self and <pid> disagree");

	// cwd is the other link-shaped file, and must agree with getcwd().
	char lnkc[256];
	ssize_t lnc = readlink("/proc/self/cwd", lnkc, sizeof lnkc - 1);
	if (lnc < 0) return fail("readlink(cwd)", "/proc/self/cwd failed");
	lnkc[lnc] = '\0';
	if (strcmp(lnkc, cwd0) != 0) return fail("readlink(cwd)", "disagrees with getcwd");

	// TRUNCATION is POSIX-silent: a short buffer yields the truncated length
	// and still no NUL. Asking for 4 bytes of an absolute path must give "/" +
	// 3 more, never a terminator inside the count.
	char tbuf[8];
	memset(tbuf, 'Z', sizeof tbuf);
	ssize_t tn = readlink("/proc/self/exe", tbuf, 4);
	if (tn != 4) return fail("readlink(trunc)", "short buffer did not truncate to 4");
	if (tbuf[0] != '/') return fail("readlink(trunc)", "truncated target not a path");
	if (tbuf[4] != 'Z') return fail("readlink(trunc)", "wrote past the buffer");

	// --- readlink's GENERAL arm: no symlinks exist, so an existing path is
	// EINVAL and an absent one is ENOENT. This is the pair musl's realpath()
	// reads as its fork in the road -- under the old ENOSYS sentinel realpath
	// failed on its first component, for every path on the system. ---
	errno = 0;
	if (readlink("/proc", lnk, sizeof lnk) >= 0)
		return fail("readlink(dir)", "an existing path reported a link");
	if (errno != EINVAL) return fail("readlink(dir)", "existing path errno not EINVAL");
	errno = 0;
	if (readlink("/no-such-path-here", lnk, sizeof lnk) >= 0)
		return fail("readlink(absent)", "an absent path reported a link");
	if (errno != ENOENT) return fail("readlink(absent)", "absent path errno not ENOENT");

	// A /proc path whose pid field is a very long digit run must be REJECTED by
	// the matcher and fall to the general arm -- not parsed. The matcher copies
	// those digits into a fixed buffer, so an unbounded run would smash the
	// stack; surviving this call and continuing is the regression.
	//
	// Deliberately a run of NINES, not zeros. It overflows past 31 bits and so
	// cannot name any Proc, which makes ENOENT unambiguous. (When this was
	// written devproc's parse_decimal accepted leading zeros, so a run of zeros
	// resolved to pid 0 and genuinely existed; V-4b-5 has since fixed that, and
	// the zero-padded case is asserted on its own below. Nines still isolate the
	// buffer bound from the pid-parsing rule, which is the point here.)
	errno = 0;
	if (readlink("/proc/99999999999999999999999/exe", lnk, sizeof lnk) >= 0)
		return fail("readlink(longpid)", "an impossible pid reported a link");
	if (errno != ENOENT) return fail("readlink(longpid)", "long-pid errno not ENOENT");

	// --- realpath: repaired for free by the EINVAL above (musl 1.2.x resolves
	// in userspace; it never touches /proc/self/fd, contrary to the note the
	// LLVM fork's getMainExecutable patch carries). Canonicalizing away "//",
	// "/./" and a "/.." must land back on the real path. ---
	// The buffer form of realpath() memcpy's up to PATH_MAX regardless of the
	// result's length (musl copies out of its own PATH_MAX scratch), so the
	// caller's buffer must BE PATH_MAX -- a shorter one is a silent overflow,
	// not a truncation.
	char rp[PATH_MAX];
	if (!realpath("/", rp)) return fail("realpath", "root failed");
	if (strcmp(rp, "/") != 0) return fail("realpath", "root not \"/\"");
	if (!realpath("/proc/./", rp)) return fail("realpath", "/proc/./ failed");
	if (strcmp(rp, "/proc") != 0) return fail("realpath", "/proc/./ not canonical");
	if (realpath("/no-such-path-here", rp)) return fail("realpath", "absent path succeeded");

	// --- the synthetic Devs stat, and their file TYPES are readable (V-4b-5) ---
	// /ctl and /env had no stat_native slot at all, so stat() on them returned
	// EIO -- and realpath() of anything beneath them with it, since musl walks
	// each prefix. devproc had a slot but reported bare permission bits with no
	// S_IFMT, so S_ISDIR of a pid directory was FALSE and every POSIX walker
	// that decides whether to descend (find, nftw, a shell glob) stopped there.
	struct stat sb;
	if (stat("/ctl", &sb) != 0) return fail("stat(/ctl)", "failed");
	if (!S_ISDIR(sb.st_mode)) return fail("stat(/ctl)", "not a directory");
	if (stat("/ctl/procs", &sb) != 0) return fail("stat(/ctl/procs)", "failed");
	if (!S_ISREG(sb.st_mode)) return fail("stat(/ctl/procs)", "not a regular file");
	if (!realpath("/ctl/./procs", rp)) return fail("realpath(/ctl)", "failed");
	if (strcmp(rp, "/ctl/procs") != 0) return fail("realpath(/ctl)", "not canonical");

	if (stat("/env", &sb) != 0) return fail("stat(/env)", "failed");
	if (!S_ISDIR(sb.st_mode)) return fail("stat(/env)", "not a directory");

	// A pid directory must classify AS a directory, and its files as files.
	if (stat(pidpath, &sb) != 0) return fail("stat(/proc/<pid>/exe)", "failed");
	if (!S_ISREG(sb.st_mode)) return fail("stat(/proc/<pid>/exe)", "not a regular file");
	char piddir[64];
	snprintf(piddir, sizeof piddir, "/proc/%ld", (long)pid);
	if (stat(piddir, &sb) != 0) return fail("stat(/proc/<pid>)", "failed");
	if (!S_ISDIR(sb.st_mode)) return fail("stat(/proc/<pid>)", "S_ISDIR is false");

	// A zero-padded pid does not name a Proc (V-4b-5) -- Linux's own rule. One
	// Proc must have exactly one name, or native /proc and the VIVARIUM diorama
	// disagree about which paths exist.
	char padded[64];
	snprintf(padded, sizeof padded, "/proc/0%ld", (long)pid);
	if (stat(padded, &sb) == 0) return fail("stat(padded pid)", "a zero-padded pid resolved");

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

	// --- realpath on a RELATIVE path (the form build tools actually use):
	// resolves against the per-Proc cwd and comes back absolute. ---
	char rpr[PATH_MAX];            // see the PATH_MAX note on the first realpath
	if (!realpath("b.txt", rpr)) return fail("realpath", "relative b.txt failed");
	if (rpr[0] != '/') return fail("realpath", "relative result not absolute");
	if (strcmp(rpr, "/pouch-fs-probe/b.txt") != 0)
		return fail("realpath", "relative result not cwd-joined");

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
