// The native C adapter: drives one private service ring from EL0 over the
// client cores (docs/ASYNC-SERVICE-ABI.md, "Native adapter"). Creator thread
// only: a forked child must neither use nor drop the ring, since fork carries
// neither its mapping nor its descriptor and the number may already name one
// of the child's own files. Each operation is an opaque handle: submit returns it, the ring's turn
// progresses it, a completion call ends it, and dropping it while it is pending
// cancels it, which aborts its whole scope.
#ifndef THYLA_LOOM_CLIENT_ADAPTER_H
#define THYLA_LOOM_CLIENT_ADAPTER_H
#include <thyla/loom.h>
#include <thyla/loom_client/fixed_requests.h>
#include <thyla/loom_client/owned_mapping.h>

// The raw kernel calls a private ring needs. Each is one system call and returns
// the kernel's answer unchanged: a value >= 0, or a negative errno. attach
// returns a fresh, eager, physically contiguous, zeroed read-write region (a
// Loom registered buffer) of a page multiple, and detach removes exactly the
// region it names. poll answers the returned events (> 0), 0 on timeout, or a
// negative errno. now is CLOCK_MONOTONIC in nanoseconds, the clock every Loom
// deadline is on. Only poll may block, and no call reenters the adapter.
struct t_loom_kernel {
    void *context;
    int64_t (*setup)(void *, uint32_t entries, struct t_owned_setup *);
    int64_t (*reg)(void *, int32_t fd, uint32_t op, void *arg, uint32_t nargs);
    int64_t (*enter)(void *, int32_t fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags);
    int64_t (*poll)(void *, int32_t fd, int16_t events, int32_t timeout_ms);
    int64_t (*close)(void *, int32_t fd);
    int64_t (*attach)(void *, size_t len);
    int64_t (*detach)(void *, uintptr_t va, size_t len);
    uint64_t (*now)(void *);
};
// libt's table, over its system-call stubs.
struct t_loom_kernel t_loom_kernel_native(void);

struct t_loom_ring;
// Handles into the ring's storage. Never copy one, and never use one again
// after the call that consumes or drops it: while its cell is free that ends
// the process, but a reused cell names another handle's object.
struct t_loom_target { struct t_loom_ring *ring; uint32_t slot; };
struct t_loom_scope { struct t_loom_ring *ring; uint32_t slot; };
struct t_loom_fid { struct t_loom_ring *ring; uint32_t slot; };
struct t_loom_pool { struct t_loom_ring *ring; uint32_t cell; };
struct t_loom_op;

// The adapter's own tables; their fields are the adapter's.
struct t_loom_slot {
    struct t_loom_service_ref ref, scope;
    unsigned kind, state, aborted, handle;
    // A scope: its handle, its fids' handles and the operation handles on it.
    size_t refs;
    // A fid's background CLUNK, while it is in flight.
    struct t_fixed_book clunk;
    unsigned clunking;
    struct t_loom_target target;
    struct t_loom_scope scope_handle;
    struct t_loom_fid fid;
};
struct t_loom_pool_cell {
    struct t_ring_pool *core;
    struct t_loom_service_ref ref;
    // The pool handle, and every stream whose route still names the pool.
    size_t refs, members;
    unsigned handle;
    struct t_loom_pool pool;
};
struct t_loom_op {
    struct t_loom_ring *ring;
    unsigned kind, handle, route, ended;
    uint32_t scope, pool;
    // CONNECT's scope or WALK's fid, while it is still the operation's; LOPEN's
    // fid, whose CLUNK waits until the LOPEN has ended (its terminal routed).
    struct t_loom_service_ref dest;
    struct t_fixed_book book;
    struct t_fixed_request *fixed;
    uint64_t cookie;
};
// A stream's delivered payloads wait here in arrival order. At most 64 leases
// are live at once, one per pool member of the ring's bank.
struct t_loom_shot_cell { struct t_loom_op *op; uint64_t seq; struct t_ring_payload *payload; };

