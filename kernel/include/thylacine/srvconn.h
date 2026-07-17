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
// Each ring is sized to hold a whole msize frame (cap = 2x the conn's
// msize; CF-3 B sizes it per service class), so a frame that would not
// fit an EMPTY ring is a protocol violation that tears the connection
// down; a frame that transiently does not fit a BUSY ring is
// back-pressure, absorbed by blocking (#348/#349 flow control on all
// three producers). The kernel client draining s2c is `tsleep`-bounded
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

#include <thylacine/poll.h>
#include <thylacine/rendez.h>
#include <thylacine/spinlock.h>
#include <thylacine/types.h>

struct poll_waiter;

// SRV_CONN_MAGIC — sentinel at offset 0 of struct SrvConn. Distinct
// from SRV_SERVICE_MAGIC (<thylacine/devsrv.h>) so the KObj_Srv
// handle-release path (P5-corvus-srv-impl-a3b) discriminates a
// connection object from a service object by the magic word. Cleared
// on free so a read of a freed SrvConn fast-fails (UAF defense).
#define SRV_CONN_MAGIC  0x535256434F4E4E00ULL    // 'SRVCONN' || 0x00

// The DEFAULT 9P msize for a /srv connection -- the per-op payload
// ceiling for a SrvConn-backed 9P session whose service did not opt into
// bulk rings (CF-3 B). corvus's wire (CORVUS-DESIGN §6.4) sends only
// short verb frames + the 1217-byte DEK envelope; netd's /srv/net rides
// the same default. 32 KiB keeps the per-conn ring memory modest for the
// many small-frame services (msize is a max -- their frames stay small).
#define SRVCONN_MSIZE   (32u * 1024u)

// The BULK 9P msize (CF-3 B; CONCURRENT-FS.md CF-3): the per-service
// opt-in for high-throughput FS services. A service posted with the
// DMSRVBULK create-perm bit mints connections whose rings hold 128 KiB
// frames, so the kernel dev9p client proposes (and stratumd accepts --
// STM_9P_MSIZE_DEFAULT is exactly 128 KiB) a 128 KiB msize: one bulk
// read/write RPC then carries 4x the default payload, on top of the
// CF-3 A SYS_RW_MAX lift. Sized to SYS_RW_MAX so a single max-size
// byte-I/O syscall maps to one RPC (minus the 9P frame overhead).
// Exactly two ring classes exist at v1.0 -- srv_reserve validates
// against this pair, so a hostile poster cannot demand arbitrary ring
// memory.
#define SRVCONN_BULK_MSIZE  (128u * 1024u)

// The 9P root fid for a SrvConn-backed session (P9 convention; matches
// SYS_ATTACH_DEFAULT_ROOT_FID). srvconn_attach_dev9p_root (9p_attach.c)
// Tattaches the dev9p root here. Promoted to the header (stalk-3b-β) so
// the shared attach helper can pass the same root fid.
#define SRVCONN_ROOT_FID  1u

// DEFAULT per-direction ring capacity (2x the default msize). Every
// conn's ring is sized 2x its service's msize (CF-3 B: srvconn_create
// computes cap = 2 * msize) so it comfortably holds one whole msize
// frame plus a second in flight -- the #841 pipelined kernel 9P client
// can have several frames outstanding, and a frame that fits an empty
// ring is the all-or-nothing send's floor. The ring buffers are HEAP
// allocations owned by the SrvConn (a default conn carries 2 x 64 KiB,
// a DMSRVBULK conn 2 x 256 KiB; NOT #65-charged -- bounded by
// SRV_MAX_CONNS, see devsrv.h). This macro is the DEFAULT-class value
// (tests + the default-msize maths); per-conn code reads ch->cap.
#define SRVCONN_RING_CAP  (2u * SRVCONN_MSIZE)

_Static_assert(SRVCONN_RING_CAP >= SRVCONN_MSIZE,
               "a connection ring must hold a full msize frame so the "
               "synchronous kernel 9P client never blocks on a write");
_Static_assert((2u * SRVCONN_BULK_MSIZE) >= SRVCONN_BULK_MSIZE,
               "the bulk ring must hold a full bulk msize frame");

