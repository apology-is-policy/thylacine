// srvconn — a kernel-minted /srv connection (P5-corvus-srv-impl-a3a).
//
// Per CORVUS-DESIGN.md §6.2 + §6.3. When a client Proc opens
// /srv/<name>, the kernel mints one SrvConn: a dedicated kernel↔server
// 9P transport plus the kernel-stamped peer identity. corvus
// (CORVUS-DESIGN §6) is the v1.0 consumer — it serves each client over
// that client's own SrvConn.
//
// What a3a (this chunk) lands: the SrvConn object, its bidirectional
// byte transport, and lifecycle/teardown. The devsrv walk/open path
// that mints a SrvConn on a client's open, the bounded accept queue,
// SYS_SRV_ACCEPT and SYS_SRV_PEER are P5-corvus-srv-impl-a3b / -a3c.
//
// The transport is two independent byte rings:
//   c2s — the kernel 9P client writes Tmsg bytes; corvus drains them.
//   s2c — corvus writes Rmsg bytes; the kernel 9P client drains them.
// The kernel 9P client is synchronous and single-frame-in-flight, so a
// ring sized to hold a whole msize frame (SRVCONN_RING_CAP) means a
// writer never blocks — a frame that would not fit is a protocol
// violation that tears the connection down. The one blocking wait is
// the kernel client draining s2c: it is `tsleep`-bounded
// (CORVUS-DESIGN §6.2) so a corvus that *hangs* times the client out
// rather than wedging it; a corvus that *crashes* wakes the client at
// once via the EOF that teardown sets.
//
// Peer identity (CORVUS-DESIGN §6.3) is captured BY VALUE at create —
// `stripes`, console bit, pid. The SrvConn holds no raw `Proc *` for
// the peer, so a peer that exits and is reaped never turns a SrvConn
// read into a use-after-free (the same discipline as the §6.3 service
// registry). SYS_SRV_PEER's live `caps` read (a3c) re-finds the Proc
// by `stripes` under the process-table lock.
//
// The connection also captures the SERVER's identity by value — the
// stripes of the service poster (corvus) at mint time. SYS_SRV_PEER
// (a3c) gates on it: only the poster may query a connection's peer
// (CORVUS-DESIGN §6.3). A by-value tag, not a `SrvService *` back-
// pointer, keeps the SrvConn free of cross-object lifetime coupling.
//
// Spec: no new module. The s2c blocking drain is a Rendez consumer
// with a deadline — the wait/wake protocol `specs/tsleep.tla` /
// `specs/scheduler.tla` (NoMissedWakeup) already pin; teardown's
// EOF-then-wake mirrors `specs/pipe.tla`'s CloseWrite. The transport
// is plumbing — cf. `kernel/include/thylacine/9p_transport.h` ("No new
// TLA+ spec at v1.0"). corvus.tla's connection layer
// (SrvBind/SrvAccept/SrvPeerOp) models identity and lifecycle, a level
// above these bytes.

#ifndef THYLACINE_SRVCONN_H
#define THYLACINE_SRVCONN_H

#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct p9_client;

// SRV_CONN_MAGIC — sentinel at offset 0 of struct SrvConn. Distinct
// from SRV_SERVICE_MAGIC (<thylacine/devsrv.h>) so the KObj_Srv
// handle-release path (P5-corvus-srv-impl-a3b) discriminates a
// connection object from a service object by the magic word. Cleared
// on free so a read of a freed SrvConn fast-fails (UAF defense).
#define SRV_CONN_MAGIC  0x535256434F4E4E00ULL    // 'SRVCONN' || 0x00

// The negotiated 9P msize for a /srv connection. Small: the corvus
// wire (CORVUS-DESIGN §6.4) carries short verb frames and the 1217-byte
// DEK envelope — 4 KiB is generous. Matches SYS_ATTACH_9P's default.
#define SRVCONN_MSIZE   4096u

// Per-direction ring capacity. Sized to comfortably hold one whole
// msize frame so the synchronous, single-frame-in-flight kernel 9P
// client never blocks on a write (see the file-header rationale).
#define SRVCONN_RING_CAP  8192u

_Static_assert(SRVCONN_RING_CAP >= SRVCONN_MSIZE,
               "a connection ring must hold a full msize frame so the "
               "synchronous kernel 9P client never blocks on a write");

// Connection lifecycle. A SrvConn is born LIVE and transitions once,
// to TORN, at teardown — a corvus crash/exit, a connection close, or a
// client-side deadline expiry. TORN is terminal; both byte rings carry
// EOF and every further send/blocking-recv fails fast.
enum srvconn_state {
    SRVCONN_STATE_LIVE = 1,
    SRVCONN_STATE_TORN = 2,
};

