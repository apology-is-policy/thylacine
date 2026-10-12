// Private Loom syscall inners: docs/ASYNC-SERVICE-ABI.md, Qualification bridge.
// Without LOOM_PRIVATE_QUALIFICATION every entry here returns -EOPNOTSUPP and no
// private ring can be created from a syscall. The SVC handlers own usercopy;
// these take kernel copies and a caller-supplied copyout, so tests drive them.
#ifndef THYLACINE_LOOM_SERVICE_SYSCALL_H
#define THYLACINE_LOOM_SERVICE_SYSCALL_H
#include <thylacine/types.h>
#include <thylacine/handle.h>
struct Loom;
struct Proc;
struct loom_params;

struct loom_buf_reg;

// The qualification lever, as compiled: false in every default image.
bool loom_private_routed(void);
// True for the register ops the private ABI reserves (2..9).
bool loom_private_register_op(u32 op);
// The exact record size an op copies in, or 0 for an op outside 2..9.
u32 loom_private_record_size(u32 op);

// Creates, maps (tagged private: fork omits it, exec closes it), starts the
// executor, installs the handle, fills *out. Unwinds completely on failure.
int loom_private_setup_for_proc(struct Proc *p, u32 entries, u32 flags,
                                struct loom_params *out, hidx_t *out_fd);
// LOOM_REGISTER_BUFFERS on a private ring: the private registration.
int loom_private_register_buffers_routed(struct Loom *l, struct Proc *p,
                                         const struct loom_buf_reg *bufs, u32 n);
// ENTER on a private ring: never sleeps. Retained Loom, calling Proc.
int loom_private_enter(struct Loom *l, struct Proc *p, u32 to_submit,
                       u32 min_complete, u32 flags);

// Writes `len` bytes at byte `offset` of the caller's record; 0 or -EFAULT.
typedef int (*loom_record_copyout_fn)(void *ctx, u32 offset, const void *src, u32 len);
// Register ops 2..9 over an already-copied record of loom_private_record_size(op)
// bytes. Retained Loom and the calling Proc in its stable syscall image. Results
// leave only through `copyout`; a reservation is published only after its
// result copyout succeeds, and rolled back (incarnation burned) when it faults.
int loom_private_register_record(struct Loom *l, struct Proc *p, u32 op,
                                 const void *record, loom_record_copyout_fn copyout,
                                 void *ctx);
#endif
