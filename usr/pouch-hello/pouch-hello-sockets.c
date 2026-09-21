/* /pouch-hello-sockets — the eighth pouch binary.
 *
 * P6-pouch-sockets (sub-chunk 12) proving binary. Exercises the FULL
 * AF_UNIX SOCK_STREAM round trip end-to-end against Thylacine's /srv
 * registry in BYTE MODE (the new transport mode added to the kernel
 * SrvConn at this sub-chunk). Uses one Proc with two pthreads — a
 * server thread doing bind/listen/accept/read/write and the main
 * thread doing connect/write/read.
 *
 * Why both threads in one Proc:
 *   - One Proc with two threads exercises bind+connect together (the
 *     per-Proc /srv client cap was retired in stalk-3b).
 *   - SYS_srv_peer is an accept-side (server) primitive; the client
 *     endpoint (open=connect, CSRVCLIENT) is rejected by direction, so
 *     the server side has the SO_PEERCRED oracle and the client side
 *     gets ENOTSOCK (matching a real two-Proc setup: only the poster
 *     queries who connected).
 *
 * Round-trip pattern (proving the byte transport, not 9P):
 *   1. server thread: socket, bind ("/srv/pouch-sock-demo"), listen,
 *      barrier-wait, accept, read, write, getsockopt(SO_PEERCRED),
 *      close.
 *   2. main thread: barrier-wait, socket, connect, write, read,
 *      getsockopt(SO_PEERCRED), close, pthread_join.
 *
 * The proving claim: every pouch socket call works AND the byte
 * stream is byte-accurate — server reads exactly the bytes the
 * client wrote, with no 9P framing visible to userspace.
 *
 * This binary requires PROC_FLAG_MAY_POST_SERVICE — joey grants it
 * via t_spawn_with_perms(..., T_SPAWN_PERM_MAY_POST_SERVICE).
 */

/* Diagnostics go to STDOUT: joey installs fds 0 and 1 only, and echoes the
 * child's stdout to the boot log. A failure line on fd 2 is a line nobody
 * sees (pouch-hello-threads.c documents the same trap). */
#define _GNU_SOURCE
#include "pouch-census.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

extern char **environ;

/* This prover runs PRE-pivot: the root is the initrd and the binary sits at
 * its top level. */
static const char SELF[] = "/pouch-hello-sockets";

static const char SOCK_PATH[]    = "/srv/pouch-sock-demo";
static const char NONEXIST_PATH[] = "/srv/pouch-sock-nonex";
static const char MSG_PING[]     = "PING\n";
static const char MSG_PONG[]     = "PONG\n";
static const char MSG_LINE[]     = "LINE 7\n";
static const char MSG_ECHO[]     = "ECHO 7 tail\n";

static pthread_barrier_t g_ready;
static int               g_server_ok = 0;

/* ---------- subtest A: family / type / protocol refusals ---------- */

static int test_family_refusals(void)
{
    /* net-5 (0016-pouch-net-sockets): AF_INET is now a valid /net socket
     * (pouch-hello-net proves it). AF_INET6 stays unsupported, so it is the
     * family-refusal case here -- this subtest still asserts a refused
     * family, just no longer AF_INET. */
    int s = socket(10 /* AF_INET6 */, SOCK_STREAM, 0);
    if (s >= 0 || errno != EAFNOSUPPORT) {
        printf("test: AF_INET6 should have refused with "
                "EAFNOSUPPORT, got s=%d errno=%d\n", s, errno);
        if (s >= 0) close(s);
        return -1;
    }
    printf("test: socket(AF_INET6) refused EAFNOSUPPORT ok\n");

    s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s >= 0 || errno != EPROTONOSUPPORT) {
        printf("test: SOCK_DGRAM should have refused with "
                "EPROTONOSUPPORT, got s=%d errno=%d\n", s, errno);
        if (s >= 0) close(s);
        return -1;
    }
    printf("test: socket(SOCK_DGRAM) refused EPROTONOSUPPORT ok\n");

    s = socket(AF_UNIX, SOCK_STREAM, 42);
    if (s >= 0 || errno != EPROTONOSUPPORT) {
        printf("test: protocol=42 should have refused with "
                "EPROTONOSUPPORT, got s=%d errno=%d\n", s, errno);
        if (s >= 0) close(s);
        return -1;
    }
    printf("test: socket(protocol=42) refused EPROTONOSUPPORT ok\n");

    return 0;
}

