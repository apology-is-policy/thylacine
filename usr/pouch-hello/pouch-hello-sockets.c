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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char SOCK_PATH[]    = "/srv/pouch-sock-demo";
static const char NONEXIST_PATH[] = "/srv/pouch-sock-nonex";
static const char MSG_PING[]     = "PING\n";
static const char MSG_PONG[]     = "PONG\n";

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
        fprintf(stderr, "test: AF_INET6 should have refused with "
                "EAFNOSUPPORT, got s=%d errno=%d\n", s, errno);
        if (s >= 0) close(s);
        return -1;
    }
    printf("test: socket(AF_INET6) refused EAFNOSUPPORT ok\n");

    s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s >= 0 || errno != EPROTONOSUPPORT) {
        fprintf(stderr, "test: SOCK_DGRAM should have refused with "
                "EPROTONOSUPPORT, got s=%d errno=%d\n", s, errno);
        if (s >= 0) close(s);
        return -1;
    }
    printf("test: socket(SOCK_DGRAM) refused EPROTONOSUPPORT ok\n");

    s = socket(AF_UNIX, SOCK_STREAM, 42);
    if (s >= 0 || errno != EPROTONOSUPPORT) {
        fprintf(stderr, "test: protocol=42 should have refused with "
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
        fprintf(stderr, "test: socket failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, NONEXIST_PATH, sizeof(NONEXIST_PATH));

    int r = connect(c, (struct sockaddr *)&addr, sizeof(addr));
    if (r == 0) {
        fprintf(stderr, "test: connect(%s) UNEXPECTEDLY succeeded\n",
                NONEXIST_PATH);
        close(c);
        return -1;
    }
    if (errno != ECONNREFUSED) {
        fprintf(stderr, "test: connect(%s) wrong errno=%d (want "
                "ECONNREFUSED)\n", NONEXIST_PATH, errno);
        close(c);
        return -1;
    }
    printf("test: connect(%s) refused ECONNREFUSED ok\n", NONEXIST_PATH);

    if (close(c) != 0) {
        fprintf(stderr, "test: close(fresh slot) failed errno=%d\n", errno);
        return -1;
    }
    printf("test: close(fresh slot) ok\n");

    return 0;
}

static int test_path_validation(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "test: socket failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, "/usr/foo", sizeof("/usr/foo"));
    int r = bind(s, (struct sockaddr *)&addr, sizeof(addr));
    if (r == 0 || errno != EINVAL) {
        fprintf(stderr, "test: bind(/usr/foo) should have rejected "
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
        fprintf(stderr, "server: socket failed errno=%d\n", errno);
        pthread_barrier_wait(&g_ready);
        return NULL;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCK_PATH, sizeof(SOCK_PATH));

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "server: bind failed errno=%d\n", errno);
        close(s);
        pthread_barrier_wait(&g_ready);
        return NULL;
    }
    if (listen(s, 1) != 0) {
        fprintf(stderr, "server: listen failed errno=%d\n", errno);
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
        fprintf(stderr, "server: accept failed errno=%d\n", errno);
        close(s);
        return NULL;
    }
    printf("server: accept ok, conn fd=%d\n", conn);

    char buf[64];
    ssize_t n = read(conn, buf, sizeof(buf) - 1);
    if (n != (ssize_t)(sizeof(MSG_PING) - 1)) {
        fprintf(stderr, "server: read got %zd want %zu errno=%d\n",
                n, sizeof(MSG_PING) - 1, errno);
        close(conn); close(s);
        return NULL;
    }
    buf[n] = '\0';
    if (memcmp(buf, MSG_PING, sizeof(MSG_PING) - 1) != 0) {
        fprintf(stderr, "server: read got '%s' want '%s'\n", buf, MSG_PING);
        close(conn); close(s);
        return NULL;
    }
    printf("server: read PING (%zd bytes byte-accurate) ok\n", n);

    ssize_t w = write(conn, MSG_PONG, sizeof(MSG_PONG) - 1);
    if (w != (ssize_t)(sizeof(MSG_PONG) - 1)) {
        fprintf(stderr, "server: write got %zd want %zu errno=%d\n",
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
        fprintf(stderr, "server: getsockopt(SO_PEERCRED) errno=%d\n", errno);
        close(conn); close(s);
        return NULL;
    }
    if (cred.pid == 0) {
        fprintf(stderr, "server: peer pid is zero (unexpected)\n");
        close(conn); close(s);
        return NULL;
    }
    printf("server: SO_PEERCRED pid=%d uid=%u gid=%u\n",
           cred.pid, cred.uid, cred.gid);

    close(conn);
    close(s);
    g_server_ok = 1;
    return NULL;
}

static int test_round_trip(void)
{
    if (pthread_barrier_init(&g_ready, NULL, 2) != 0) {
        fprintf(stderr, "test: pthread_barrier_init failed\n");
        return -1;
    }

    pthread_t srv;
    if (pthread_create(&srv, NULL, server_main, NULL) != 0) {
        fprintf(stderr, "test: pthread_create failed\n");
        return -1;
    }

    pthread_barrier_wait(&g_ready);
    printf("client: server ready, connecting\n");

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c < 0) {
        fprintf(stderr, "client: socket failed errno=%d\n", errno);
        pthread_join(srv, NULL);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, SOCK_PATH, sizeof(SOCK_PATH));

    if (connect(c, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "client: connect failed errno=%d\n", errno);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    printf("client: connect ok at %s, conn fd=0x%x\n", SOCK_PATH, c);

    ssize_t w = write(c, MSG_PING, sizeof(MSG_PING) - 1);
    if (w != (ssize_t)(sizeof(MSG_PING) - 1)) {
        fprintf(stderr, "client: write got %zd want %zu errno=%d\n",
                w, sizeof(MSG_PING) - 1, errno);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    printf("client: write PING ok\n");

    char buf[64];
    ssize_t n = read(c, buf, sizeof(buf) - 1);
    if (n != (ssize_t)(sizeof(MSG_PONG) - 1)) {
        fprintf(stderr, "client: read got %zd want %zu errno=%d\n",
                n, sizeof(MSG_PONG) - 1, errno);
        close(c);
        pthread_join(srv, NULL);
        return -1;
    }
    buf[n] = '\0';
    if (memcmp(buf, MSG_PONG, sizeof(MSG_PONG) - 1) != 0) {
        fprintf(stderr, "client: read got '%s' want '%s'\n", buf, MSG_PONG);
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
            fprintf(stderr, "client: getsockopt(SO_PEERCRED) UNEXPECTEDLY "
                    "succeeded on client-side fd\n");
            close(c);
            pthread_join(srv, NULL);
            return -1;
        }
        if (errno != ENOTSOCK) {
            fprintf(stderr, "client: getsockopt(SO_PEERCRED) wrong errno=%d "
                    "(want ENOTSOCK on client-side fd at v1.0)\n", errno);
            close(c);
            pthread_join(srv, NULL);
            return -1;
        }
        printf("client: SO_PEERCRED client-side returns ENOTSOCK at v1.0 ok\n");
    }

    close(c);
    pthread_join(srv, NULL);
    pthread_barrier_destroy(&g_ready);

    if (!g_server_ok) {
        fprintf(stderr, "test: server thread reported failure\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    printf("pouch-hello-sockets: AF_UNIX SOCK_STREAM byte-mode /srv\n");

    if (test_family_refusals() != 0)     return 1;
    if (test_path_validation() != 0)     return 1;
    if (test_connect_nonexistent() != 0) return 1;
    if (test_round_trip() != 0)          return 1;

    printf("pouch-hello-sockets: exit 0\n");
    return 0;
}
