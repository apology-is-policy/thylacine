/* /pouch-hello-pty -- the PTY-3 proving binary.
 *
 * Exercises the pouch pty boundary-line (0021-pouch-pty, PTY-DESIGN.md
 * section 7): a pouch/Linux program reaches the PTY-2 ptyfs server + the
 * PTY-1 kernel controlling-terminal syscalls through the STANDARD POSIX
 * calls, translated by the tty ioctl dispatcher + the seam numbers.
 *
 * The GATE is the deterministic control surface -- this one Proc is both
 * the terminal emulator (master) and the session (slave), the same
 * two-roles-one-flow shape as the native /bin/pty-probe:
 *
 *   posix_openpt/grantpt/unlockpt -> /dev/pts/ptmx mint + no-op dance
 *   ptsname_r                     -> TIOCGPTN = the fstat qid decode
 *   isatty                        -> TIOCGWINSZ; 1 on master+slave,
 *                                    0 on a pipe (stdout here)
 *   tcgetattr                     -> the fresh-pts FULL COOKED default
 *   cfmakeraw + tcsetattr         -> the five-flag ctl decompose + readback
 *   TIOCSWINSZ / TIOCGWINSZ       -> the ctl winsize op (+ live WINCH)
 *   setsid + TIOCSCTTY + tcsetpgrp-> the kernel session dance (89/95/96/97)
 *   cooked ISIG ^C                -> SIGINT handler fires; the 0x03 is
 *                                    CONSUMED (never a slave byte) --
 *                                    SignalXorByte through the POSIX view
 *   close(master)                 -> SIGHUP handler fires + slave EOF
 *                                    (drain-then-EOF)
 *   kill/raise of a tty signum    -> EPERM (receive-only; the kernel
 *                                    I-39 POST gate, surfaced POSIX-shaped)
 *   forkpty                       -> -1 (fork is ENOSYS in pouch; a
 *                                    pre-existing pouch-wide seam, not a
 *                                    pty gap -- asserted honest-fail)
 */
#define _GNU_SOURCE   /* ptsname_r */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>

#define CHECK(cond, msg)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("pouch-hello-pty: FAIL %s (errno=%d)\n",    \
			       (msg), errno);                              \
			return 1;                                          \
		}                                                          \
	} while (0)

static volatile sig_atomic_t got_winch;
static volatile sig_atomic_t got_int;
static volatile sig_atomic_t got_hup;

static void on_winch(int sig) { (void)sig; got_winch = 1; }
static void on_int(int sig)   { (void)sig; got_int = 1; }
static void on_hup(int sig)   { (void)sig; got_hup = 1; }

/* Bounded wait for an async note delivery: each poll timeout is an
 * EL0-return (a delivery point) + yields to ptyfs. `fd` must have no
 * pending POLLIN data (a ready fd just makes the loop spin faster --
 * still bounded). */
static int wait_flag(volatile sig_atomic_t *flag, int fd)
{
	for (int i = 0; i < 300 && !*flag; i++) {
		struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
		poll(&p, 1, 10);
	}
	return *flag != 0;
}