struct t_loom_ring {
    struct t_loom_kernel k;
    int32_t fd;
    unsigned lost, state, retired, used, root, admission;
    // Handles the caller holds: targets, scopes, fids, pools and operations.
    size_t handles;
    // The ring's own, the core backend and the mapping backend.
    unsigned holders;
    uintptr_t region_va[64];
    size_t region_len[64], regions, buffers;
    uintptr_t ring_va;
    size_t ring_len;
    unsigned ring_valid;
    // The SQ's only producer, over pointers validated with the mapping.
    unsigned char *hdr, *names;
    uint32_t *sq_array;
    struct t_loom_sqe *sqes;
    uint32_t sq_entries, name_buffer, dropped;
    uint64_t submitted, ended, shot_seq;
    struct t_loom_slot slots[64];
    struct t_loom_pool_cell pools[64];
    struct t_loom_shot_cell shots[64];
    struct t_owned_mapping map;
    struct t_fixed_storage client;
    struct t_fixed_driver *driver;
    struct t_loom_op *ops;
    size_t op_count, op_next;
    struct t_request_entry *book;
};

// One ring's storage, for a ring of `entries` SQEs (a power of two up to 4096):
// one request-book entry and one operation handle per CQ slot. Zero it before
// its first use (static storage is), and reuse it only once t_loom_idle
// answers 1.
#define T_LOOM_RING_STORAGE(entries) \
    struct { struct t_loom_ring base; struct t_request_entry book[2 * (entries)]; struct t_loom_op ops[2 * (entries)]; }
#define T_LOOM_RING_SETUP(storage, kernel, entries, lengths, n, out) \
    t_loom_ring_setup(&(storage)->base, (storage)->book, (storage)->ops, \
                      sizeof((storage)->ops) / sizeof((storage)->ops[0]), (kernel), (entries), (lengths), (n), (out))

// A ring of `entries` SQEs whose fixed and pooled I/O use n registered regions,
// one per length, each rounded up to a page. At most 63 buffers; the adapter
// registers one more for WALK names. cq is the storage's operation count.
int t_loom_ring_setup(struct t_loom_ring *, struct t_request_entry *book, struct t_loom_op *ops, size_t cq,
                      const struct t_loom_kernel *, uint32_t entries, const size_t *lengths, size_t n,
                      struct t_loom_ring **);
// 1 once the storage may be reused: the ring dropped, and every handle, read
// result and payload with it.
int t_loom_idle(const struct t_loom_ring *);
uint64_t t_loom_now(struct t_loom_ring *);
// Completions the kernel has posted that no turn has routed yet. It reads the
// CQ's entry count when the CQ is full; the kernel holds what it cannot post
// until a turn makes room.
size_t t_loom_backlog(struct t_loom_ring *);
// One bounded step: submit what the SQ holds, route every completion the CQ
// holds and retry deferred cleanup. If none of that found work, wait once for a
// completion or until the deadline (absolute monotonic ns, when has_deadline).
// Answers the completions routed, or a negative errno: -ECANCELED once the ring
// is closed, -EIO once it has failed, or poll's own refusal.
int64_t t_loom_turn(struct t_loom_ring *, int has_deadline, uint64_t deadline);
// Stop admission and abort every scope; turns then drive the ring until
// t_loom_retired. A scope's retirement is not a poll event, so a caller waiting
// for it gives its turns a deadline.
void t_loom_retire(struct t_loom_ring *);
int t_loom_retired(const struct t_loom_ring *);
// Close the ring now: every scope aborts and every pending operation ends
// -ECANCELED. Leases keep their bytes until they are dropped.
void t_loom_close(struct t_loom_ring *);
// Close, and give back the ring handle.
void t_loom_ring_drop(struct t_loom_ring *);

// A strict 9P service named in the registry registry_fd (from open("/srv",
// O_PATH)). Synchronous: the kernel checks the name now and the service when a
// scope connects. Dropping it reaps the slot once no CONNECT borrows it.
int t_loom_target(struct t_loom_ring *, int32_t registry_fd, const void *name, size_t len, struct t_loom_target **);
void t_loom_target_drop(struct t_loom_target *);
// Register ranges of the ring's buffers as a pool for pooled reads. The pool's
// slot is reaped once its handle and every stream that names it are gone and
// every lease is back.
int t_loom_pool(struct t_loom_ring *, const struct t_loom_service_pool_member *, size_t, struct t_loom_pool **);
void t_loom_pool_drop(struct t_loom_pool *);