// P5-corvus-srv-impl-b2: max bytes of a path component the open path's
// Twalk consumes. v1.0 has one-component paths (e.g. "ctl", "ops"); the
// 64-byte cap is comfortable headroom for any future single-component
// path corvus serves under /srv/<name>/. The syscall-level path buffer
// (sys_srv_connect_handler) is sized to this.
#define SRVCONN_PATH_MAX  64u

// Per-op client-side deadlines (ns). The kernel's synchronous p9_client
// dispatches every blocking 9P op through `srvconn_client_recv`, which
// is `tsleep`-bounded by `cn->client_deadline_ns`. Each production site
// that initiates a blocking op MUST set a fresh deadline first
// (`srvconn_set_client_deadline(cn, timer_now_ns() + DEADLINE_NS)`) —
// otherwise the default 0 reads as "no deadline" and a hung corvus
// wedges the caller indefinitely.
//
// Handshake (Tversion + Tattach + optional Twalk + Tlopen on a brand-
// new connection): the kernel does no userspace crypto on this path;
// every op is small (≤ msize) and corvus's server side just shuffles
// 9P frames. 5 seconds is comfortable headroom over QEMU-emulated
// AArch64's worst-case scheduling jitter without papering over a
// legitimately-hung server. Caller path:
// `srvconn_attach_dev9p_root` (open=connect / SYS_ATTACH_9P_SRV).
#define SRVCONN_HANDSHAKE_DEADLINE_NS  (5ull * 1000ull * 1000ull * 1000ull)

// Steady-state read/write (after handshake): corvus may run Argon2id
// (m_cost=16 MiB) + AEGIS-256 + ML-KEM-768 in a single verb dispatch
// before staging the response. 30 seconds covers the worst-case
// userspace crypto budget on emulated targets; on hardware verb
// dispatch completes in well under a second. Caller paths:
// `sys_read_for_proc` / `sys_write_for_proc` KOBJ_SRV arms.
#define SRVCONN_OP_DEADLINE_NS  (30ull * 1000ull * 1000ull * 1000ull)

// Connection lifecycle. A SrvConn is born LIVE and transitions once,
// to TORN, at teardown — a corvus crash/exit, a connection close, or a
// client-side deadline expiry. TORN is terminal; both byte rings carry
// EOF and every further send/blocking-recv fails fast.
enum srvconn_state {
    SRVCONN_STATE_LIVE = 1,
    SRVCONN_STATE_TORN = 2,
};

// One direction of the transport: a byte FIFO with a single blocking
// consumer ROLE and a single blocking producer ROLE. `eof` latches
// at teardown — the consumer then drains any residual bytes and reads
// EOF; the producer's writes fail. `rendez` is where the (single)
// role-holding consumer waits; `wrendez` is where the (single)
// role-holding producer waits for ring room (#348 s2c back-pressure).
// The two are SEPARATE Rendezes so a reader and a writer never contend
// one single-waiter slot; the single-waiter convention then holds
// trivially — each Rendez has exactly one possible waiter: the current
// holder of the `reading` / `writing` role.
//
// #354 (CF-3 B): role CONTENTION no longer fails. A 2nd concurrent
// blocking party parks on `role_waiters` (each on its OWN stack Rendez
// via a poll_waiter — the #349 send_waiters_list multi-waiter pattern)
// until the holder releases, then re-contends. So N threads may
// blocking-read (or blocking-write) one direction concurrently: reads
// serialize per CALL (each recv consumes one lock-atomic chunk), writes
// serialize per BUFFER (the role is held across the whole multi-chunk
// delivery — frame/call atomicity vs any other producer). Pre-#354 the
// 2nd party was refused (-1), which a POSIX server's write_full treated
// as fatal — the mount-close cascade the #348 audit flagged (F1).
struct srvconn_chan {
    spin_lock_t    lock;          // protects count / head / tail / eof + roles
    u32            cap;           // ring capacity (2x the conn's msize; CF-3 B)
    u32            count;         // bytes buffered; 0..cap
    u32            head;          // next write index; mod cap
    u32            tail;          // next read index; mod cap
    bool           eof;           // teardown latched this direction
    bool           reading;       // the single blocking-consumer ROLE (RW-4
                                  // R2-F1): the holder is the only thread that
                                  // may park on `rendez`. Contenders park on
                                  // `role_waiters` until release (#354).
    bool           writing;       // the single blocking-PRODUCER role (#348):
                                  // symmetric to `reading`; the holder is the
                                  // only thread that may park on `wrendez`.
                                  // Held across the WHOLE buffer delivery so
                                  // bytes of one call never interleave with
                                  // another producer's (frame atomicity).
    struct Rendez  rendez;        // the role-holding consumer waits here
    struct Rendez  wrendez;       // the role-holding producer waits here, for
                                  // ring room (FULL -> has-room). A SEPARATE
                                  // Rendez from `rendez` so the reader and the
                                  // writer never share a single-waiter slot.
    struct poll_waiter_list role_waiters;  // #354: parked role CONTENDERS
                                  // (read + write share the list; each woken
                                  // party re-checks its own role under lock
                                  // and re-parks if still held). Woken on
                                  // every role release + at teardown.
    u8            *buf;           // heap ring storage, `cap` bytes; owned by
                                  // the SrvConn (freed at the last unref)
};

