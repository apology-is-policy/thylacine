// The native adapter's kernel calls on Thylacine: each entry is one system call,
// its arguments and result unchanged. SYS_BURROW_ATTACH maps eager, physically
// contiguous, zeroed read-write memory, the kind Loom buffer registration takes.
#include <thyla/loom_client/adapter.h>
#include <thyla/poll.h>

_Static_assert(sizeof(struct t_owned_setup) == sizeof(struct t_loom_params), "setup record");

static int64_t k_setup(void *context, uint32_t entries, struct t_owned_setup *params) {
    (void)context;
    return t_loom_setup(entries, (struct t_loom_params *)(void *)params);
}
static int64_t k_reg(void *context, int32_t fd, uint32_t op, void *arg, uint32_t nargs) {
    (void)context;
    return t_loom_register(fd, op, arg, nargs);
}
static int64_t k_enter(void *context, int32_t fd, uint32_t to_submit, uint32_t min_complete, uint32_t flags) {
    (void)context;
    return t_loom_enter(fd, to_submit, min_complete, flags);
}
static int64_t k_poll(void *context, int32_t fd, int16_t events, int32_t timeout_ms) {
    (void)context;
    struct pollfd p = { fd, events, 0 };
    long rc = t_poll(&p, 1, timeout_ms);
    return rc <= 0 ? rc : p.revents;
}
static int64_t k_close(void *context, int32_t fd) {
    (void)context;
    return t_close(fd);
}
static int64_t k_attach(void *context, size_t len) {
    (void)context;
    return t_burrow_attach(len);
}
static int64_t k_detach(void *context, uintptr_t va, size_t len) {
    (void)context;
    return t_burrow_detach(va, len);
}
// A clock that cannot be read reads as zero, as libthyla-rs's monotonic_ns does.
static uint64_t k_now(void *context) {
    (void)context;
    struct t_timespec ts;
    if (t_clock_gettime(T_CLOCK_MONOTONIC, &ts) || ts.tv_sec < 0 || ts.tv_nsec < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}
struct t_loom_kernel t_loom_kernel_native(void) {
    return (struct t_loom_kernel){ 0, k_setup, k_reg, k_enter, k_poll, k_close, k_attach, k_detach, k_now };
}