/* ---------- subtest B: connect to nonexistent + path validation ---------- */

static int test_connect_nonexistent(void)
{
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) {
        printf("test: socket failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, NONEXIST_PATH, sizeof(NONEXIST_PATH));

    int r = connect(c, (struct sockaddr *)&addr, sizeof(addr));
    if (r == 0) {
        printf("test: connect(%s) UNEXPECTEDLY succeeded\n",
                NONEXIST_PATH);
        close(c);
        return -1;
    }
    if (errno != ECONNREFUSED) {
        printf("test: connect(%s) wrong errno=%d (want "
                "ECONNREFUSED)\n", NONEXIST_PATH, errno);
        close(c);
        return -1;
    }
    printf("test: connect(%s) refused ECONNREFUSED ok\n", NONEXIST_PATH);

    if (close(c) != 0) {
        printf("test: close(fresh slot) failed errno=%d\n", errno);
        return -1;
    }
    printf("test: close(fresh slot) ok\n");

    return 0;
}

static int test_path_validation(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        printf("test: socket failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, "/usr/foo", sizeof("/usr/foo"));
    int r = bind(s, (struct sockaddr *)&addr, sizeof(addr));
    if (r == 0 || errno != EINVAL) {
        printf("test: bind(/usr/foo) should have rejected "
                "EINVAL, got r=%d errno=%d\n", r, errno);
        close(s);
        return -1;
    }
    printf("test: bind(/usr/foo) rejected EINVAL ok (path outside /srv/)\n");

    close(s);
    return 0;
}

/* ---------- subtest C: round-trip data via byte-mode SrvConn ---------- */

static void *server_main(void *arg)
{
    (void)arg;

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        printf("server: socket failed errno=%d\n", errno);
        pthread_barrier_wait(&g_ready);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCK_PATH, sizeof(SOCK_PATH));

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("server: bind failed errno=%d\n", errno);
        close(s);
        pthread_barrier_wait(&g_ready);
        return NULL;
    }
    if (listen(s, 1) != 0) {
        printf("server: listen failed errno=%d\n", errno);
        close(s);
        pthread_barrier_wait(&g_ready);
        return NULL;
    }
    printf("server: bind+listen ok at %s\n", SOCK_PATH);

    /* Release the client: server is ready for accept. */
    pthread_barrier_wait(&g_ready);

    struct sockaddr_un peer;
    socklen_t          peerlen = sizeof(peer);
    int conn = accept(s, (struct sockaddr *)&peer, &peerlen);
    if (conn < 0) {
        printf("server: accept failed errno=%d\n", errno);
        close(s);
        return NULL;
    }
    printf("server: accept ok, conn fd=%d\n", conn);

    char buf[64];
    ssize_t n = read(conn, buf, sizeof(buf) - 1);
    if (n != (ssize_t)(sizeof(MSG_PING) - 1)) {
        printf("server: read got %zd want %zu errno=%d\n",
                n, sizeof(MSG_PING) - 1, errno);
        close(conn); close(s);
        return NULL;
    }
    buf[n] = '\0';
    if (memcmp(buf, MSG_PING, sizeof(MSG_PING) - 1) != 0) {
        printf("server: read got '%s' want '%s'\n", buf, MSG_PING);
        close(conn); close(s);
        return NULL;
    }
    printf("server: read PING (%zd bytes byte-accurate) ok\n", n);

    ssize_t w = write(conn, MSG_PONG, sizeof(MSG_PONG) - 1);
    if (w != (ssize_t)(sizeof(MSG_PONG) - 1)) {
        printf("server: write got %zd want %zu errno=%d\n",
                w, sizeof(MSG_PONG) - 1, errno);
        close(conn); close(s);
        return NULL;
    }
    printf("server: write PONG ok\n");

    struct {
        int          pid;
        unsigned int uid;
        unsigned int gid;
    } cred = {0};
    socklen_t credlen = sizeof(cred);
    if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &credlen) != 0) {
        printf("server: getsockopt(SO_PEERCRED) errno=%d\n", errno);
        close(conn); close(s);
        return NULL;
    }
    if (cred.pid == 0) {
        printf("server: peer pid is zero (unexpected)\n");
        close(conn); close(s);
        return NULL;
    }
    printf("server: SO_PEERCRED pid=%d uid=%u gid=%u\n",
           cred.pid, cred.uid, cred.gid);

    /* The client's stdio leg: the server end stays raw, so a byte the
     * client's FILE mangles cannot be un-mangled by a FILE here. */
    n = read(conn, buf, sizeof(buf) - 1);
    if (n != (ssize_t)(sizeof(MSG_LINE) - 1) ||
        memcmp(buf, MSG_LINE, sizeof(MSG_LINE) - 1) != 0) {
        /* The client is about to wait at a barrier only this thread can
         * release: say why and end the process, or the boot hangs mute. */
        printf("server: stdio leg read got %zd errno=%d\n", n, errno);
        fflush(stdout);
        _exit(1);
    }
    /* The ppoll leg's sequencing, both halves load-bearing:
     *  - the client polls only AFTER its request has been consumed (this
     *    barrier). Polled earlier, a kernel that samples the WRONG end of the
     *    connection calls the client's own unread request "readable" and the
     *    leg goes green over it -- which is what it did for one gate.
     *  - the connection stays open until the client HAS polled (the barrier
     *    after the write), so the only thing that can end its poll is the
     *    reply. The pause makes "parked, then woken by the reply" the usual
     *    schedule; "replied before it looked" is also correct and also passes. */
    pthread_barrier_wait(&g_ready);
    usleep(100 * 1000);
    w = write(conn, MSG_ECHO, sizeof(MSG_ECHO) - 1);
    if (w != (ssize_t)(sizeof(MSG_ECHO) - 1)) {
        printf("server: stdio leg write got %zd errno=%d\n", w, errno);
        fflush(stdout);
        _exit(1);
    }
    pthread_barrier_wait(&g_ready);
    /* The close is the client's second poll: an orderly EOF. */
    usleep(100 * 1000);

    close(conn);
    close(s);
    g_server_ok = 1;
    return NULL;
}