// A kernel-minted /srv connection. Allocated by srvconn_create; freed
// when the last reference is dropped (srvconn_unref). Holds the
// bidirectional byte transport (c2s / s2c) + the kernel-stamped peer
// and server identity. A 9P-mode connection is driven by a SEPARATE,
// caller-owned kernel 9P client wrapping these rings via
// p9_srvconn_transport (srvconn_attach_dev9p_root) — the SrvConn itself
// is pure transport + identity (stalk-3b-β retired the old embedded
// per-SrvConn p9_client).
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

    // The conn's 9P msize class (CF-3 B), captured from the SERVICE at
    // mint (SRVCONN_MSIZE default; SRVCONN_BULK_MSIZE for a DMSRVBULK
    // post). Both rings are sized 2x this. srvconn_attach_dev9p_root
    // reads it (srvconn_msize) as the kernel client's msize proposal +
    // recv cap, so the negotiated session msize can never exceed what
    // the rings carry. Immutable for the conn's lifetime.
    u32                 msize;

    // Kernel-client-side blocking-recv deadline. Absolute ns on the
    // timer_now_ns timebase; 0 = no deadline (blocks indefinitely,
    // woken only by data or EOF). The op-driving path sets this before
    // each blocking op; srvconn_set_client_deadline also clears
    // `client_timed_out` so each op starts from a fresh signal.
    u64                 client_deadline_ns;
    bool                client_timed_out;  // last client recv hit the deadline

    struct srvconn_chan c2s;               // kernel client → corvus
    struct srvconn_chan s2c;               // corvus → kernel client

    // Pollers registered on this connection's server endpoint (P5-poll-b).
    // The hook list is connection-wide because the server endpoint Spoor
    // is bidirectional — POLLIN watches c2s, POLLOUT watches s2c, and a
    // teardown surfaces POLLHUP/POLLERR on both. Every producer site
    // (srvconn_client_send + srvconn_server_send into chan_produce,
    // srvconn_teardown's chan_set_eof on both directions) calls
    // poll_waiter_list_wake(&poll_list) after releasing the channel lock,
    // mirroring kernel/pipe.c's discipline (specs/poll.tla MakeReady).
    struct poll_waiter_list poll_list;

    // P6-pouch-sockets (sub-chunk 12): transport mode propagated from
    // the service at mint (read service->mode under the registry lock;
    // srvconn_set_byte_mode is the one-way setter). FALSE (default) is
    // SRV_MODE_9P; TRUE is SRV_MODE_BYTE.
    //
    // Post stalk-3b-β the two modes diverge at the connect path
    // (devsrv_open_connect), not at per-op dispatch:
    //   - SRV_MODE_9P  — devsrv_open_connect wraps the SrvConn in a
    //     kernel 9P client (srvconn_attach_dev9p_root) and returns a
    //     dev9p root Spoor; the client never sees a KObj_Srv handle.
    //   - SRV_MODE_BYTE — devsrv_open_connect returns a CSRVCLIENT
    //     KOBJ_SPOOR byte-conn Spoor, routing raw bytes through
    //     srvconn_client_send/recv against c2s / s2c (no 9P framing).
    // The setter is called BEFORE the SrvConn is enqueued (so an
    // accepting server never observes a mode-mid-flight conn).
    //
    // F5 close (P6-pouch-sockets audit): the setter uses
    // __atomic_store_n(.., __ATOMIC_RELEASE) and every reader (devsrv_
    // read, sys_read/write_for_proc's KOBJ_SRV arm, sys_srv_connect_
    // for_proc's byte-mode gate) uses __atomic_load_n(.., __ATOMIC_
    // ACQUIRE). The release-acquire pairing ensures cross-CPU observers
    // see the field consistently with the SrvConn publication; the
    // pre-publication property still holds for current observers
    // (which all reach cn through a lock-paired path).
    bool                byte_mode;

    // P6-pouch-stratumd-boot 16c: kernel-attached gate.
    //
    // SYS_ATTACH_9P_SRV wraps a byte-mode SrvConn in a kernel-owned 9P
    // client (via p9_srvconn_transport). After this call, the SrvConn's
    // c2s / s2c rings are LOAD-BEARING for the kernel client — any
    // teardown (chan_set_eof on either ring) breaks all subsequent
    // Twalk / Tread / Twrite sends.
    //
    // The default handle_close discipline for KOBJ_SRV is "close the
    // connection: srvconn_teardown both rings, then srvconn_unref"
    // (CORVUS-DESIGN.md section 6.2). For a kernel-attached SrvConn,
    // teardown would break the FS attach -- the userspace handle close
    // is releasing the user's view but the kernel client is still using
    // the rings. With kernel_attached=true, handle_close skips
    // srvconn_teardown; only srvconn_unref runs. The connection lives
    // until the LAST holder (the adapter) drops its ref via
    // p9_attached_destroy's transport.close, which runs srvconn_unref
    // and lets the refcount hit 0.
    //
    // One-way (false -> true), set by SYS_ATTACH_9P_SRV AFTER all
    // failure paths have been cleared and BEFORE the handle could be
    // closed. Atomic store-release pairs with handle_close's atomic
    // load-acquire (cross-CPU userspace close can race the syscall's
    // setter; the release-acquire ordering ensures the close either
    // sees the flag set, or sees it clear AND the syscall has not yet
    // returned the attach_fd to userspace).
    bool                kernel_attached;
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
// four are captured by value. `msize` is the conn's 9P msize class
// (SRVCONN_MSIZE or SRVCONN_BULK_MSIZE, from the service's ring class --
// CF-3 B; any other value is rejected NULL); each ring is heap-allocated
// at 2x msize. Allocates the SrvConn + initializes its byte transport
// (c2s / s2c) + poll list. The connection is born LIVE with refcount 1 —
// the caller owns that reference and drops it via srvconn_unref.
//
// Returns NULL on allocation failure or a bad msize. A 9P-mode session
// over this connection is the caller's responsibility to construct
// (srvconn_attach_dev9p_root wraps the rings in a kernel 9P client).
struct SrvConn *srvconn_create(u64 peer_stripes, int peer_pid,
                               bool peer_console, u64 server_stripes,
                               u32 msize);