int main(int argc, char **argv)
{
	/* --- #55c: the CONSOLE mode (`pouch-hello-pty cons`) ----------
	 * Run FROM ut in a live session so fd 1 is a real inherited cons
	 * fd (the boot prover pipes the probe's stdio, so it cannot carry
	 * this leg). Proves the 0026 cons arm end-to-end: the is-a-cons
	 * qid decode (bit 41) -> TIOCGWINSZ served from /dev/winsize
	 * (aurora's live grid) -> isatty truth on the console ->
	 * TIOCSWINSZ EPERM (renderer-owned geometry) -> tcgetattr ENOTTY
	 * (the console termios stays with the consctl delegation chain --
	 * the documented ARCH 23.5.3 seam). */
	if (argc > 1 && strcmp(argv[1], "cons") == 0) {
		/* fd 1, not fd 0: ut gives an un-redirected fg child a PIPE
		 * stdin on the console (the PTY-4b rule -- Inherit only under
		 * job control, which is pts-only), but stdout rides
		 * stdio_inherit = the cons Spoor. fd 1 is also the fd musl's
		 * stdio line-buffering probe targets -- the load-bearing one. */
		struct winsize ws;
		CHECK(ioctl(1, TIOCGWINSZ, &ws) == 0,
		      "TIOCGWINSZ(cons stdout)");
		CHECK(isatty(1) == 1, "isatty(cons stdout)");
		errno = 0;
		CHECK(ioctl(1, TIOCSWINSZ, &ws) == -1 && errno == EPERM,
		      "TIOCSWINSZ(cons) is EPERM");
		struct termios tio;
		errno = 0;
		CHECK(tcgetattr(1, &tio) == -1 && errno == ENOTTY,
		      "tcgetattr(cons) is ENOTTY (the documented seam)");
		printf("pty-cons: %ux%u isatty=1 setws=EPERM tcgetattr=ENOTTY OK\n",
		       (unsigned)ws.ws_col, (unsigned)ws.ws_row);
		return 0;
	}

	/* --- A. mint + name ------------------------------------------- */
	int mfd = posix_openpt(O_RDWR | O_NOCTTY);
	CHECK(mfd >= 0, "posix_openpt(/dev/pts/ptmx)");
	CHECK(grantpt(mfd) == 0, "grantpt");
	CHECK(unlockpt(mfd) == 0, "unlockpt (TIOCSPTLCK no-op)");

	char sname[32];
	CHECK(ptsname_r(mfd, sname, sizeof(sname)) == 0,
	      "ptsname_r (TIOCGPTN qid decode)");
	CHECK(strncmp(sname, "/dev/pts/", 9) == 0, "ptsname shape");

	int sfd = open(sname, O_RDWR);
	CHECK(sfd >= 0, "open(slave)");

	/* --- B. isatty ------------------------------------------------- */
	CHECK(isatty(mfd) == 1, "isatty(master)");
	CHECK(isatty(sfd) == 1, "isatty(slave)");
	errno = 0;
	CHECK(isatty(1) == 0, "isatty(stdout=pipe) is 0");

	/* --- C. the fresh-pts cooked default --------------------------- */
	struct termios tio0;
	CHECK(tcgetattr(sfd, &tio0) == 0, "tcgetattr(slave)");
	CHECK((tio0.c_lflag & ICANON) && (tio0.c_lflag & ECHO) &&
	      (tio0.c_lflag & ISIG), "cooked default lflag (icanon+echo+isig)");
	CHECK(tio0.c_iflag & ICRNL, "cooked default iflag (icrnl)");
	CHECK((tio0.c_oflag & OPOST) && (tio0.c_oflag & ONLCR),
	      "cooked default oflag (opost+onlcr)");
	CHECK(tio0.c_cc[VINTR] == 0x03, "VINTR is ^C");

	/* --- D. raw + restore ------------------------------------------ */
	struct termios raw = tio0;
	cfmakeraw(&raw);
	CHECK(tcsetattr(sfd, TCSANOW, &raw) == 0, "tcsetattr(raw)");
	struct termios chk;
	CHECK(tcgetattr(sfd, &chk) == 0, "tcgetattr(raw readback)");
	CHECK(!(chk.c_lflag & (ICANON | ECHO | ISIG)) &&
	      !(chk.c_iflag & ICRNL) && !(chk.c_oflag & ONLCR),
	      "raw readback (all five clear)");
	CHECK(tcsetattr(sfd, TCSAFLUSH, &tio0) == 0, "tcsetattr(restore cooked)");
	CHECK(tcgetattr(sfd, &chk) == 0 && (chk.c_lflag & ICANON),
	      "cooked restore readback");

	/* --- E. winsize ------------------------------------------------ */
	struct winsize ws = { .ws_row = 24, .ws_col = 80 };
	CHECK(ioctl(mfd, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ(master)");
	struct winsize rd;
	memset(&rd, 0, sizeof(rd));
	CHECK(ioctl(sfd, TIOCGWINSZ, &rd) == 0, "TIOCGWINSZ(slave)");
	CHECK(rd.ws_row == 24 && rd.ws_col == 80, "winsize readback 80x24");

	/* --- F. the session dance (kernel 89/95/96/97) ----------------- */
	long sid = setsid();
	CHECK(sid > 0, "setsid (SYS_SETSID=89)");
	CHECK(getsid(0) == (pid_t)sid, "getsid(0) round-trip");
	CHECK(getpgid(0) == (pid_t)sid, "getpgid(0) == new sid");
	CHECK(ioctl(sfd, TIOCSCTTY, 0) == 0, "TIOCSCTTY (SYS_TTY_ACQUIRE)");
	CHECK(tcgetpgrp(sfd) == (pid_t)sid, "tcgetpgrp == leader pgid");
	CHECK(tcsetpgrp(sfd, (pid_t)sid) == 0, "tcsetpgrp (idempotent re-seat)");

	/* --- G. live WINCH through the fg session ---------------------- */
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	CHECK(sigaction(SIGWINCH, &sa, 0) == 0, "sigaction(SIGWINCH)");
	ws.ws_row = 25; ws.ws_col = 81;
	CHECK(ioctl(mfd, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ(changed)");
	CHECK(wait_flag(&got_winch, sfd), "SIGWINCH handler fired");

	/* --- H. cooked ISIG ^C: signal, never a byte ------------------- */
	sa.sa_handler = on_int;
	CHECK(sigaction(SIGINT, &sa, 0) == 0, "sigaction(SIGINT)");
	CHECK(write(mfd, "x\x03y\r", 4) == 4, "master types x ^C y CR");
	char buf[16];
	long r = read(sfd, buf, sizeof(buf));
	CHECK(r == 3 && buf[0] == 'x' && buf[1] == 'y' && buf[2] == '\n',
	      "slave line is xy\\n (the ^C consumed, CR->NL)");
	CHECK(wait_flag(&got_int, sfd), "SIGINT handler fired (ISIG)");
	/* drain the echo so the master side is clean: "xy\r\n" (the ^C is
	 * never echoed; the NL echo is ONLCR-expanded). */
	r = read(mfd, buf, sizeof(buf));
	CHECK(r == 4 && buf[0] == 'x' && buf[1] == 'y' &&
	      buf[2] == '\r' && buf[3] == '\n', "echo is xy CRNL (no ^C leak)");

	/* --- I. the receive-only tty family + widened sigaction -------- */
	sa.sa_handler = on_hup;
	CHECK(sigaction(SIGHUP, &sa, 0) == 0, "sigaction(SIGHUP)");
	sa.sa_handler = SIG_DFL;
	CHECK(sigaction(SIGTSTP, &sa, 0) == 0, "sigaction(SIGTSTP) accepted");
	CHECK(sigaction(SIGQUIT, &sa, 0) == 0, "sigaction(SIGQUIT) accepted");
	errno = 0;
	CHECK(kill(0, SIGHUP) == -1 && errno == EPERM,
	      "kill(SIGHUP) is EPERM (receive-only; the kernel POST gate)");
	errno = 0;
	CHECK(raise(SIGTSTP) == -1 && errno == EPERM,
	      "raise(SIGTSTP) is EPERM (receive-only)");

	/* --- J. forkpty: honest-fail (fork is ENOSYS in pouch) --------- */
	{
		int am = -1;
		errno = 0;
		CHECK(forkpty(&am, 0, 0, 0) == -1,
		      "forkpty fails honestly (no fork in pouch)");
	}

	/* --- K. teardown: HUP on last-master close + drain-then-EOF ---- */
	CHECK(close(mfd) == 0, "close(master)");
	r = read(sfd, buf, sizeof(buf));
	CHECK(r == 0, "slave EOF after master close");
	CHECK(wait_flag(&got_hup, sfd), "SIGHUP handler fired (last master)");
	CHECK(close(sfd) == 0, "close(slave)");

	printf("pouch-hello-pty: exit 0\n");
	return 0;
}
