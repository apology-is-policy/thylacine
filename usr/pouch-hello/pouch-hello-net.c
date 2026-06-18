/* /pouch-hello-net -- the net-5 proving binary.
 *
 * Exercises the AF_INET socket-compat boundary-line (0016-pouch-net-sockets,
 * NET-DESIGN section 7): a pouch/Linux program reaches netd's /net through
 * the BSD socket calls, translated to /net 9P file operations.
 *
 * The GATE is the deterministic, host-decoupled control surface -- the part
 * that exercises the novel translation without a live peer:
 *   socket(AF_INET)            -> open /net/tcp/clone, read N (fd >= 0)
 *   family / proto rejection   -> AF_INET6 EAFNOSUPPORT; SOCK_RAW EPROTONOSUPPORT
 *   setsockopt                 -> SO_REUSEADDR benign-noop; SO_KEEPALIVE honest ENOPROTOOPT
 *   bind(INADDR_ANY:7701)      -> stash local addr (pouch-side)
 *   listen                     -> "announce *!7701" to netd (Err -> listen() -1)
 *   getsockname                -> the bound addr reads back
 *   close                      -> slot freed, ctl/data fds closed, conn clunked
 *   poll() over /net (net-6b-3)-> a connected UDP socket is POLLOUT-ready but
 *                                 POLLIN times out (the shim targets the QTPOLL
 *                                 `ready` sibling, not the always-ready data fd)
 *
 * The live connect+send+recv round-trip is BEST-EFFORT (host-coupled: it
 * egresses the real NIC through slirp to the host network) -- LOGGED, never a
 * gate, per the net-3c/net-4b discipline. The deterministic in-guest data
 * round-trip is owed to net-8 (a netd loopback interface).
 *
 * The blocking byte stream (send/recv on a connected socket) reuses 0006's
 * already-proven tagged read/write dispatch, so its correctness rides the
 * live probe + net-8 rather than this gate.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define CHECK(cond, msg)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("pouch-hello-net: FAIL %s (errno=%d)\n",    \
			       (msg), errno);                              \
			return 1;                                          \
		}                                                          \
	} while (0)

static void best_effort_live(void)
{
	/* Connect to a public HTTP endpoint and round-trip one request. Whether
	 * this succeeds depends entirely on the dev host's outbound network --
	 * it is LOGGED, never asserted (the net-3c/net-4b lesson). */
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		printf("pouch-hello-net: live skip (socket %d)\n", errno);
		return;
	}
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port   = htons(80);
	to.sin_addr.s_addr = htonl(0x01010101u);   /* 1.1.1.1 */
	if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0) {
		printf("pouch-hello-net: live connect skip (errno=%d, host net?)\n",
		       errno);
		close(s);
		return;
	}
	static const char req[] =
		"GET / HTTP/1.0\r\nHost: 1.1.1.1\r\nConnection: close\r\n\r\n";
	if (send(s, req, sizeof(req) - 1, 0) < 0) {
		printf("pouch-hello-net: live send skip (errno=%d)\n", errno);
		close(s);
		return;
	}
	char buf[64];
	long n = recv(s, buf, sizeof(buf) - 1, 0);
	if (n > 0) {
		buf[n] = '\0';
		/* first line only (avoid dumping the whole header) */
		char *nl = strchr(buf, '\r');
		if (nl) *nl = '\0';
		printf("pouch-hello-net: live round-trip OK (%ld bytes: %s)\n", n, buf);
	} else {
		printf("pouch-hello-net: live recv skip (n=%ld errno=%d)\n", n, errno);
	}
	close(s);
}

