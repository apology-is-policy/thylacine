/* A simulated kernel for the native C adapter, the C twin of loom-client's
 * tests/native/sim.rs. ENTER, poll, REGISTER and close follow
 * kernel/loom_service_frontend.c, loom_service_decode.c and loom.c: ENTER never
 * sleeps and stops at a full CQ or a pending rejection, copies each SQE before
 * releasing sq_head and copies a WALK name before it returns; poll reports
 * POLLIN for a non-empty CQ and POLLHUP while closing. The service is a strict
 * 9P server with a flat file tree, answering on the next poll. */
#ifndef SIM_KERNEL_H
#define SIM_KERNEL_H
#include <thyla/loom_client/adapter.h>

enum { SIM_EBADF = -9, SIM_EBUSY = -16, SIM_EINVAL = -22, SIM_ENOENT = -2, SIM_ENOSPC = -28, SIM_ENOTDIR = -20,
       SIM_EISDIR = -21, SIM_ETIMEDOUT = -110, SIM_ECANCELED = -125, SIM_EINTR = -4, SIM_EACCES = -13,
       SIM_EIO = -5, SIM_ENOMEM = -12 };
enum { SIM_RESERVED = 1, SIM_READY = 5, SIM_ABORTING = 6, SIM_RETIRED = 7 };
enum { SIM_TARGET = 1, SIM_SCOPE, SIM_FID, SIM_POOL };
enum { SIM_OP_CONNECT = 1, SIM_OP_WALK, SIM_OP_OPEN, SIM_OP_READ, SIM_OP_WRITE, SIM_OP_POOLED, SIM_OP_CLUNK };
#define SIM_NAME 64
#define SIM_MAX 512

struct sim_slot {
    unsigned used, kind, open, has_name;
    uint64_t inc;
    uint32_t state;
    /* A fid's scope slot; a scope's own. */
    size_t scope;
    /* A fid's file (none for the root); a target's service name. */
    unsigned char name[SIM_NAME];
    size_t name_len;
};
struct sim_op {
    uint64_t user_data, offset, deadline, at;
    size_t scope, source, target, dest, pool;
    unsigned kind, multi;
    uint32_t buf, len, limit;
    unsigned char name[256];
    size_t name_len;
};
struct sim_cqe {
    int has_scope;
    size_t scope;
    uint64_t user_data;
    int32_t result;
    uint32_t flags;
    struct t_loom_service_buffer_receipt receipt;
};
struct sim_member { struct t_loom_service_pool_member range; int leased; uint64_t lease; };
struct sim_file { unsigned char name[SIM_NAME]; size_t name_len; unsigned char data[4096]; size_t len; };
struct sim_shots {
    unsigned char name[SIM_NAME];
    size_t name_len, head, count;
    struct { unsigned char b[256]; size_t len; } q[64];
};
struct sim_region { uintptr_t va; size_t len; };

struct sim {
    uint64_t clock;
    struct sim_region regions[128];
    size_t nregions, attaches, attach_fail_at;
    /* The ring. */
    int has_ring, ring_fd, closing;
    uintptr_t ring_va;
    struct t_owned_setup p;
    uint32_t sq_head, cq_tail;
    struct sim_slot slots[64];
    uint64_t next_inc, next_lease;
    struct sim_region buffers[64];
    size_t nbuffers;
    struct sim_member pools[64][64];
    size_t pool_members[64];
    struct sim_op ops[SIM_MAX];
    size_t nops;
    /* Completions waiting for CQ room (no scope for an SQE ENTER refused). */
    struct sim_cqe deferred[SIM_MAX];
    size_t ndeferred;
    struct sim_file files[16];
    size_t nfiles;
    struct sim_shots shots[8];
    size_t nshots;
    unsigned char services[4][SIM_NAME];
    size_t service_len[4], nservices;
    int32_t registry_fd;
    /* The server answers nothing while held. */
    int hold;
    /* An abort's terminals wait for sim_release_terminals: the kernel retires a
     * scope in the visit that cancels its operations and posts their terminals
     * on a later one. */
    int hold_terminals;
    struct sim_cqe held[SIM_MAX];
    size_t nheld;
    /* ENTER's refusals wait for sim_release_rejections: the kernel precharges a
     * refusal it cannot post into a full CQ and posts it on a later visit. */
    int hold_rejections;
    struct sim_cqe rejected[SIM_MAX];
    size_t nrejected;
    /* ENTER consumes at most this many SQEs per call, when set. */
    int has_enter_limit;
    uint32_t enter_limit;
    int32_t polls[1024];
    size_t npolls, enters, clunks, clunk_sqes, returns;
    size_t aborts[256], naborts, reaps[256], nreaps;
    int32_t closed_fds[16];
    size_t nclosed;
    /* Bad sq_array indices seen; the kernel's `dropped`. */
    uint32_t dropped;
    /* Overwrite the next CQE's user_data, to forge a stale completion. */
    int has_forge;
    uint64_t forge_next;
    /* The last payload shot posted, for a test that replays it. */
    int has_last_shot;
    struct sim_cqe last_shot;
    /* Every ENTER on a live ring answers this, as a non-creator image's does. */
    int has_enter_errno;
    int64_t enter_errno;
    /* The next poll answers this instead of looking at the ring. */
    int has_poll_answer;
    int64_t poll_answer;
    /* The next RETURN answers this instead of releasing its member. */
    int has_refuse_return;
    int64_t refuse_return;
};

void sim_init(struct sim *);
struct t_loom_kernel sim_kernel(struct sim *);
size_t sim_live_slots(const struct sim *);
/* One visit of the kernel's engine, which runs beside the adapter: it retires
 * every aborted scope with nothing in flight. */
void sim_engine_visit(struct sim *);
void sim_step(struct sim *);
void sim_release_rejections(struct sim *);
void sim_release_terminals(struct sim *);
/* Post the last payload shot's CQE a second time, receipt and all: an I-29
 * breach the kernel never makes, for the adapter's defence in depth. */
void sim_replay_last_shot(struct sim *);
/* Another thread closes the ring's descriptor: the number is gone, so ENTER and
 * REGISTER answer -EBADF and poll POLLNVAL. */
void sim_close_under(struct sim *);
/* The ring latches closing while the descriptor still resolves: a close racing
 * ENTER (-ECANCELED) or poll (POLLHUP). */
void sim_latch_under(struct sim *);
uint32_t sim_hdr_word(struct sim *, size_t);
void sim_poke_hdr(struct sim *, size_t, uint32_t);
void sim_file(struct sim *, const char *name, const void *bytes, size_t len);
const struct sim_file *sim_find_file(const struct sim *, const char *name);
void sim_shot(struct sim *, const char *name, const void *bytes, size_t len);
int sim_closed(const struct sim *, int32_t fd);
#endif