// Submissions. A deadline is absolute monotonic ns, used when has_deadline; a
// timeout aborts the whole scope.
// A new connection: a fresh 9P session to the service, and the scope that owns it.
int t_loom_connect(struct t_loom_target *, int has_deadline, uint64_t deadline, struct t_loom_op **);
// Walk one component (1..255 bytes, no slash or NUL, neither "." nor "..") from
// the scope's root or from a fid, or clone it with an empty name.
int t_loom_walk(struct t_loom_scope *, const void *name, size_t len, int has_deadline, uint64_t deadline,
                struct t_loom_op **);
int t_loom_walk_fid(struct t_loom_fid *, const void *name, size_t len, int has_deadline, uint64_t deadline,
                    struct t_loom_op **);
// LOPEN with mode 0 (read), 1 (write) or 2 (read-write).
int t_loom_open(struct t_loom_fid *, uint32_t mode, int has_deadline, uint64_t deadline, struct t_loom_op **);
// File bytes at offset into range of a ring buffer. A buffer holds one fixed
// operation at a time, and none overlapping a pool.
int t_loom_read(struct t_loom_fid *, struct t_loom_service_pool_member range, uint64_t offset, int has_deadline,
                uint64_t deadline, struct t_loom_op **);
// Exactly range.length bytes at offset, through range.
int t_loom_write(struct t_loom_fid *, struct t_loom_service_pool_member range, uint64_t offset, const void *bytes,
                 size_t len, int has_deadline, uint64_t deadline, struct t_loom_op **);
// Pooled reads of at most limit bytes each from offset: one, or with multishot
// a stream that runs until its final completion.
int t_loom_stream(struct t_loom_fid *, struct t_loom_pool *, uint64_t offset, uint32_t limit, int multishot,
                  int has_deadline, uint64_t deadline, struct t_loom_op **);

// Completions: 0 while pending (the handle is kept); 1 once done, which
// consumes the handle and gives the CQE's result (a negative errno for a failed
// operation); a negative errno when the ring has closed or is retiring
// (-ECANCELED; a retiring ring's operation can still answer 1 once its terminal
// is routed) or has failed (-EIO), or -EINVAL for another kind's handle, and
// the handle is kept.
int t_loom_connect_done(struct t_loom_op *, int32_t *result, struct t_loom_scope **);
int t_loom_walk_done(struct t_loom_op *, int32_t *result, struct t_loom_fid **);
int t_loom_open_done(struct t_loom_op *, int32_t *result);
// A finished READ's result is the core's, holding its buffer range. It is set
// only for a successful read; drop it to free the range.
int t_loom_read_done(struct t_loom_op *, int32_t *result, struct t_fixed_read_result **);
int t_loom_write_done(struct t_loom_op *, int32_t *result);
// Cancel: a pending operation's scope aborts, siblings included.
void t_loom_op_drop(struct t_loom_op *);

const unsigned char *t_loom_read_bytes(const struct t_fixed_read_result *, size_t *);
void t_loom_read_drop(struct t_fixed_read_result *);

// A stream's next shot: 0 while none has arrived; 1 with a payload lease, or
// with its end (end set, the final result in *result), after which the stream
// yields nothing; a negative errno as for completions. The handle is kept
// either way: drop it with t_loom_op_drop, which aborts the scope if the stream
// has not ended. Payloads already delivered stay valid.
int t_loom_stream_next(struct t_loom_op *, struct t_ring_payload **, int *end, int32_t *result);
const unsigned char *t_loom_payload_bytes(const struct t_ring_payload *, size_t *);
// Return the lease now: 0 consumed it; a refusal answers its errno and keeps it.
int t_loom_payload_return(struct t_ring_payload *);
// Return the lease, in the background if the kernel refuses it now.
void t_loom_payload_drop(struct t_ring_payload *);

// Cancel the connection: every operation on it ends, siblings included, and no
// new one is admitted.
void t_loom_scope_abort(struct t_loom_scope *);
// The scope lives while this handle, a fid of it or an operation on it does;
// when the last goes it is aborted and its slot reaped.
void t_loom_scope_drop(struct t_loom_scope *);
// Clunks the fid in the background.
void t_loom_fid_drop(struct t_loom_fid *);

#endif
