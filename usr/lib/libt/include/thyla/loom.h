// Native C mirror of the Loom ring ABI (kernel/include/thylacine/loom.h) and its
// three syscalls. Every offset is pinned below; loom-bridge-probe checks the
// mirror against what the kernel actually writes into a ring.
#ifndef THYLA_LOOM_H
#define THYLA_LOOM_H
#include <stdint.h>
#include <thyla/syscall.h>
#include <thyla/loom_service.h>

#define T_SYS_LOOM_SETUP    66
#define T_SYS_LOOM_REGISTER 67
#define T_SYS_LOOM_ENTER    68

#define T_LOOM_MAX_ENTRIES         4096u
#define T_LOOM_SETUP_SQPOLL        1u
#define T_LOOM_SETUP_CQSIZE        2u
#define T_LOOM_ENTER_GETEVENTS     1u
#define T_LOOM_ENTER_NONBLOCK      2u
#define T_LOOM_REGISTER_HANDLES    0u
#define T_LOOM_REGISTER_BUFFERS    1u
#define T_LOOM_RING_SQ_NEED_WAKEUP 1u
#define T_LOOM_OP_NOP              0u
#define T_LOOM_SQE_LINK            1u
#define T_LOOM_SQE_DRAIN           2u
#define T_LOOM_SQE_CQE_SKIP        4u
#define T_LOOM_SQE_MULTISHOT       8u
#define T_LOOM_CQE_MORE            1u

// A private SERVICE_BUFFERS ring reports its receipt array in _resv1: offset,
// total size and record size (ASYNC-SERVICE-BUFFERS.md).
struct t_loom_params {
    uint32_t flags;
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t ring_size;
    uint64_t ring_va;
    uint32_t hdr_off;
    uint32_t sq_array_off;
    uint32_t sqe_off;
    uint32_t cqe_off;
    uint32_t sq_array_size;
    uint32_t sqe_size;
    uint32_t cqe_size;
    uint32_t _resv0;
    uint64_t _resv1[4];
};
_Static_assert(sizeof(struct t_loom_params) == 88, "t_loom_params size");
_Static_assert(__builtin_offsetof(struct t_loom_params, flags) == 0, "t_loom_params.flags");
_Static_assert(__builtin_offsetof(struct t_loom_params, sq_entries) == 4, "t_loom_params.sq_entries");
_Static_assert(__builtin_offsetof(struct t_loom_params, cq_entries) == 8, "t_loom_params.cq_entries");
_Static_assert(__builtin_offsetof(struct t_loom_params, ring_size) == 12, "t_loom_params.ring_size");
_Static_assert(__builtin_offsetof(struct t_loom_params, ring_va) == 16, "t_loom_params.ring_va");
_Static_assert(__builtin_offsetof(struct t_loom_params, hdr_off) == 24, "t_loom_params.hdr_off");
_Static_assert(__builtin_offsetof(struct t_loom_params, sq_array_off) == 28, "t_loom_params.sq_array_off");
_Static_assert(__builtin_offsetof(struct t_loom_params, sqe_off) == 32, "t_loom_params.sqe_off");
_Static_assert(__builtin_offsetof(struct t_loom_params, cqe_off) == 36, "t_loom_params.cqe_off");
_Static_assert(__builtin_offsetof(struct t_loom_params, sq_array_size) == 40, "t_loom_params.sq_array_size");
_Static_assert(__builtin_offsetof(struct t_loom_params, sqe_size) == 44, "t_loom_params.sqe_size");
_Static_assert(__builtin_offsetof(struct t_loom_params, cqe_size) == 48, "t_loom_params.cqe_size");
_Static_assert(__builtin_offsetof(struct t_loom_params, _resv0) == 52, "t_loom_params._resv0");
_Static_assert(__builtin_offsetof(struct t_loom_params, _resv1) == 56, "t_loom_params._resv1");

struct t_loom_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t _resv0;
    uint32_t handle_idx;
    uint64_t offset;
    uint32_t len;
    uint32_t buf_idx_or_off;
    uint64_t user_data;
    uint64_t _resv1[4];
};
_Static_assert(sizeof(struct t_loom_sqe) == 64, "t_loom_sqe size");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, opcode) == 0, "t_loom_sqe.opcode");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, flags) == 1, "t_loom_sqe.flags");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, _resv0) == 2, "t_loom_sqe._resv0");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, handle_idx) == 4, "t_loom_sqe.handle_idx");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, offset) == 8, "t_loom_sqe.offset");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, len) == 16, "t_loom_sqe.len");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, buf_idx_or_off) == 20, "t_loom_sqe.buf_idx_or_off");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, user_data) == 24, "t_loom_sqe.user_data");
_Static_assert(__builtin_offsetof(struct t_loom_sqe, _resv1) == 32, "t_loom_sqe._resv1");