/* How many sockets this Proc can still open: socket() until it refuses, then
 * give them all back. Measured, not mirrored from libc's table size -- the
 * leak check below compares two readings of it. */
static int free_socket_slots(void)
{
    int held[64];
    int n = 0;
    while (n < (int)(sizeof(held) / sizeof(held[0]))) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) break;
        held[n++] = fd;
    }
    for (int i = 0; i < n; i++) close(held[i]);
    return n;
}

/* stdio over a connected socket: fdopen() the client end, write a line through
 * the FILE, read the reply back through it (the scanf pushback path included),
 * and fclose() it. The socket fd is a libc-side slot, not a kernel handle, so
 * a stdio backend that hands f->fd to the kernel raw fails every one of these
 * and fclose() strands the slot. Consumes `c` either way. */
static int stdio_over_socket(int c)
{
    FILE *cf = fdopen(c, "r+");
    if (!cf) {
        printf("client: fdopen(socket) failed errno=%d\n", errno);
        close(c);
        return -1;
    }
    if (fputs(MSG_LINE, cf) == EOF || fflush(cf) != 0) {
        printf("client: stdio write over socket failed errno=%d\n", errno);
        fclose(cf);
        return -1;
    }
    /* ppoll() on the socket must report the REPLY, and nothing else, as
     * readable. Three things have each broken this and each reads differently:
     *   - libc hands the kernel the socket fd raw (a tagged value, not a kernel
     *     handle): POLLNVAL at once -- which also counts as "ready", so only
     *     the revents tell it from success;
     *   - the kernel samples the SERVER's end for a client: POLLIN from the
     *     client's own unread request, or -- polled after the server consumed
     *     it, as here -- nothing, ever: pr == 0 after the full timeout;
     *   - the kernel walks no hook list when the reply lands: the same pr == 0.
     * See server_main for why the barriers sit where they do. */
    pthread_barrier_wait(&g_ready);         /* the request has been consumed */
    {
        struct pollfd pf = { .fd = c, .events = POLLIN };
        struct timespec ts = { .tv_sec = 10 };
        int pr = ppoll(&pf, 1, &ts, NULL);
        if (pr != 1 || pf.revents != POLLIN) {
            printf("client: ppoll(socket) = %d revents=%#x errno=%d (want exactly POLLIN)\n",
                   pr, (unsigned)pf.revents, errno);
            fflush(stdout);
            _exit(1);                       /* the server waits at a barrier */
        }
    }
    pthread_barrier_wait(&g_ready);         /* polled; the server may close */
    int v = -1;
    char rest[16];
    if (fscanf(cf, "ECHO %d", &v) != 1 || v != 7 ||
        !fgets(rest, sizeof rest, cf) || strcmp(rest, " tail\n") != 0) {
        printf("client: stdio read over socket wrong: v=%d errno=%d\n",
                v, errno);
        fclose(cf);
        return -1;
    }
    errno = 0;
    if (fseek(cf, 0, SEEK_CUR) != -1 || errno != ESPIPE) {
        printf("client: fseek(socket FILE) errno=%d (want ESPIPE)\n", errno);
        fclose(cf);
        return -1;
    }
    /* The peer's orderly close, in the shape a socket program expects:
     * POLLIN|POLLHUP and no POLLERR, then a read of 0. The kernel reports a
     * /srv connection pipe-like (POLLHUP|POLLERR, no POLLIN at a drained EOF);
     * libc's poll() supplies the socket shape. Without it the loop every port
     * has -- "POLLIN? then read; 0 means closed" -- never sees its POLLIN and
     * spins on a poll() that returns at once. */
    {
        struct pollfd pf = { .fd = c, .events = POLLIN };
        struct timespec ts = { .tv_sec = 10 };
        int pr = ppoll(&pf, 1, &ts, NULL);
        if (pr != 1 || pf.revents != (POLLIN | POLLHUP)) {
            printf("client: ppoll(closed socket) = %d revents=%#x errno=%d (want POLLIN|POLLHUP)\n",
                   pr, (unsigned)pf.revents, errno);
            fclose(cf);
            return -1;
        }
        if (fgetc(cf) != EOF || !feof(cf)) {
            printf("client: read after POLLHUP was not EOF\n");
            fclose(cf);
            return -1;
        }
    }
    if (fclose(cf) != 0) {
        printf("client: fclose(socket FILE) failed errno=%d\n", errno);
        return -1;
    }
    printf("client: stdio over socket (fputs/ppoll/fscanf/fgets/ppoll-eof/fclose) ok\n");
    return 0;
}