int main(void)
{
	int one = 1;

	/* 1. socket() over /net/tcp */
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(fd >= 0, "socket(AF_INET,STREAM)");

	/* 2. family / type rejection (P-3: explicit POSIX errno) */
	errno = 0;
	int r6 = socket(AF_INET6, SOCK_STREAM, 0);
	CHECK(r6 < 0 && errno == EAFNOSUPPORT, "AF_INET6 must EAFNOSUPPORT");
	errno = 0;
	int rraw = socket(AF_INET, SOCK_RAW, 0);
	CHECK(rraw < 0 && errno == EPROTONOSUPPORT, "SOCK_RAW must EPROTONOSUPPORT");

	/* 3. setsockopt: benign-satisfied vs honest-reject */
	CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == 0,
	      "setsockopt(SO_REUSEADDR)");
	errno = 0;
	int rk = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
	CHECK(rk < 0 && errno == ENOPROTOOPT, "SO_KEEPALIVE must ENOPROTOOPT");

	/* 4. bind + listen (announce) */
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(7701);
	CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0, "bind(*:7701)");
	CHECK(listen(fd, 1) == 0, "listen -> announce");

	/* 5. getsockname reads the bound addr back */
	struct sockaddr_in got;
	socklen_t glen = sizeof(got);
	memset(&got, 0, sizeof(got));
	CHECK(getsockname(fd, (struct sockaddr *)&got, &glen) == 0, "getsockname");
	CHECK(got.sin_family == AF_INET, "getsockname family");
	CHECK(got.sin_port == htons(7701), "getsockname port readback");

	/* 6. close frees the slot + the held ctl fd + clunks the conn */
	CHECK(close(fd) == 0, "close listener");

	/* 7. data-call control surface (net-6a-2): shutdown / sendto / recvfrom.
	 * All deterministic + host-decoupled -- the error paths need no peer, and
	 * a UDP sendto to the on-link gateway egresses without a listener. */
	int tfd = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(tfd >= 0, "socket(AF_INET,STREAM) #2");
	char rb[8];
	errno = 0;
	CHECK(shutdown(tfd, 99) < 0 && errno == EINVAL, "shutdown bad-how EINVAL");
	errno = 0;
	CHECK(shutdown(tfd, SHUT_WR) < 0 && errno == ENOTCONN, "shutdown fresh ENOTCONN");
	errno = 0;
	CHECK(sendto(tfd, "x", 1, MSG_OOB, 0, 0) < 0 && errno == EOPNOTSUPP,
	      "sendto bad-flag EOPNOTSUPP");
	errno = 0;
	CHECK(sendto(tfd, "x", 1, 0, 0, 0) < 0 && errno == ENOTCONN,
	      "sendto no-dest fresh ENOTCONN");
	errno = 0;
	CHECK(recvfrom(tfd, rb, sizeof rb, MSG_OOB, 0, 0) < 0 && errno == EOPNOTSUPP,
	      "recvfrom bad-flag EOPNOTSUPP");
	errno = 0;
	CHECK(recvfrom(tfd, rb, sizeof rb, 0, 0, 0) < 0 && errno == ENOTCONN,
	      "recvfrom fresh ENOTCONN");
	CHECK(close(tfd) == 0, "close tfd");

	/* UDP per-datagram sendto: targets the on-link DHCP gateway 10.0.2.2:9
	 * (discard). netd binds a local port + records the remote + egresses the
	 * datagram -- no peer needed (UDP is fire-and-forget). The send then
	 * leaves the socket connected, so getpeername reads the dest back and
	 * shutdown(SHUT_WR) drives the real `hangup` ctl write. */
	int ufd = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(ufd >= 0, "socket(AF_INET,DGRAM)");
	struct sockaddr_in gw;
	memset(&gw, 0, sizeof(gw));
	gw.sin_family = AF_INET;
	gw.sin_port   = htons(9);
	gw.sin_addr.s_addr = htonl(0x0a000202u);   /* 10.0.2.2 */
	CHECK(sendto(ufd, "ping", 4, 0, (struct sockaddr *)&gw, sizeof(gw)) == 4,
	      "udp sendto egress");
	struct sockaddr_in pg;
	socklen_t plen = sizeof(pg);
	memset(&pg, 0, sizeof(pg));
	CHECK(getpeername(ufd, (struct sockaddr *)&pg, &plen) == 0, "udp getpeername");
	CHECK(pg.sin_addr.s_addr == htonl(0x0a000202u) && pg.sin_port == htons(9),
	      "udp peer readback (10.0.2.2:9)");
	CHECK(shutdown(ufd, SHUT_WR) == 0, "udp shutdown(SHUT_WR) -> hangup");
	CHECK(close(ufd) == 0, "close ufd");

	/* 8. poll() readiness over /net (net-6b-3): the pouch poll() shim must
	 *    target the QTPOLL /net/<proto>/N/ready sibling, NOT the always-ready
	 *    data fd. A UDP socket connected to the gateway is immediately
	 *    WRITABLE (POLLOUT, no peer needed -- a datagram send never blocks)
	 *    but has NO datagram queued, so POLLIN must DEFER and time out. The
	 *    POLLIN-timeout is the load-bearing assertion: it proves poll()
	 *    actually WAITED on the readiness file. Were the shim still polling
	 *    the data fd (a regular dev9p file = always-ready), POLLIN would
	 *    return ready instantly and this would fail. */
	int pfd = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(pfd >= 0, "socket(AF_INET,DGRAM) #poll");
	CHECK(sendto(pfd, "p", 1, 0, (struct sockaddr *)&gw, sizeof(gw)) == 1,
	      "udp connect-for-poll (binds local port -> writable)");
	struct pollfd po;
	po.fd = pfd; po.events = POLLOUT; po.revents = 0;
	int pr = poll(&po, 1, 2000);
	CHECK(pr == 1 && (po.revents & POLLOUT),
	      "poll(POLLOUT) ready on a writable udp socket");
	po.fd = pfd; po.events = POLLIN; po.revents = 0;
	pr = poll(&po, 1, 150);
	CHECK(pr == 0, "poll(POLLIN) times out on an empty udp socket (waited)");
	CHECK(close(pfd) == 0, "close pfd");

	printf("pouch-hello-net: control surface OK "
	       "(socket/reject/setsockopt/bind/listen/getsockname/"
	       "shutdown/sendto/recvfrom/poll/close)\n");

	best_effort_live();

	printf("pouch-hello-net: exit 0\n");
	return 0;
}