// srvconn_msize — the conn's 9P msize class (set at mint from the
// service's ring class). Fail-closes to SRVCONN_MSIZE on a NULL /
// corrupted conn so a defensive caller never proposes past the rings.
u32 srvconn_msize(const struct SrvConn *cn);

// srvconn_server_recv_blocking — F1 close (P6-pouch-sockets audit).
// Blocking variant of srvconn_server_recv; mirrors srvconn_client_recv
// against c2s. Used by `devsrv_read` for byte-mode SrvConns (the POSIX
// AF_UNIX SOCK_STREAM blocking-read expectation). 9P-mode SrvConns
// keep the non-blocking srvconn_server_recv (corvus's poll-then-read
// pattern). Returns bytes read (>=0) or -1 on bad args / torn cn.
long srvconn_server_recv_blocking(struct SrvConn *cn, u8 *buf, long n);

// srvconn_set_byte_mode — one-way setter for cn->byte_mode = true.
// Set at conn mint in devsrv_open_connect (from the posted service's
// mode) BEFORE the SrvConn is enqueued in the accept backlog, so an
// accepting server + the open=connect path both read a stable flag.
//
// Idempotent (sets the flag; never clears). No lock needed: the field
// is captured at mint (BEFORE any other observer can see the cn) and
// is then read-only for the cn's lifetime. Subsequent observers see a
// consistent value.
//
// `cn` must be a freshly-minted SrvConn from srvconn_create — undefined
// behavior on a NULL or already-published SrvConn.
void srvconn_set_byte_mode(struct SrvConn *cn);

