// Thylacine userspace poll ABI (libt).
//
// Mirror of kernel/include/thylacine/poll.h's user-facing surface
// (struct pollfd + event bits). The kernel-side header carries internal
// types (rendez, etc.) that userspace must not see; this slim header
// just pins the SYS_POLL ABI.
//
// MUST mirror the kernel side; the kernel's _Static_asserts pin the
// layout there. Drift here would surface as a SYS_POLL ABI mismatch
// at runtime — keep them in lockstep.

#ifndef THYLA_POLL_H
#define THYLA_POLL_H

// Event bits — match Linux for the future musl shim.
#define POLLIN    0x001
#define POLLOUT   0x004
#define POLLERR   0x008
#define POLLHUP   0x010
#define POLLNVAL  0x020

struct pollfd {
    int            fd;       // fd index (KOBJ_SPOOR or KOBJ_SRV at v1.0)
    short          events;   // T_POLL* bitmask requested
    short          revents;  // kernel-filled subset of `events` that fired
};

#endif // THYLA_POLL_H