// One direction of the transport: a byte FIFO with a single blocking
// consumer. `eof` latches at teardown — the consumer then drains any
// residual bytes and reads EOF; the producer's writes fail. `rendez`
// is where the (single) blocking consumer waits; the single-waiter
// Rendez convention holds because the kernel 9P client serializes
// every op on `p9_client.lock`, so at most one thread drains a given
// ring at a time.
struct srvconn_chan {
    spin_lock_t    lock;          // protects count / head / tail / eof
    u32            count;         // bytes buffered; 0..SRVCONN_RING_CAP
    u32            head;          // next write index; mod SRVCONN_RING_CAP
    u32            tail;          // next read index; mod SRVCONN_RING_CAP
    bool           eof;           // teardown latched this direction
    struct Rendez  rendez;        // the single blocking consumer waits here
    u8             buf[SRVCONN_RING_CAP];
};

// A kernel-minted /srv connection. Allocated by srvconn_create; freed
// when the last reference is dropped (srvconn_unref). The embedded
// `client` is the dedicated synchronous kernel 9P client — kernel-
// owned, never placed in any handle table (invariant C-23).
struct SrvConn {
    u64                 magic;             // SRV_CONN_MAGIC; 0 once freed
    int                 ref;               // refcount; create → 1 (atomic)
    spin_lock_t         lock;              // protects `state`
    enum srvconn_state  state;

    // Peer identity — CORVUS-DESIGN §6.3, captured BY VALUE at create.
    u64                 peer_stripes;      // the peer Proc's stripes tag
    int                 peer_pid;          // the peer Proc's pid (diagnostics)
    bool                peer_console;      // peer's console-attachment bit

    // Server identity — the service poster's (corvus's) stripes tag at
    // mint, by value. SYS_SRV_PEER's poster gate compares it against the
    // caller's stripes (CORVUS-DESIGN §6.3).
    u64                 server_stripes;

    // Kernel-client-side blocking-recv deadline. Absolute ns on the
    // timer_now_ns timebase; 0 = no deadline (blocks indefinitely,
    // woken only by data or EOF). The op-driving path sets this before
    // each blocking op; srvconn_set_client_deadline also clears
    // `client_timed_out` so each op starts from a fresh signal.
    u64                 client_deadline_ns;
    bool                client_timed_out;  // last client recv hit the deadline

    struct srvconn_chan c2s;               // kernel client → corvus
    struct srvconn_chan s2c;               // corvus → kernel client

    // The dedicated synchronous kernel 9P client + its receive buffer.
    // Heap-allocated (the p9_client is large); owned by this SrvConn,
    // destroyed when the SrvConn is freed.
    struct p9_client   *client;
    u8                 *recv_buf;
};

_Static_assert(__builtin_offsetof(struct SrvConn, magic) == 0,
               "magic at offset 0 — the KObj_Srv handle-release "
               "discriminator reads the first u64, and a freed-SrvConn "
               "read must fast-fail on the cleared magic (UAF defense)");

// =============================================================================
// Lifecycle.
// =============================================================================

// srvconn_create — mint a connection. `peer_stripes` / `peer_pid` /
// `peer_console` are the opening client Proc's kernel-stamped identity;
// `server_stripes` is the service poster's (corvus's) stripes tag. All
// four are captured by value. Allocates the SrvConn, its receive buffer,
// and the embedded synchronous p9_client (configured over this
// connection's transport). The connection is born LIVE with refcount 1 —
// the caller owns that reference and drops it via srvconn_unref.
//
// Returns NULL on allocation failure (all-or-nothing — no partial
// state remains). The p9_client is initialized but NOT handshaken;
// driving Tversion/Tattach is the open path's job (a3b).
struct SrvConn *srvconn_create(u64 peer_stripes, int peer_pid,
                               bool peer_console, u64 server_stripes);

// srvconn_ref — take a reference. Extincts on a NULL / corrupted conn.
void srvconn_ref(struct SrvConn *cn);

// srvconn_unref — drop a reference. The last unref tears the
// connection down (idempotent), destroys the p9_client, and frees all
// storage. After the last unref the pointer is INVALID. NULL is a safe
// no-op. Extincts on refcount underflow.
void srvconn_unref(struct SrvConn *cn);