// srvconn_set_kernel_attached — one-way setter for cn->kernel_attached.
// P6-pouch-stratumd-boot 16c. Called from sys_attach_9p_srv_handler
// AFTER all failure paths have cleared and BEFORE the syscall returns
// the attach_fd to userspace. Atomic store-release pairs with the
// handle_close path's atomic load-acquire.
//
// Once set, handle_close on the KOBJ_SRV handle SKIPS srvconn_teardown
// (so the c2s/s2c rings stay live for the kernel 9P client). The
// connection is torn down by the adapter's close hook running at
// p9_attached_destroy time -- triggered when the LAST KOBJ_SPOOR handle
// referencing the attach session is closed.
void srvconn_set_kernel_attached(struct SrvConn *cn);

// srvconn_is_kernel_attached — atomic load-acquire read of the flag.
// Returns the kernel_attached state; pairs with the setter's release.
bool srvconn_is_kernel_attached(const struct SrvConn *cn);

// srvconn_ref — take a reference. Extincts on a NULL / corrupted conn.
void srvconn_ref(struct SrvConn *cn);

// srvconn_unref — drop a reference. The last unref tears the
// connection down (idempotent) and frees all storage. After the last
// unref the pointer is INVALID. NULL is a safe no-op. Extincts on
// refcount underflow.
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
// Client-side blocking-recv deadline.
// =============================================================================

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

// srvconn_client_send_frame — ALL-OR-NOTHING send of one whole 9P frame
// (#841). The pipelined kernel 9P client (ARCH §21.10) may have several frames
// in flight, so c2s can transiently hold a prior undrained frame; a partial
// write would leave a fragment on the wire and desync the shared stream. This
// writes the WHOLE `n`-byte frame iff it fits the c2s ring right now, else
// writes nothing. Returns:
//   n   — the whole frame was written.
//   0   — no room (the caller fails the op + marks the session dead; no
//         fragment is ever left on the wire).
//  -1   — the connection is torn (EOF) or args are bad, or n > the ring (a
//         framing-layer bug -- a frame can never exceed msize <= ring cap).
// Non-blocking. Frame-atomicity vs concurrent senders is the caller's
// responsibility (the kernel 9P client holds c->lock across the send).
long srvconn_client_send_frame(struct SrvConn *cn, const u8 *buf, long n);

// srvconn_client_recv — kernel client reads up to `n` bytes from corvus
// (the s2c ring), BLOCKING until data arrives, the connection is torn
// down, or `client_deadline_ns` passes. A concurrent blocking reader
// parks until the current one finishes (#354 role-park; the same
// deadline bounds the role wait). Returns:
//   >0  — bytes read.
//    0  — EOF: the connection is torn down and no residual bytes remain.
//   -1  — the deadline passed (then srvconn_client_timed_out is true),
//         a #811 death-interrupt, or args are bad.
long srvconn_client_recv(struct SrvConn *cn, u8 *buf, long n);