struct t_loom_cqe {
    uint64_t user_data;
    int32_t  result;
    uint32_t flags;
};
_Static_assert(sizeof(struct t_loom_cqe) == 16, "t_loom_cqe size");
_Static_assert(__builtin_offsetof(struct t_loom_cqe, user_data) == 0, "t_loom_cqe.user_data");
_Static_assert(__builtin_offsetof(struct t_loom_cqe, result) == 8, "t_loom_cqe.result");
_Static_assert(__builtin_offsetof(struct t_loom_cqe, flags) == 12, "t_loom_cqe.flags");

// sq_tail and cq_head are the caller's to advance; the kernel owns the rest.
struct t_loom_ring_hdr {
    uint32_t sq_head;
    uint32_t sq_tail;
    uint32_t sq_mask;
    uint32_t sq_entries;
    uint32_t cq_head;
    uint32_t cq_tail;
    uint32_t cq_mask;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t dropped;
    uint32_t overflow;
    uint32_t _pad[5];
};
_Static_assert(sizeof(struct t_loom_ring_hdr) == 64, "t_loom_ring_hdr size");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, sq_head) == 0, "t_loom_ring_hdr.sq_head");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, sq_tail) == 4, "t_loom_ring_hdr.sq_tail");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, sq_mask) == 8, "t_loom_ring_hdr.sq_mask");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, sq_entries) == 12, "t_loom_ring_hdr.sq_entries");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, cq_head) == 16, "t_loom_ring_hdr.cq_head");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, cq_tail) == 20, "t_loom_ring_hdr.cq_tail");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, cq_mask) == 24, "t_loom_ring_hdr.cq_mask");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, cq_entries) == 28, "t_loom_ring_hdr.cq_entries");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, flags) == 32, "t_loom_ring_hdr.flags");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, dropped) == 36, "t_loom_ring_hdr.dropped");
_Static_assert(__builtin_offsetof(struct t_loom_ring_hdr, overflow) == 40, "t_loom_ring_hdr.overflow");

struct t_loom_buf_reg {
    uint64_t va;
    uint64_t len;
};
_Static_assert(sizeof(struct t_loom_buf_reg) == 16, "t_loom_buf_reg size");
_Static_assert(__builtin_offsetof(struct t_loom_buf_reg, len) == 8, "t_loom_buf_reg.len");

// The legacy route answers fd / -1; the private route (PRIVATE_SERVICE or
// SERVICE_BUFFERS in params->flags) answers fd / -errno. A params block the
// kernel cannot read answers -1 before either route is chosen.
static inline long t_loom_setup(unsigned long entries, struct t_loom_params *params) {
    register long x0 __asm__("x0") = (long)entries;
    register long x1 __asm__("x1") = (long)params;
    register long x8 __asm__("x8") = T_SYS_LOOM_SETUP;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory", "cc");
    return x0;
}

// Ops 0 and 1 answer 0 / -1 on an ordinary ring; ops 2..9 take one record
// (nargs 1) and answer 0 (ABORT: 0 or 1) / -errno.
static inline long t_loom_register(long fd, unsigned long op, void *arg,
                                   unsigned long nargs) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)op;
    register long x2 __asm__("x2") = (long)arg;
    register long x3 __asm__("x3") = (long)nargs;
    register long x8 __asm__("x8") = T_SYS_LOOM_REGISTER;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                      : "memory", "cc");
    return x0;
}

// On a private ring this never sleeps: min_complete must be 0 and flags may hold
// only NONBLOCK. It answers the SQEs consumed or -errno; park in t_poll.
static inline long t_loom_enter(long fd, unsigned long to_submit,
                                unsigned long min_complete, unsigned long flags) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)to_submit;
    register long x2 __asm__("x2") = (long)min_complete;
    register long x3 __asm__("x3") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_LOOM_ENTER;
    __asm__ volatile ("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                      : "memory", "cc");
    return x0;
}

#endif