// srvconn_teardown — transition the connection to TORN: latch EOF on
// both byte rings and wake any blocked consumer. Idempotent — a second
// call is a no-op. Does NOT free the SrvConn (references may still be
// held); freeing happens at the last srvconn_unref. Maps to
// CORVUS-DESIGN §6.2's teardown ("sets the transport to ERROR and EOFs
// both pipe ends"). NULL is a safe no-op.
void srvconn_teardown(struct SrvConn *cn);

// srvconn_is_live — true iff `cn` is a valid SrvConn not yet torn down.
bool srvconn_is_live(const struct SrvConn *cn);

// =============================================================================
// Kernel 9P client.
// =============================================================================

// srvconn_client — the connection's dedicated synchronous kernel 9P
// client. Kernel-owned; never installed in a handle table (C-23).
// Returns NULL for a NULL / corrupted conn.
struct p9_client *srvconn_client(struct SrvConn *cn);

// srvconn_set_client_deadline — set the absolute deadline (timer_now_ns
// timebase) for the connection's next blocking client recv, and clear
// the `client_timed_out` signal. `deadline_ns == 0` means no deadline.
// The op-driving path calls this immediately before each blocking 9P
// op (a3b). Extincts on a NULL / corrupted conn.
void srvconn_set_client_deadline(struct SrvConn *cn, u64 deadline_ns);

// srvconn_client_timed_out — true iff the most recent blocking client
// recv ended on the deadline rather than on data or EOF. Lets the
// op-driving path map a transport failure to -ETIMEDOUT (corvus hung)
// vs -EIO (corvus crashed). Returns false for a NULL / corrupted conn.
bool srvconn_client_timed_out(const struct SrvConn *cn);

// =============================================================================
// Peer identity (SYS_SRV_PEER — P5-corvus-srv-impl-a3c).
//
// The connection's kernel-stamped identity fields, captured by value at
// mint (CORVUS-DESIGN §6.3). Each accessor revalidates SRV_CONN_MAGIC and
// fail-closes (0 / false) on a NULL or corrupted conn — a torn read can
// never produce a stale or fabricated identity.
// =============================================================================

// srvconn_peer_stripes — the peer (client) Proc's immutable stripes tag.
u64 srvconn_peer_stripes(const struct SrvConn *cn);

// srvconn_peer_console — the peer Proc's console-attachment bit at mint.
bool srvconn_peer_console(const struct SrvConn *cn);

// srvconn_server_stripes — the service poster's (corvus's) stripes tag at
// mint. SYS_SRV_PEER's poster gate compares this against the caller.
u64 srvconn_server_stripes(const struct SrvConn *cn);

// =============================================================================
// Raw byte transport.
//
// The kernel 9P client reaches the transport through a p9_transport_ops
// vtable that wraps srvconn_client_send / srvconn_client_recv. corvus
// (the server) reaches its side through srvconn_server_send /
// srvconn_server_recv — the calls the devsrv connection-Spoor read/
// write ops route to (a3b).
// =============================================================================

// srvconn_client_send — kernel client writes `n` bytes toward corvus
// (the c2s ring). Returns bytes accepted (0..n; in correct single-
// frame-in-flight operation always n), or -1 if the connection is torn
// down or args are bad. Non-blocking.
long srvconn_client_send(struct SrvConn *cn, const u8 *buf, long n);

// srvconn_client_recv — kernel client reads up to `n` bytes from corvus
// (the s2c ring), BLOCKING until data arrives, the connection is torn
// down, or `client_deadline_ns` passes. Returns:
//   >0  — bytes read.
//    0  — EOF: the connection is torn down and no residual bytes remain.
//   -1  — the deadline passed (then srvconn_client_timed_out is true),
//         or args are bad.
long srvconn_client_recv(struct SrvConn *cn, u8 *buf, long n);

// srvconn_server_send — corvus writes `n` bytes toward the kernel
// client (the s2c ring). Returns bytes accepted (0..n) or -1 if torn
// down / bad args. Non-blocking.
long srvconn_server_send(struct SrvConn *cn, const u8 *buf, long n);

// srvconn_server_recv — corvus reads up to `n` bytes from the kernel
// client (the c2s ring). Non-blocking — corvus polls. Returns:
//   >0  — bytes read.
//    0  — the ring is empty but the connection is live (poll again).
//   -1  — the connection is torn down (and the ring is drained): EOF.
long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n);

// =============================================================================
// Diagnostics.
// =============================================================================

// Cumulative counters. (created - freed) == live SrvConn count.
u64 srvconn_total_created(void);
u64 srvconn_total_freed(void);

#endif  // THYLACINE_SRVCONN_H