// srvconn_client_send_blocking — the BLOCKING client-side byte write
// (CF-3 B; the c2s twin of #348's srvconn_server_send_blocking). The
// non-blocking srvconn_client_send above short-writes (or returns 0) on
// a full c2s ring; a POSIX byte client — a pouch AF_UNIX writer, or the
// per-user stratumd proxy forwarding a Tmsg upstream through its conn
// Spoor — treats a 0 from write() as EPIPE and closes (the #348 cascade
// on the third producer). This variant delivers the WHOLE buffer,
// parking on c2s.wrendez until the SERVER drains c2s
// (srvconn_server_recv / _blocking wake wrendez on drain) or teardown
// latches eof. Return contract identical to srvconn_server_send_blocking
// (n / partial-then-eof / -1). Death-interruptible (#811). Role-parked
// vs a concurrent blocking producer (#354): the whole buffer enters c2s
// contiguously. Both-directions-full with a peer that never reads is
// the classic full-duplex application deadlock POSIX AF_UNIX has too —
// death/teardown still unwinds it.
long srvconn_client_send_blocking(struct SrvConn *cn, const u8 *buf, long n);

// srvconn_server_send — corvus writes `n` bytes toward the kernel
// client (the s2c ring). Returns bytes accepted (0..n) or -1 if torn
// down / bad args. Non-blocking.
long srvconn_server_send(struct SrvConn *cn, const u8 *buf, long n);

// srvconn_server_send_blocking — the BLOCKING server-side write (#348).
//
// The non-blocking srvconn_server_send above can short-write (or return 0)
// when the s2c ring is full. A POSIX server endpoint — a 9P-server Proc
// like stratumd writing reply frames through devsrv_write — treats a 0
// from write() as EPIPE and CLOSES the connection, killing the kernel-
// attached mount mid-build (the #348 s2c back-pressure, the s2c twin of
// the #349 c2s fix). This blocking variant instead delivers the WHOLE
// buffer: it writes what fits, then parks on s2c.wrendez until the kernel
// client drains s2c (srvconn_client_recv wakes wrendez) or teardown
// latches eof. Returns:
//   n   — all bytes delivered.
//   >0  — partial then eof (the connection tore down mid-write).
//   -1  — eof before any byte / bad args / #811 death-interrupt.
// Death-interruptible (#811): a dying Proc unwinds at its EL0-return
// die-check. Frame-atomicity vs a concurrent producer is the `writing`
// role's: it is held across the WHOLE multi-chunk delivery, and a 2nd
// concurrent blocking producer PARKS on the role list until release
// (#354 close; pre-#354 it was refused -1, which a POSIX server's
// write_full treated as fatal -> mount close -- the #348-audit F1
// latent, live once stratumd went threaded at CF-2). A partial return
// after a death-interrupt is safe-by-construction (audit F2): the dying
// Proc unwinds at its next EL0 return before any write_full retry can
// re-enter, so the partial count is never re-driven from a live caller.
long srvconn_server_send_blocking(struct SrvConn *cn, const u8 *buf, long n);

// srvconn_server_recv — corvus reads up to `n` bytes from the kernel
// client (the c2s ring). Non-blocking — corvus polls. Returns:
//   >0  — bytes read.
//    0  — the ring is empty but the connection is live (poll again).
//   -1  — the connection is torn down (and the ring is drained): EOF.
long srvconn_server_recv(struct SrvConn *cn, u8 *buf, long n);

// =============================================================================
// poll — readiness probe on the server endpoint Spoor (P5-poll-b).
//
// The Dev `.poll` slot for a devsrv connection Spoor (devsrv_poll) routes
// to here. Server-endpoint semantics: POLLIN is c2s.count > 0 (bytes to
// read), POLLOUT is !s2c.eof && s2c.count < s2c.cap (room to
// write), POLLHUP is c2s.eof, POLLERR is s2c.eof. Both EOFs latch
// together at srvconn_teardown so HUP and ERR fire on the same edge.
// POLLIN may coexist with POLLHUP — POSIX: buffered bytes plus EOF.
//
// Atomic register-then-sample under both channel locks (c2s then s2c —
// fixed order, no path takes them in reverse). pw == NULL is the post-
// wake sample-only call. Returns the masked revents (POLLIN/POLLOUT
// gated by `events`, output-only bits always returned).
// =============================================================================

short srvconn_poll(struct SrvConn *cn, short events, struct poll_waiter *pw);

// =============================================================================
// Diagnostics.
// =============================================================================

// Cumulative counters. (created - freed) == live SrvConn count.
u64 srvconn_total_created(void);
u64 srvconn_total_freed(void);

#endif  // THYLACINE_SRVCONN_H