/* A socket fd does not fit an fd_set, and FD_SET must say so instead of
 * storing 128 MiB past the set. The child does exactly that and must die by
 * abort() -- 127 here -- not return, and not fault on some unmapped page. */
static int test_fdset_guard(void)
{
    char *cargv[] = { (char *)SELF, (char *)"fdsetoob", NULL };
    pid_t pid;
    int st = 0;
    if (posix_spawn(&pid, SELF, NULL, NULL, cargv, environ) != 0) {
        printf("test: fdset guard: respawn failed errno=%d\n", errno);
        return -1;
    }
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 127) {
        printf("test: fdset guard: child status %#x (want abort = exit 127)\n",
               (unsigned)st);
        return -1;
    }
    printf("test: FD_SET(socket fd) stops the program (abort) ok\n");
    return 0;
}

static int test_round_trip(void)
{
    int slots_before = free_socket_slots();
    if (slots_before < 3 || slots_before >= 64) {
        /* 64 is the probe's own ceiling: a reading AT it is not a measurement. */
        printf("test: %d socket slots free at start (want 3..63)\n",
                slots_before);
        return -1;
    }

    if (pthread_barrier_init(&g_ready, NULL, 2) != 0) {
        printf("test: pthread_barrier_init failed\n");
        return -1;
    }

    pthread_t srv;
    if (pthread_create(&srv, NULL, server_main, NULL) != 0) {
        printf("test: pthread_create failed\n");
        return -1;
    }

    pthread_barrier_wait(&g_ready);
    printf("client: server ready, connecting\n");

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) {
        printf("client: socket failed errno=%d\n", errno);
        pthread_join(srv, NULL);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCK_PATH, sizeof(SOCK_PATH));

    if (connect(c, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("client: connect failed errno=%d\n", errno);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    printf("client: connect ok at %s, conn fd=0x%x\n", SOCK_PATH, c);

    ssize_t w = write(c, MSG_PING, sizeof(MSG_PING) - 1);
    if (w != (ssize_t)(sizeof(MSG_PING) - 1)) {
        printf("client: write got %zd want %zu errno=%d\n",
                w, sizeof(MSG_PING) - 1, errno);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    printf("client: write PING ok\n");

    char buf[64];
    ssize_t n = read(c, buf, sizeof(buf) - 1);
    if (n != (ssize_t)(sizeof(MSG_PONG) - 1)) {
        printf("client: read got %zd want %zu errno=%d\n",
                n, sizeof(MSG_PONG) - 1, errno);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    buf[n] = '\0';
    if (memcmp(buf, MSG_PONG, sizeof(MSG_PONG) - 1) != 0) {
        printf("client: read got '%s' want '%s'\n", buf, MSG_PONG);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    printf("client: read PONG (%zd bytes byte-accurate) ok\n", n);

    /* SO_PEERCRED on the client side is NOT supported at v1.0 — it is an
     * accept-side (server) query. stalk-3c open=connect made the client
     * endpoint a devsrv conn Spoor too, so SYS_SRV_PEER rejects it by
     * direction (the CSRVCLIENT flag): its SrvConn stamps the CONNECTOR as
     * the peer, so a client query would mis-report the caller's own
     * identity. The proving check is on the SERVER side above (see
     * "server: SO_PEERCRED ..."). For now, exercise the negative path:
     * client-side getsockopt returns ENOTSOCK. */
    {
        struct {
            int          pid;
            unsigned int uid;
            unsigned int gid;
        } cred = {0};
        socklen_t credlen = sizeof(cred);
        int r = getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &credlen);
        if (r == 0) {
            printf("client: getsockopt(SO_PEERCRED) UNEXPECTEDLY "
                    "succeeded on client-side fd\n");
            close(c);
            pthread_join(srv, NULL);
            return -1;
        }
        if (errno != ENOTSOCK) {
            printf("client: getsockopt(SO_PEERCRED) wrong errno=%d "
                    "(want ENOTSOCK on client-side fd at v1.0)\n", errno);
            close(c);
            pthread_join(srv, NULL);
            return -1;
        }
        printf("client: SO_PEERCRED client-side returns ENOTSOCK at v1.0 ok\n");
    }

    int stdio_rc = stdio_over_socket(c);   /* closes c */
    if (stdio_rc != 0) {
        /* Do NOT join: on a libc whose fclose() strands the socket slot -- the
         * defect this leg exists for -- the connection is still open, the
         * server thread is blocked in read(), and the join never returns. The
         * failure would then arrive as joey's reap deadline with no line. */
        printf("pouch-hello-sockets: FAIL stdio over socket\n");
        fflush(stdout);
        _exit(1);
    }
    pthread_join(srv, NULL);
    pthread_barrier_destroy(&g_ready);
    if (!g_server_ok) {
        printf("test: server thread reported failure\n");
        return -1;
    }

    /* Every slot this test took must be back: the listener and the client end
     * that was closed through its FILE. (The AF_UNIX accepted end is a plain
     * kernel fd, not a slot.) */
    int slots_after = free_socket_slots();
    if (slots_after != slots_before) {
        printf("test: socket slots leaked: %d free before, %d after\n",
                slots_before, slots_after);
        return -1;
    }
    printf("test: socket slots %d free before == %d after ok\n",
           slots_before, slots_after);
    return 0;
}

int main(int argc, char **argv)
{
    /* CHILD (self-respawn, the fd_set guard leg). */
    if (argc >= 2 && !strcmp(argv[1], "fdsetoob")) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0) return 3;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        return 0;                /* reached only if the guard is missing */
    }

    printf("pouch-hello-sockets: AF_UNIX SOCK_STREAM byte-mode /srv\n");

    if (test_family_refusals() != 0)     return 1;
    if (test_path_validation() != 0)     return 1;
    if (test_connect_nonexistent() != 0) return 1;
    if (test_round_trip() != 0)          return 1;
    if (test_fdset_guard() != 0)         return 1;

    /* The census is what joey matches: a stale binary prints the old marker. */
    puts(POUCH_CENSUS_SOCKETS);
    return 0;
}
