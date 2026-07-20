// Thylacine userspace syscall wrappers (libt — Thylacine userspace runtime).
//
// At P4-Ia1 the userspace syscall surface is exactly two operations
// (matching kernel/include/thylacine/syscall.h):
//
//   t_exits(status)     → SYS_EXITS  (terminate; status==0 ⇒ "ok", else "fail")
//   t_puts(buf, len)    → SYS_PUTS   (write `len` bytes to UART)
//
// Phase 5+ extends the surface: open / close / read / write / mount / bind /
// rfork / wait / mmap / munmap / notify. Each new syscall appends an inline
// wrapper here; the dispatch number lives in the kernel header.
//
// Calling convention (matches kernel/include/thylacine/syscall.h §AArch64 ABI):
//   x8       = syscall number
//   x0..x5   = positional arguments
//   x0       = return value
//
// The kernel saves the full exception_context on SVC entry and restores it
// on eret, so userspace observes ALL GPRs preserved across a syscall except
// x0 (return value). Inline asm clobber list is therefore "memory" + "cc"
// only — not the full caller-saved set.
//
// Header-only by design: trivial wrappers don't justify an out-of-line .a
// path. _start (the program-entry stub) lives in libt.a because it isn't
// inlinable.

#ifndef THYLA_SYSCALL_H
#define THYLA_SYSCALL_H

#include <stddef.h>

// Syscall numbers. MUST mirror kernel/include/thylacine/syscall.h.
enum {
    T_SYS_EXITS       = 0,
    T_SYS_PUTS        = 1,
    T_SYS_MMIO_CREATE = 2,  // P4-Ib: create KObj_MMIO handle
    T_SYS_IRQ_CREATE  = 3,  // P4-Ib: create KObj_IRQ handle
    T_SYS_IRQ_WAIT    = 4,  // P4-Ib: block until IRQ fires
    T_SYS_MMIO_MAP    = 5,  // P4-Ic2: map KObj_MMIO into user-VA
    T_SYS_DMA_CREATE  = 6,  // P4-Ic5b1b: allocate KObj_DMA handle
    T_SYS_DMA_MAP     = 7,  // P4-Ic5b1b: map KObj_DMA into user-VA, returns PA
    T_SYS_PIPE        = 8,  // P5-fd-pipe: create connected pair; rd in x0, wr in x1
    T_SYS_READ        = 9,  // P5-fd-rw: read(fd, buf, len)
    T_SYS_WRITE       = 10, // P5-fd-rw: write(fd, buf, len)
    T_SYS_CLOSE       = 11, // P5-fd-syscalls: close(fd)
    T_SYS_DUP         = 12, // P5-fd-syscalls: dup(oldfd, new_rights) → newfd
    T_SYS_ATTACH_9P   = 13, // P5-attach-syscall: 9P client attach over Spoor pair
    T_SYS_MOUNT       = 14, // P5-mount-syscall: graft Spoor at target_path_id
    T_SYS_UNMOUNT     = 15, // P5-mount-syscall: remove mount entry at target_path_id
    T_SYS_MLOCKALL    = 16, // P5-corvus-syscalls: pin pages (CAP_LOCK_PAGES)
    T_SYS_SET_DUMPABLE = 17, // P5-corvus-syscalls: set NODUMP flag (one-way to 0)
    T_SYS_SET_TRACEABLE = 18, // P5-corvus-syscalls: set NOTRACE flag (one-way to 0)
    T_SYS_EXPLICIT_BZERO = 19, // P5-corvus-syscalls: barrier'd zeroize user-VA buf
    T_SYS_GETRANDOM   = 20, // P5-corvus-syscalls: read kernel CSPRNG (CAP_CSPRNG_READ)
    T_SYS_SPAWN       = 21, // P5-spawn-wait: rfork RFPROC + exec on a devramfs binary
    T_SYS_WAIT_PID    = 22, // P5-spawn-wait: reap one ZOMBIE child; write status if non-NULL
    T_SYS_SPAWN_WITH_FDS = 23, // P5-stratumd-stub-b: spawn + pre-install fds in child
    T_SYS_SPAWN_WITH_CAPS = 24, // P5-spawn-caps: spawn + grant cap-subset to child
    T_SYS_SPAWN_FULL  = 25, // P5-spawn-full: spawn + inherit fds + grant cap-subset
    // 26..30 + 43 — RETIRED/reserved (stalk-3c): SYS_POST_SERVICE (26),
    // SYS_SRV_CONNECT (30), SYS_POST_SERVICE_BYTE (43) are gone. Posting is
    // SYS_WALK_CREATE on a /srv dir (create=post; DMSRVBYTE selects mode);
    // connecting is SYS_OPEN on /srv/<name> (open=connect). No libt wrapper
    // for the retired numbers; 27..29 (srv_accept / srv_peer / poll) remain.
    T_SYS_SPAWN_WITH_PERMS = 31, // P5-corvus-srv-impl-b3a: spawn_full + perm_flags
    T_SYS_CAP_GRANT   = 32,      // P5-hostowner-b-b: register pending cap grant
    T_SYS_CAP_USE     = 33,      // P5-hostowner-b-b: redeem pending cap grant
    T_SYS_WALK_OPEN   = 34,      // P5-stratumd-stub-bringup-e1: walk-and-open one path component
    T_SYS_CHROOT      = 35,      // P5-stratumd-stub-bringup-e2: stamp territory root_spoor
    T_SYS_SET_TID_ADDRESS = 36,  // P6-pouch-kernel-auxv: return the calling thread's tid
    // 37 + 38 — SYS_BURROW_ATTACH / SYS_BURROW_DETACH (P6-pouch-mem-a).
    // 83 + 84 — SYS_BURROW_ATTACH_LAZY / SYS_BURROW_DECOMMIT (#321, the
    // overcommit model): reserve demand-zero VA + madvise(DONTNEED).
    // No libt C-side wrappers yet — pouch's mmap reaches the lazy attach
    // (83) directly through the musl seam (0003-pouch-mman); the native
    // Rust heap reaches it through libthyla-rs::t_burrow_attach_lazy.
    // Native C consumers will land a wrapper pair when one materialises.
    T_SYS_TORPOR_WAIT = 39,      // P6-pouch-wait-addr: wait on a user-VA word
    T_SYS_TORPOR_WAKE = 40,      // P6-pouch-wait-addr: wake waiters on a user-VA word
    T_SYS_THREAD_SPAWN = 41,     // P6-pouch-threads: spawn an EL0 Thread in the caller's Proc
    T_SYS_THREAD_EXIT  = 42,     // P6-pouch-threads: exit the calling Thread; never returns
    // 43..48 — SYS_POST_SERVICE_BYTE + SYS_NOTE_OPEN / NOTIFY / NOTED /
    // POSTNOTE / NOTE_MASK (P6-pouch-sockets + P6-pouch-signals); pouch
    // musl reaches them directly through bits/syscall.h.in.
    T_SYS_SPAWN_FULL_ARGV = 49,  // P6-pouch-stratumd-boot 16b-alpha: argv pass-through spawn
    T_SYS_FSTAT       = 50,      // P6-pouch-stratumd-boot 16b-gamma: native file metadata
    T_SYS_LSEEK       = 51,      // P6-pouch-stratumd-boot 16b-gamma: file-offset cursor
    T_SYS_ATTACH_9P_SRV = 52,    // P6-pouch-stratumd-boot 16c: 9P attach over byte-mode KOBJ_SRV
    T_SYS_PIVOT_ROOT  = 53,      // P6-pouch-stratumd-boot 16c: atomic root_spoor swap
    T_SYS_WALK_CREATE = 54,      // FS-mutation foundation: create-then-open a component
    T_SYS_FSYNC       = 55,      // FS-mutation foundation: durability barrier
    T_SYS_READDIR     = 56,      // FS-mutation foundation: directory enumeration
    T_SYS_RENAME      = 57,      // FS-gamma: atomic rename/move (Trenameat)
    T_SYS_UNLINK      = 58,      // FS-gamma: remove a file or empty directory (Tunlinkat)
    T_SYS_WSTAT       = 59,      // A-2a: chmod/chown via Tsetattr
    T_SYS_EXIT_GROUP  = 60,      // SYS_EXIT_GROUP: whole-Proc group-terminate (ARCH 7.9.1/I-24)
    // 61 = SYS_CAP_GRANT_CLEARANCE (native libthyla-rs only; not used from libt).
    T_SYS_BOOT_COMPLETE     = 62,  // A-5a: init signals boot-complete -> banner
    T_SYS_CONSOLE_RELINQUISH = 63, // A-5a: drop own console-attach (I-27)
    T_SYS_CONSOLE_OPEN      = 64,  // A-5a: open /dev/cons -> R|W KOBJ_SPOOR fd
    T_SYS_OPEN              = 65,  // A-5b-0/stalk-1: multi-component pathname open
    T_SYS_CHDIR             = 69,  // LS-4: set the per-Proc cwd (dot_path)
    T_SYS_FD2PATH           = 71,  // #66: fd -> namespace name (Plan 9 fd2path)
    // 72..74 = getpid/uid/gid (native libthyla-rs only).
    T_SYS_CLOCK_GETTIME     = 75,  // LS-K: read CLOCK_REALTIME / CLOCK_MONOTONIC
    T_SYS_PCI_CLAIM         = 76,  // pci-1c: claim a VirtIO-PCI function -> KOBJ_PCI
    T_SYS_PCI_MAP_BAR       = 77,  // pci-1c: map a KObj_PCI BAR into user VA
    T_SYS_PCI_INFO          = 78,  // pci-1c: read a KObj_PCI's resolved topology
    T_SYS_CLOCK_SETTIME     = 79,  // net-7a: step CLOCK_REALTIME (CAP_HOSTOWNER)
    T_SYS_PREAD             = 85,  // #37: positioned read (cursor untouched)
    T_SYS_PWRITE            = 86,  // #37: positioned write (cursor untouched)
    T_SYS_YIELD             = 87,  // #33: voluntary yield (hint; always 0)
    T_SYS_STAT              = 88,  // POUNCE: path-stat in one syscall (1 RPC)
    T_SYS_SETSID            = 89,  // PTY-1a: new session (POSIX setsid)
    T_SYS_SETPGID           = 90,  // PTY-1a: move self/child into a pgrp
    T_SYS_GETPGID           = 91,  // PTY-1a: read a Proc's pgid (0 = self)
    T_SYS_GETSID            = 92,  // PTY-1a: read a Proc's sid (0 = self)
    T_SYS_PTY_REGISTER      = 93,  // PTY-1c: pts registry ops (ptyfs-only)
    T_SYS_TTY_SIGNAL        = 94,  // PTY-1d: server-side signal-class report
    T_SYS_TTY_ACQUIRE       = 95,  // PTY-1d: controlling-terminal acquisition
    T_SYS_TTY_SET_FG        = 96,  // PTY-1d: tcsetpgrp
    T_SYS_TTY_GET_FG        = 97,  // PTY-1d: tcgetpgrp
    T_SYS_TTY_CONT          = 98,  // PTY-1f: fg/bg resume of a job-stopped pgrp
};

// SYS_PTY_REGISTER ops (PTY-1c). Server-side (ptyfs) only: MINT records a
// pty pair's master (connection, qid) + returns the gen-stamped pts_id;
// SLAVE binds each served slave open; FREE drops the pts at last close.
#define T_PTY_REG_MINT   0
#define T_PTY_REG_SLAVE  1
#define T_PTY_REG_FREE   2

// SYS_TTY_SIGNAL classes (PTY-1d). TSTP (live since PTY-1f) is the
// job-control suspend: a caught susp delivers the tty:susp note only; an
// uncaught one on a non-orphaned foreground group takes the default STOP
// (waitpid-with-T_WAIT_UNTRACED reports it; T_SYS_TTY_CONT resumes).
#define T_TTY_SIG_INT    1
#define T_TTY_SIG_QUIT   2
#define T_TTY_SIG_TSTP   3
#define T_TTY_SIG_WINCH  4
#define T_TTY_SIG_HUP    5

// SYS_CLOCK_*TIME clock ids (match Linux clockid_t) + the 16-byte timespec.
#define T_CLOCK_REALTIME    0
#define T_CLOCK_MONOTONIC   1
struct t_timespec {
    long tv_sec;    // 0: seconds (since the Unix epoch for REALTIME; since boot
                    //    for MONOTONIC)
    long tv_nsec;   // 8: nanoseconds within the second, [0, 1000000000)
};
_Static_assert(sizeof(struct t_timespec) == 16, "t_timespec is the 16-byte ABI record");

// SYS_UNLINK flags: rmdir an empty directory (vs unlink a non-directory).
// Mirrors the kernel's SYS_UNLINK_REMOVEDIR / wire P9_UNLINK_AT_REMOVEDIR.
#define T_UNLINK_REMOVEDIR  0x200u

// SYS_WSTAT valid-mask bits (A-2a; IDENTITY-DESIGN.md §9.5). Mirror the kernel
// T_WSTAT_* / wire P9_SETATTR_* bits. chmod sets T_WSTAT_MODE; chown sets
// T_WSTAT_UID | T_WSTAT_GID.
#define T_WSTAT_MODE       0x1u
#define T_WSTAT_UID        0x2u
#define T_WSTAT_GID        0x4u
#define T_WSTAT_MODE_MASK  0777u

// Torpor error codes — match kernel's TORPOR_ERR_* (Linux/musl-numeric
// so the pouch syscall seam will decode them as -errno when the pouch
// pthread layer lands at sub-chunk 9). Native callers can branch on
// these directly; the value-mismatch fast path returns 0 (TORPOR_OK).
#define T_TORPOR_OK             0
#define T_TORPOR_ERR_EINVAL    (-22)
#define T_TORPOR_ERR_EFAULT    (-14)
#define T_TORPOR_ERR_ETIMEDOUT (-110)

// t_torpor_wait — wait until *addr_va may have changed and a matching
// t_torpor_wake arrives, OR the timeout elapses. Returns 0 on success
// (woken OR value mismatch), or one of the T_TORPOR_ERR_* errnos.
// `timeout_us < 0` blocks indefinitely. The 4-byte word at addr_va
// must be aligned and within the user VA bound.
__attribute__((always_inline))
static inline long t_torpor_wait(unsigned int *addr_va, unsigned int expected,
                                 long timeout_us) {
    register long x0 __asm__("x0") = (long)(unsigned long)addr_va;
    register long x1 __asm__("x1") = (long)expected;
    register long x2 __asm__("x2") = timeout_us;
    register long x8 __asm__("x8") = T_SYS_TORPOR_WAIT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_torpor_wake — wake up to `count` threads waiting on `*addr_va`.
// Returns the number of waiters actually woken (>= 0), or -EINVAL on
// bad args. `count == 0` is a no-op (returns 0); UINT32_MAX is the
// "wake all" pattern.
//
// F12 (P3 audit): `count` is `unsigned int` here so the wrapper cannot
// pass a u64-valued count to the SVC. The kernel SVC handler narrows
// the raw arg to u32 (`(u32)count_raw` in `sys_torpor_wake_handler`),
// so a Rust- or asm-side caller passing a u64 > UINT32_MAX would have
// its high bits silently discarded. UINT32_MAX already means "wake
// all"; values above it are semantically the same intent, so the
// narrowing is not a soundness concern, only a slightly-lossy ABI.
__attribute__((always_inline))
static inline long t_torpor_wake(unsigned int *addr_va, unsigned int count) {
    register long x0 __asm__("x0") = (long)(unsigned long)addr_va;
    register long x1 __asm__("x1") = (long)count;
    register long x8 __asm__("x8") = T_SYS_TORPOR_WAKE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_walk_open's FROM_ROOT sentinel — pass as spoor_fd to walk from the
// caller's pivoted territory root_spoor instead of a handle.
// P5-stratumd-stub-bringup-e2. Matches SYS_WALK_OPEN_FROM_ROOT.
#define T_WALK_OPEN_FROM_ROOT  ((long)(-1))

// SYS_WALK_OPEN omode bits — must mirror SYS_WALK_OPEN_OMODE_VALID in
// kernel/include/thylacine/syscall.h. Plan-9 modes: OREAD=0, OWRITE=1,
// ORDWR=2, OEXEC=3 in the low 2 bits, optional OTRUNC=0x10 modifier.
#define T_OREAD    0u
#define T_OWRITE   1u
#define T_ORDWR    2u
#define T_OEXEC    3u
#define T_OTRUNC   0x10u
// FS-delta (IDENTITY-DESIGN.md §9.4): walk-without-open (Linux O_PATH /
// Plan 9 walk). SYS_WALK_OPEN with this flag returns a NON-OPENED, walkable
// KObj_Spoor -- the valid base for creating/walking/renaming/unlinking
// children + a valid SYS_CHROOT target (a normally-opened handle is not: 9P
// forbids Twalk from an opened fid). Access bits are ignored when set.
#define T_OPATH    0x80u

// Maximum single-component name length for t_walk_open (matches the
// kernel cap; passing longer returns -1 from the syscall). 255 since the
// #36 raise (the 64-era cap EINVAL'd content-addressed names).
#define T_WALK_OPEN_NAME_MAX  255u

// SYS_SPAWN_WITH_PERMS perm_flags — must mirror SPAWN_PERM_* in
// kernel/include/thylacine/syscall.h. SPAWN_PERM_MAY_POST_SERVICE stamps
// PROC_FLAG_MAY_POST_SERVICE on the spawned child so it may call
// SYS_POST_SERVICE; granting it requires the parent to be console-
// attached.
#define T_SPAWN_PERM_MAY_POST_SERVICE  (1u << 0)
// T_SPAWN_PERM_CONSOLE_TRUSTED (A-4c-2) records the spawned child as the trusted
// login authority (the SAK re-grant target); joey grants it to /sbin/corvus.
// Like every perm bit, granting it requires the parent to be console-attached.
#define T_SPAWN_PERM_CONSOLE_TRUSTED   (1u << 1)
// T_SPAWN_PERM_CONSOLE_OWNER (LS-5) records the spawned child as the console
// OWNER -- the Proc that receives the `interrupt` note on Ctrl-C. Distinct from
// CONSOLE_TRUSTED (it never confers console-attach, I-27); gated like
// MAY_POST_SERVICE so trusted /sbin/login confers it on the session shell.
#define T_SPAWN_PERM_CONSOLE_OWNER     (1u << 2)
// T_SPAWN_PERM_CONSOLE_RENDERER (G-4) records the spawned child as the bound
// console RENDERER (Aurora) -- the single Proc allowed to open the
// /dev/consdrain + /dev/consfeed pair. The third console role (I-27): no
// elevation authority, no Ctrl-C-target authority. Gated NARROW
// (console-attach-only, like CONSOLE_TRUSTED) + single-holder (refused while
// a live renderer holds the role). joey grants it to /bin/aurora.
#define T_SPAWN_PERM_CONSOLE_RENDERER  (1u << 3)

// SYS_SPAWN_FULL_ARGV bounds — must mirror SYS_SPAWN_ARGV_MAX +
// SYS_SPAWN_ARGV_DATA_MAX in kernel/include/thylacine/syscall.h.
#define T_SYS_SPAWN_ARGV_MAX        512u
#define T_SYS_SPAWN_ARGV_DATA_MAX   65536u

// SYS_SPAWN_FULL_ARGV argument record — must mirror struct sys_spawn_args
// in kernel/include/thylacine/syscall.h. The 80-byte ABI shape is pinned
// by _Static_assert there; the layout here is the user-visible counterpart.
// A-1a appended the identity block (56 -> 80); leave identity_flags == 0
// (the default for a designated-initializer / zeroed struct) to spawn a
// child that INHERITS the caller's identity. Setting SPAWN_IDENTITY_SET
// requires the caller to hold CAP_SET_IDENTITY (else the syscall -> -1).
struct t_sys_spawn_args {
    unsigned long  name_va;          // 0
    unsigned long  argv_data_va;     // 8
    unsigned long  fd_list_va;       // 16
    unsigned int   name_len;         // 24
    unsigned int   argv_data_len;    // 28
    unsigned int   argc;             // 32
    unsigned int   fd_count;         // 36
    unsigned int   perm_flags;       // 40
    unsigned int   _pad_envp;        // 44 — must be 0 at v1.0
    unsigned long  cap_mask;         // 48
    unsigned int   principal_id;     // 56 — A-1a (honored iff SPAWN_IDENTITY_SET)
    unsigned int   primary_gid;      // 60 — A-1a
    unsigned long  supp_gids_va;     // 64 — A-1a (user-VA of supp_gid_count u32s)
    unsigned int   supp_gid_count;   // 72 — A-1a (0..15)
    unsigned int   identity_flags;   // 76 — A-1a (T_SPAWN_IDENTITY_SET)
    // Menagerie step 5 (append-only, 80 -> 96): the hardware-allowance grant.
    // Leave allowance_flags == 0 (zeroed struct) to inherit the parent's
    // allowance (the broad default for every non-warden caller).
    unsigned long  allowance_va;     // 80 — user-VA of a struct t_allowance_desc
    unsigned int   allowance_flags;  // 88 — T_SPAWN_ALLOWANCE_SET
    unsigned int   _pad_allow;       // 92 — must be 0 at v1.0
};
_Static_assert(sizeof(struct t_sys_spawn_args) == 96,
               "struct t_sys_spawn_args must mirror the kernel's "
               "struct sys_spawn_args 96-byte ABI (A-1a identity block + "
               "the Menagerie step-5 allowance block)");
// R1 F8 fix: per-field offsetof asserts (mirror the kernel struct's
// asserts) so a reordering on either side that leaves total size
// unchanged still fails at compile time.
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, name_va) == 0,
               "t_sys_spawn_args.name_va at ABI offset 0");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, argv_data_va) == 8,
               "t_sys_spawn_args.argv_data_va at ABI offset 8");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, fd_list_va) == 16,
               "t_sys_spawn_args.fd_list_va at ABI offset 16");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, name_len) == 24,
               "t_sys_spawn_args.name_len at ABI offset 24");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, argv_data_len) == 28,
               "t_sys_spawn_args.argv_data_len at ABI offset 28");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, argc) == 32,
               "t_sys_spawn_args.argc at ABI offset 32");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, fd_count) == 36,
               "t_sys_spawn_args.fd_count at ABI offset 36");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, perm_flags) == 40,
               "t_sys_spawn_args.perm_flags at ABI offset 40");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, _pad_envp) == 44,
               "t_sys_spawn_args._pad_envp at ABI offset 44");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, cap_mask) == 48,
               "t_sys_spawn_args.cap_mask at ABI offset 48");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, principal_id) == 56,
               "t_sys_spawn_args.principal_id at ABI offset 56");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, primary_gid) == 60,
               "t_sys_spawn_args.primary_gid at ABI offset 60");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, supp_gids_va) == 64,
               "t_sys_spawn_args.supp_gids_va at ABI offset 64");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, supp_gid_count) == 72,
               "t_sys_spawn_args.supp_gid_count at ABI offset 72");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, identity_flags) == 76,
               "t_sys_spawn_args.identity_flags at ABI offset 76");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, allowance_va) == 80,
               "t_sys_spawn_args.allowance_va at ABI offset 80");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, allowance_flags) == 88,
               "t_sys_spawn_args.allowance_flags at ABI offset 88");
_Static_assert(__builtin_offsetof(struct t_sys_spawn_args, _pad_allow) == 92,
               "t_sys_spawn_args._pad_allow at ABI offset 92");

// Menagerie step 5: T_SPAWN_ALLOWANCE_SET (mirror SPAWN_ALLOWANCE_SET) + the
// hardware-allowance descriptor (mirror struct t_allowance_desc). A C caller
// that wants to confer a narrowed allowance fills a t_allowance_desc, points
// allowance_va at it, and sets allowance_flags |= T_SPAWN_ALLOWANCE_SET. The
// kernel gates the conferred set as a narrowing of the caller's own allowance.
#define T_SPAWN_ALLOWANCE_SET  (1u << 0)

struct t_hw_window {
    unsigned long base;
    unsigned long size;
};
struct t_allowance_desc {
    struct t_hw_window mmio[8];
    unsigned int       mmio_count;
    unsigned int       irq_count;
    unsigned int       irq[8];
    unsigned long      dma_max;
    unsigned int       pci_count;
    unsigned int       pci[8];   // packed bus<<16 | dev<<8 | fn
    unsigned int       _pad_pci;
};
_Static_assert(sizeof(struct t_allowance_desc) == 216,
               "struct t_allowance_desc must mirror the kernel's 216-byte ABI");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, mmio) == 0,
               "t_allowance_desc.mmio at ABI offset 0");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, mmio_count) == 128,
               "t_allowance_desc.mmio_count at ABI offset 128");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, irq_count) == 132,
               "t_allowance_desc.irq_count at ABI offset 132");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, irq) == 136,
               "t_allowance_desc.irq at ABI offset 136");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, dma_max) == 168,
               "t_allowance_desc.dma_max at ABI offset 168");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, pci_count) == 176,
               "t_allowance_desc.pci_count at ABI offset 176");
_Static_assert(__builtin_offsetof(struct t_allowance_desc, pci) == 180,
               "t_allowance_desc.pci at ABI offset 180");

// A-1a: identity_flags bits (mirror SPAWN_IDENTITY_* in the kernel header).
#define T_SPAWN_IDENTITY_SET   (1u << 0)

// A-1a: reserved principal-id / gid sentinels (mirror <thylacine/proc.h>).
// None is privileged (I-22). Real ids are corvus-assigned in [1, 0xFFFFFFFD].
#define T_PRINCIPAL_INVALID    0u
#define T_PRINCIPAL_SYSTEM     0xFFFFFFFEu
#define T_PRINCIPAL_NONE       0xFFFFFFFFu
#define T_GID_INVALID          0u
#define T_GID_SYSTEM           0xFFFFFFFEu
#define T_GID_NONE             0xFFFFFFFFu

// SYS_GETRANDOM flags (mirror kernel/include/thylacine/syscall.h).
#define T_GRND_NONBLOCK   1u

// CAP_* bits — MUST mirror kernel/include/thylacine/caps.h. Used as
// `cap_mask` arg to t_spawn_with_caps / t_spawn_full / t_cap_grant /
// t_cap_use.
#define T_CAP_HW_CREATE       (1UL << 0)
#define T_CAP_LOCK_PAGES      (1UL << 1)
#define T_CAP_CSPRNG_READ     (1UL << 2)
#define T_CAP_HOSTOWNER       (1UL << 3)   // elevation-only; not in CAP_ALL
#define T_CAP_GRANT_HOSTOWNER (1UL << 4)   // fork-grantable; joey → corvus
#define T_CAP_SET_IDENTITY    (1UL << 5)   // A-1a; fork-grantable; gates SPAWN_IDENTITY_SET
// A-4a clearance/legate caps (mirror kernel/include/thylacine/caps.h).
#define T_CAP_GRANT_CLEARANCE (1UL << 6)   // fork-grantable; corvus-only; register clearance grants
#define T_CAP_DAC_OVERRIDE    (1UL << 7)   // elevation-only; perm_check rwx bypass
#define T_CAP_CHOWN           (1UL << 8)   // elevation-only; chown/chgrp-to-any
#define T_CAP_KILL            (1UL << 9)   // elevation-only; cross-identity kill override

// Maximum binary name length for t_spawn (mirror SYS_SPAWN_NAME_MAX).
#define T_SPAWN_NAME_MAX  256u

// Maximum inherited-fd count for t_spawn_with_fds (mirror SYS_SPAWN_MAX_FDS).
#define T_SPAWN_MAX_FDS   16u

// Mount flags — mirror kernel/include/thylacine/territory.h (Plan 9
// MREPL / MBEFORE / MAFTER / MCREATE). At v1.0 only MREPL has
// distinguished semantics (replace existing entry at the same target);
// MBEFORE / MAFTER / MCREATE are stored for future union-mount work.
#define T_MREPL    0x0001u
#define T_MBEFORE  0x0002u
#define T_MAFTER   0x0004u
#define T_MCREATE  0x0008u

// VMA prot bits — MUST mirror kernel/include/thylacine/vma.h's
// VMA_PROT_* values. Used as the 3rd argument to t_mmio_map.
#define T_PROT_READ       (1u << 0)
#define T_PROT_WRITE      (1u << 1)
#define T_PROT_EXEC       (1u << 2)

// Rights bits. MUST mirror kernel/include/thylacine/handle.h.
#define T_RIGHT_READ      (1u << 0)
#define T_RIGHT_WRITE     (1u << 1)
#define T_RIGHT_MAP       (1u << 2)
#define T_RIGHT_TRANSFER  (1u << 3)
#define T_RIGHT_DMA       (1u << 4)
#define T_RIGHT_SIGNAL    (1u << 5)

// t_exits — terminate the calling process with `status`. Never returns.
// status==0 maps to kernel exits("ok"); non-zero to exits("fail").
__attribute__((noreturn, always_inline))
static inline void t_exits(long status) {
    register long x0 __asm__("x0") = status;
    register long x8 __asm__("x8") = T_SYS_EXITS;
    __asm__ volatile (
        "svc #0"
        :: "r"(x0), "r"(x8)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

// t_puts — write `len` bytes from `buf` to the kernel's diagnostic UART.
// Returns `len` on success, -1 on validation failure (NULL buf, oversized
// len, fault on user-VA copy).
__attribute__((always_inline))
static inline long t_puts(const char *buf, size_t len) {
    register long x0 __asm__("x0") = (long)(unsigned long)buf;
    register long x1 __asm__("x1") = (long)len;
    register long x8 __asm__("x8") = T_SYS_PUTS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_putstr — convenience wrapper: write a NUL-terminated string via t_puts.
// Computes strlen inline (no libc dependency).
__attribute__((always_inline))
static inline long t_putstr(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return t_puts(s, n);
}

// t_mmio_create — allocate a KObj_MMIO handle claiming the physical
// memory range [pa, pa+size). Returns a non-negative handle index on
// success, -1 on:
//   - EPERM (caller lacks T_RIGHT_HW_CREATE capability)
//   - EINVAL (pa or size not page-aligned, size == 0, rights invalid)
//   - EBUSY (PA range overlaps an already-claimed range)
//   - EOOM / handle-table-full
//
// Pinned by specs/handles.tla::HwResourceExclusive + ::HwHandleImpliesCap.
__attribute__((always_inline))
static inline long t_mmio_create(unsigned long pa, unsigned long size,
                                 unsigned long rights) {
    register long x0 __asm__("x0") = (long)pa;
    register long x1 __asm__("x1") = (long)size;
    register long x2 __asm__("x2") = (long)rights;
    register long x8 __asm__("x8") = T_SYS_MMIO_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_irq_create — allocate a KObj_IRQ handle subscribing to GIC `intid`.
// Returns a non-negative handle index on success, -1 on EPERM / EINVAL /
// EBUSY / OOM. Pinned by HwResourceExclusive + HwHandleImpliesCap.
__attribute__((always_inline))
static inline long t_irq_create(unsigned long intid, unsigned long rights) {
    register long x0 __asm__("x0") = (long)intid;
    register long x1 __asm__("x1") = (long)rights;
    register long x8 __asm__("x8") = T_SYS_IRQ_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_irq_wait — block until at least one IRQ has fired on the subscription
// represented by handle `h`. Returns the collapsed-fire count (>=1), or
// -1 on bad handle / wrong kind / missing T_RIGHT_SIGNAL.
__attribute__((always_inline))
static inline long t_irq_wait(long h) {
    register long x0 __asm__("x0") = h;
    register long x8 __asm__("x8") = T_SYS_IRQ_WAIT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_clock_gettime — fill `*ts` for `clk_id` (T_CLOCK_REALTIME/MONOTONIC) (LS-K).
// Returns 0 / -EINVAL (bad clk_id) / -EFAULT (bad va).
static inline long t_clock_gettime(unsigned long clk_id, struct t_timespec *ts) {
    register long x0 __asm__("x0") = (long)clk_id;
    register long x1 __asm__("x1") = (long)(unsigned long)ts;
    register long x8 __asm__("x8") = T_SYS_CLOCK_GETTIME;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_clock_settime — step CLOCK_REALTIME to `*ts` (net-7a). clk_id must be
// T_CLOCK_REALTIME; requires CAP_HOSTOWNER. Returns 0 / -EINVAL / -EFAULT /
// -EACCES.
static inline long t_clock_settime(unsigned long clk_id, const struct t_timespec *ts) {
    register long x0 __asm__("x0") = (long)clk_id;
    register long x1 __asm__("x1") = (long)(unsigned long)ts;
    register long x8 __asm__("x8") = T_SYS_CLOCK_SETTIME;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_mmio_map — install a user-VA mapping for a KObj_MMIO handle. The
// PTEs are installed lazily on first access (demand-paging). Returns 0
// on success, -1 on:
//   - cap-missing (defense)
//   - bad handle (range, wrong kind, missing T_RIGHT_MAP)
//   - prot exceeds rights / EXEC requested / prot == 0
//   - vaddr unaligned / overflow / overlaps existing VMA
//   - OOM
//
// Pinned by specs/handles.tla::HwHandleImpliesCap (cap held) +
// specs/burrow.tla::NoUseAfterFree (Burrow lifecycle bound to VMA).
__attribute__((always_inline))
static inline long t_mmio_map(long h, unsigned long vaddr, unsigned long prot) {
    register long x0 __asm__("x0") = h;
    register long x1 __asm__("x1") = (long)vaddr;
    register long x2 __asm__("x2") = (long)prot;
    register long x8 __asm__("x8") = T_SYS_MMIO_MAP;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_dma_create — allocate a KObj_DMA handle backed by `size` bytes of
// kernel-allocated contiguous pinned memory. Requires T_RIGHT_HW_CREATE
// capability. Size must be > 0 and ≤ 1 MiB at v1.0; kernel page-aligns
// up. Returns a non-negative handle index on success, -1 on EPERM /
// EINVAL (bad size) / EOOM. The buffer's PA is chosen by the kernel
// and stable for the handle's lifetime.
//
// Pinned by specs/handles.tla::HwHandleImpliesCap +
// HwResourceExclusive (via buddy's per-alloc partitioning).
__attribute__((always_inline))
static inline long t_dma_create(unsigned long size, unsigned long rights) {
    register long x0 __asm__("x0") = (long)size;
    register long x1 __asm__("x1") = (long)rights;
    register long x8 __asm__("x8") = T_SYS_DMA_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_attach_9p — wrap a Spoor pair (tx, rx) in a 9P client + drive the
// Tversion + Tattach handshake; return a KOBJ_SPOOR fd for the 9P
// tree's root. For duplex transports (Unix socket, vsock — Phase 5+),
// pass the same fd for both tx_fd and rx_fd. For half-duplex pipes,
// pass the matching write-end + read-end.
//
// aname is a server-side path or capability string (up to 256 bytes
// at v1.0). n_uname is 0 for no-auth attach at v1.0; Phase 5+
// authentication backends will use it as the per-user identifier.
//
// Returns the new fd (>=0) on success, -1 on:
//   - invalid tx_fd / rx_fd or missing R/W rights
//   - aname out of user-VA bound / aname_len > 256
//   - server-side Rlerror on Tversion or Tattach
//   - kmalloc OOM / handle table full
__attribute__((always_inline))
static inline long t_attach_9p(long tx_fd, long rx_fd,
                               const char *aname, size_t aname_len,
                               unsigned long n_uname) {
    register long x0 __asm__("x0") = tx_fd;
    register long x1 __asm__("x1") = rx_fd;
    register long x2 __asm__("x2") = (long)(unsigned long)aname;
    register long x3 __asm__("x3") = (long)aname_len;
    register long x4 __asm__("x4") = (long)n_uname;
    register long x8 __asm__("x8") = T_SYS_ATTACH_9P;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_mount — graft the Spoor at `source_spoor_fd` onto the mount-point
// directory named by the absolute `path` (`path_len` bytes) in the caller's
// Territory mount table (stalk-2: path-keyed; was an abstract path_id token).
// The kernel `stalk`s `path` from the Territory root to the mount point's
// (dc, devno, qid.path) identity and keys the entry on that. Plan 9 `mount`
// semantics: the mount entry holds its OWN refcount on the source Spoor (per
// ARCH §9.6.6), so the caller can `t_close(source_spoor_fd)` after a
// successful mount; the mount table keeps the Spoor alive until `t_unmount`
// or Territory destruction.
//
// The MOUNT POINT MUST EXIST as a walkable directory (Plan 9 M1; devramfs
// ships /srv + /proc, the disk FS provides its own).
//
// `flags` is T_MREPL / T_MBEFORE / T_MAFTER / T_MCREATE (bit-or'd).
//
// Returns 0 on success, -1 on:
//   - path absent / empty / too long / not resolvable
//   - invalid source_spoor_fd (not KOBJ_SPOOR or out-of-range)
//   - source handle missing T_RIGHT_READ
//   - flags has bits outside the valid set
//   - Territory mount table full (8 entries at v1.0)
__attribute__((always_inline))
static inline long t_mount(const char *path, unsigned long path_len,
                           long source_spoor_fd, unsigned long flags) {
    register long x0 __asm__("x0") = (long)path;
    register long x1 __asm__("x1") = (long)path_len;
    register long x2 __asm__("x2") = source_spoor_fd;
    register long x3 __asm__("x3") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_MOUNT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_unmount — remove the FIRST mount entry whose mount-point identity matches
// the absolute `path` (`path_len` bytes) from the caller's Territory mount
// table; drop the source's per-entry refcount. (Union mounts with multiple
// entries at the same point require multiple t_unmount calls — Phase 5+ once
// walk-side union support lands.) Returns 0 on success, -1 if no entry exists
// (or the path does not resolve).
__attribute__((always_inline))
static inline long t_unmount(const char *path, unsigned long path_len) {
    register long x0 __asm__("x0") = (long)path;
    register long x1 __asm__("x1") = (long)path_len;
    register long x8 __asm__("x8") = T_SYS_UNMOUNT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pipe — create a connected Spoor pair, install both as KOBJ_SPOOR
// handles in the calling process's table. Returns 0 on success with
// *out_rd_fd / *out_wr_fd populated; -1 on table-full / OOM.
__attribute__((always_inline))
static inline long t_pipe(long *out_rd_fd, long *out_wr_fd) {
    register long x0 __asm__("x0");
    register long x1 __asm__("x1");
    register long x8 __asm__("x8") = T_SYS_PIPE;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0), "=r"(x1)
        : "r"(x8)
        : "memory", "cc"
    );
    if (x0 < 0) return -1;
    *out_rd_fd = x0;
    *out_wr_fd = x1;
    return 0;
}

// t_read — read up to `len` bytes from `fd` into `buf`. Returns bytes
// read (>0), 0 on EOF, -1 on error. Per-call cap is 128 KiB
// (kernel-side SYS_RW_MAX, CF-3 A; a single 9P RPC's payload is still
// msize-clamped, so short reads stay normal); userspace loops.
__attribute__((always_inline))
static inline long t_read(long fd, void *buf, size_t len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x8 __asm__("x8") = T_SYS_READ;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_write — write up to `len` bytes from `buf` to `fd`. Returns bytes
// written (>=0), -1 on error.
__attribute__((always_inline))
static inline long t_write(long fd, const void *buf, size_t len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x8 __asm__("x8") = T_SYS_WRITE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pread — read up to `len` bytes from `fd` at absolute byte offset
// `off` (#37). The per-Spoor cursor is neither read nor advanced, so
// concurrent positioned ops on one fd share no mutable state (the POSIX
// pread contract). Returns bytes read (>0), 0 on EOF, -1 on error --
// including off < 0 and a non-seekable Dev (the ESPIPE shape: pread on
// a pipe fails). Per-call cap is SYS_RW_MAX (128 KiB, CF-3 A).
__attribute__((always_inline))
static inline long t_pread(long fd, void *buf, size_t len, long off) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x3 __asm__("x3") = off;
    register long x8 __asm__("x8") = T_SYS_PREAD;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pwrite — write up to `len` bytes from `buf` to `fd` at absolute byte
// offset `off` (#37). Cursor untouched; same gates as t_pread. Returns
// bytes written (>=0), -1 on error.
__attribute__((always_inline))
static inline long t_pwrite(long fd, const void *buf, size_t len, long off) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)len;
    register long x3 __asm__("x3") = off;
    register long x8 __asm__("x8") = T_SYS_PWRITE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_yield — voluntary yield (#33). If another thread is queued runnable on
// the calling CPU, the caller is requeued behind it and it runs; otherwise
// returns immediately. A hint (POSIX sched_yield shape); always returns 0.
__attribute__((always_inline))
static inline long t_yield(void) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = T_SYS_YIELD;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_setsid — become the leader of a new session + group (PTY-1a; POSIX
// setsid(2)). Returns the new sid (> 0), -T_E_ACCES (13, negated) if the
// caller is already a process-group leader, or -1.
__attribute__((always_inline))
static inline long t_setsid(void) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = T_SYS_SETSID;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_setpgid — move self (pid 0) or a live direct child into a process group
// (PTY-1a; POSIX setpgid(2)). pgid 0 mints a new group of the target's own
// pid. 0 on success; negative errno per the SYS_SETPGID contours (EPERM
// contours arrive as -13 EACCES).
__attribute__((always_inline))
static inline long t_setpgid(long pid, long pgid) {
    register long x0 __asm__("x0") = pid;
    register long x1 __asm__("x1") = pgid;
    register long x8 __asm__("x8") = T_SYS_SETPGID;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_getpgid / t_getsid — read a Proc's pgid / sid (PTY-1a; pid 0 = self).
// Returns the id or -3 (ESRCH) if no such Proc.
__attribute__((always_inline))
static inline long t_getpgid(long pid) {
    register long x0 __asm__("x0") = pid;
    register long x8 __asm__("x8") = T_SYS_GETPGID;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

__attribute__((always_inline))
static inline long t_getsid(long pid) {
    register long x0 __asm__("x0") = pid;
    register long x8 __asm__("x8") = T_SYS_GETSID;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pty_register — pts registry ops (PTY-1c; ptyfs the server is the only
// legal caller). op T_PTY_REG_MINT(conn_fd, master_qid, 0) -> pts_id > 0;
// T_PTY_REG_SLAVE(conn_fd, slave_qid, pts_id) -> 0; T_PTY_REG_FREE(pts_id,
// 0, 0) -> 0. Negative errno per the SYS_PTY_REGISTER contours.
__attribute__((always_inline))
static inline long t_pty_register(long op, long a1, long a2, long a3) {
    register long x0 __asm__("x0") = op;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x8 __asm__("x8") = T_SYS_PTY_REGISTER;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_tty_signal — report a signal-class event on a pts the caller MINTED
// (PTY-1d; the server half of the tty seam). Returns the posted count or
// negative errno per the SYS_TTY_SIGNAL contours.
__attribute__((always_inline))
static inline long t_tty_signal(long pts_id, long sig_class) {
    register long x0 __asm__("x0") = pts_id;
    register long x1 __asm__("x1") = sig_class;
    register long x8 __asm__("x8") = T_SYS_TTY_SIGNAL;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_tty_acquire — acquire the pts behind `slave_fd` as the calling session
// leader's controlling terminal (PTY-1d; POSIX acquisition semantics).
__attribute__((always_inline))
static inline long t_tty_acquire(long slave_fd) {
    register long x0 __asm__("x0") = slave_fd;
    register long x8 __asm__("x8") = T_SYS_TTY_ACQUIRE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_tty_set_fg / t_tty_get_fg — tcsetpgrp / tcgetpgrp (PTY-1d).
__attribute__((always_inline))
static inline long t_tty_set_fg(long fd, long pgid) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = pgid;
    register long x8 __asm__("x8") = T_SYS_TTY_SET_FG;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

__attribute__((always_inline))
static inline long t_tty_get_fg(long fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = T_SYS_TTY_GET_FG;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_tty_cont — resume job-stopped `pgid` via a slave fd of one's controlling
// terminal (PTY-1f; the shell's fg/bg). Gated like t_tty_set_fg; returns the
// visited-member count or -errno.
__attribute__((always_inline))
static inline long t_tty_cont(long fd, long pgid) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = pgid;
    register long x8 __asm__("x8") = T_SYS_TTY_CONT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_close — release the handle at `fd`. For KOBJ_SPOOR handles the
// kernel's release path routes through spoor_clunk (sets pipe EOF +
// wakes the other side per P5-pipe-blocking). Returns 0 on success,
// -1 on invalid fd.
__attribute__((always_inline))
static inline long t_close(long fd) {
    register long x0 __asm__("x0") = fd;
    register long x8 __asm__("x8") = T_SYS_CLOSE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_dup — duplicate `oldfd` with possibly-reduced rights into a new
// handle slot. `new_rights` MUST be a subset of oldfd's rights —
// elevation is rejected by the kernel's RightsCeiling enforcement.
// Returns the new fd (>=0) on success, -1 on invalid oldfd / rights
// elevation / table-full.
__attribute__((always_inline))
static inline long t_dup(long oldfd, unsigned long new_rights) {
    register long x0 __asm__("x0") = oldfd;
    register long x1 __asm__("x1") = (long)new_rights;
    register long x8 __asm__("x8") = T_SYS_DUP;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// =============================================================================
// P5-corvus-syscalls: v1.0 hardening syscalls (CORVUS-DESIGN.md §4.1.1).
// =============================================================================

// t_mlockall — pin all currently-mapped + future-mapped pages. Caller
// must hold T_RIGHT-equivalent CAP_LOCK_PAGES. Returns 0 on success,
// -1 on missing cap. Sets PROC_FLAG_MLOCKED on the calling Proc.
__attribute__((always_inline))
static inline long t_mlockall(unsigned long flags) {
    register long x0 __asm__("x0") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_MLOCKALL;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_set_dumpable — control core-dump permission. One-way to 0:
// t_set_dumpable(0) sets PROC_FLAG_NODUMP; t_set_dumpable(1) on a
// Proc that has the flag set is REFUSED. Returns 0 / -1.
__attribute__((always_inline))
static inline long t_set_dumpable(unsigned long dumpable) {
    register long x0 __asm__("x0") = (long)dumpable;
    register long x8 __asm__("x8") = T_SYS_SET_DUMPABLE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_set_traceable — control debug-Spoor attach permission. One-way to 0.
__attribute__((always_inline))
static inline long t_set_traceable(unsigned long traceable) {
    register long x0 __asm__("x0") = (long)traceable;
    register long x8 __asm__("x8") = T_SYS_SET_TRACEABLE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_explicit_bzero — compiler-barrier'd memset to zero of `len` bytes
// starting at `buf`. Per-call cap is SYS_RW_STACK (4096); loop for
// larger buffers. Returns 0 on success, -1 on user-VA validation
// failure. The kernel's per-byte uaccess_store_u8 path is the
// barrier — the compiler cannot elide the writes across the syscall
// boundary.
__attribute__((always_inline))
static inline long t_explicit_bzero(void *buf, size_t len) {
    register long x0 __asm__("x0") = (long)(unsigned long)buf;
    register long x1 __asm__("x1") = (long)len;
    register long x8 __asm__("x8") = T_SYS_EXPLICIT_BZERO;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_getrandom — read `len` random bytes into `buf` from the kernel
// CSPRNG. Caller must hold CAP_CSPRNG_READ. Per-call cap is SYS_RW_STACK
// (4096); loop for larger buffers. Returns bytes read (= len on
// success) or -1 on: missing cap / bad user-VA / CSPRNG unseeded /
// CSPRNG hardware fault.
__attribute__((always_inline))
static inline long t_getrandom(void *buf, size_t len, unsigned long flags) {
    register long x0 __asm__("x0") = (long)(unsigned long)buf;
    register long x1 __asm__("x1") = (long)len;
    register long x2 __asm__("x2") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_GETRANDOM;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn — load the named binary from the boot initrd (devramfs) and
// rfork(RFPROC) a child Proc that exec_setup's it. `name` need not be
// NUL-terminated; `name_len` is authoritative and must be in
// 1..T_SPAWN_NAME_MAX. The child inherits no capabilities at v1.0.
// Returns the child PID (>0) on success, -1 on: bad name buffer /
// missing binary / blob too large / OOM. Parent uses t_wait_pid to
// reap when the child exits.
__attribute__((always_inline))
static inline long t_spawn(const char *name, size_t name_len) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x8 __asm__("x8") = T_SYS_SPAWN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// WAIT_WNOHANG — t_wait_pid_for flag: do not block; return 0 if no
// matching zombie is ready. Mirrors POSIX WNOHANG and the kernel's
// WAIT_WNOHANG (proc.h); the value MUST match.
#define WAIT_WNOHANG 1

// PTY-1e job-control wait flags (mirror POSIX WUNTRACED/WCONTINUED and the
// kernel proc.h values). Passing either OPTS INTO the packed status
// encoding below (a plain wait keeps the raw exit status); a stop/continue
// REPORT returns the child's pid WITHOUT reaping it. want_pid extends to
// the POSIX group selectors: 0 = any child in the caller's process group;
// < -1 = any child in group -want_pid.
#define WAIT_UNTRACED  2
#define WAIT_CONTINUED 4

// The packed status encoding (only under WAIT_UNTRACED/WAIT_CONTINUED;
// the Linux wait(2) layout).
#define WAIT_STATUS_STOPPED       (0x7f | (20 << 8))
#define WAIT_STATUS_CONTINUED     0xffff
#define WAIT_IF_EXITED(st)        (((st) & 0x7f) == 0)
#define WAIT_EXITSTATUS(st)       (((st) >> 8) & 0xff)
#define WAIT_IF_STOPPED(st)       (((st) & 0xff) == 0x7f)
#define WAIT_IF_CONTINUED(st)     ((st) == 0xffff)

// t_wait_pid_for — reap a ZOMBIE child, filtered by pid and/or flags.
//   want_pid: -1 = any child; >0 = that specific child.
//   flags:    WAIT_WNOHANG = do not block.
// Returns the reaped PID (>0); 0 (WAIT_WNOHANG set + a matching child is
// alive but not yet a zombie); -1 (no matching child). Writes the child's
// exit_status to *status_out on a successful reap if non-NULL. Mirrors
// kernel/proc.c::wait_pid_for (POSIX waitpid(pid, flags) shape).
__attribute__((always_inline))
static inline long t_wait_pid_for(int want_pid, int flags, int *status_out) {
    register long x0 __asm__("x0") = (long)want_pid;
    register long x1 __asm__("x1") = (long)flags;
    register long x2 __asm__("x2") = (long)(unsigned long)status_out;
    register long x8 __asm__("x8") = T_SYS_WAIT_PID;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_wait_pid — reap ANY zombie child, blocking. Equivalent to
// t_wait_pid_for(-1, 0, status_out). Plan 9 wait(2) shape. Most callers
// (single-child rfork-then-reap) use this form.
__attribute__((always_inline))
static inline long t_wait_pid(int *status_out) {
    return t_wait_pid_for(-1, 0, status_out);
}

// t_spawn_with_fds — like t_spawn, but pre-install the listed fds in
// the child's handle table at slots 0..fd_count-1. Each fd must be an
// open KOBJ_SPOOR handle in the caller; fd_count ≤ T_SPAWN_MAX_FDS.
// The parent retains its own holds on each fd; the child gets its own
// independent ref (caller-side refcount unchanged).
//
// Returns the child PID (>0) on success, -1 on: bad name buffer /
// missing binary / blob too large / fd_count > T_SPAWN_MAX_FDS /
// fd_list bound violation / any listed fd is not KOBJ_SPOOR / OOM.
__attribute__((always_inline))
static inline long t_spawn_with_fds(const char *name, size_t name_len,
                                    const unsigned int *fds, size_t fd_count) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)fds;
    register long x3 __asm__("x3") = (long)fd_count;
    register long x8 __asm__("x8") = T_SYS_SPAWN_WITH_FDS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_with_caps — like t_spawn, but the child's caps are
// `caller's caps & cap_mask`. ARCH I-2 / I-6 monotonic-reduction is
// enforced structurally (the kernel-internal rfork_with_caps's AND
// can only reduce, never elevate). Setting bits in cap_mask that the
// caller doesn't hold has no effect — those bits are silently
// dropped at the AND.
//
// v1.0 cap bits (mirror kernel/include/thylacine/caps.h):
//   CAP_HW_CREATE   (1<<0) — hardware resource creation
//   CAP_LOCK_PAGES  (1<<1) — SYS_MLOCKALL
//   CAP_CSPRNG_READ (1<<2) — SYS_GETRANDOM
//
// Returns the child PID (>0) on success, -1 on the same conditions
// as t_spawn (bad name / missing binary / blob too large / OOM).
__attribute__((always_inline))
static inline long t_spawn_with_caps(const char *name, size_t name_len,
                                     unsigned long cap_mask) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)cap_mask;
    register long x8 __asm__("x8") = T_SYS_SPAWN_WITH_CAPS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_full — combination of t_spawn_with_fds + t_spawn_with_caps:
// child inherits the listed Spoor fds at slots 0..fd_count-1 AND gets
// cap-subset (caller's caps & cap_mask). Both inheritance modes apply
// independently with the same constraints and refcount discipline as
// the single-purpose variants. Used at P5-corvus-bringup where joey
// spawns /sbin/corvus with a pipe pair + CAP_LOCK_PAGES + CAP_CSPRNG_READ.
//
// Returns the child PID (>0) on success, -1 on the union of conditions
// from t_spawn_with_fds + t_spawn_with_caps.
__attribute__((always_inline))
static inline long t_spawn_full(const char *name, size_t name_len,
                                const unsigned int *fds, size_t fd_count,
                                unsigned long cap_mask) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)fds;
    register long x3 __asm__("x3") = (long)fd_count;
    register long x4 __asm__("x4") = (long)cap_mask;
    register long x8 __asm__("x8") = T_SYS_SPAWN_FULL;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_with_perms — extends t_spawn_full with a `perm_flags` bitmask
// that the kernel stamps on the child Proc atomically inside the spawn
// thunk (BEFORE the child's exec_setup, so the child never observes the
// un-stamped intermediate state). `perm_flags` is the bitwise-OR of
// T_SPAWN_PERM_* bits.
//
// Any nonzero perm_flags bit requires the calling Proc to be console-
// attached (the v1.0 local-console trust anchor). joey IS console-
// attached; an ordinary Proc that wandered into this call is rejected.
//
// At v1.0 the only defined bit is T_SPAWN_PERM_MAY_POST_SERVICE, used
// by joey to grant /sbin/corvus the right to call SYS_POST_SERVICE
// ("corvus"). The bit is NOT a cap (rfork would propagate caps); kernel-
// stamped at spawn so it cannot cross an rfork boundary.
//
// Returns the child PID (>0) on success, -1 on:
//   - perm_flags has unknown bits
//   - any perm_flags bit set AND caller is not console-attached
//   - any condition that t_spawn_full returns -1 on
__attribute__((always_inline))
static inline long t_spawn_with_perms(const char *name, size_t name_len,
                                      const unsigned int *fds, size_t fd_count,
                                      unsigned long cap_mask,
                                      unsigned long perm_flags) {
    register long x0 __asm__("x0") = (long)(unsigned long)name;
    register long x1 __asm__("x1") = (long)name_len;
    register long x2 __asm__("x2") = (long)(unsigned long)fds;
    register long x3 __asm__("x3") = (long)fd_count;
    register long x4 __asm__("x4") = (long)cap_mask;
    register long x5 __asm__("x5") = (long)perm_flags;
    register long x8 __asm__("x8") = T_SYS_SPAWN_WITH_PERMS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_spawn_full_argv — combined spawn primitive that extends t_spawn_with
// _perms with argv pass-through (P6-pouch-stratumd-boot 16b-alpha).
//
// Construct a `struct t_sys_spawn_args` on the caller's stack (or any
// addressable memory), fill every field, and call this wrapper with its
// address. The kernel uaccess-copies the struct, validates every field
// + sub-buffer, and dispatches to the spawn body. Same gate as
// t_spawn_with_perms: any nonzero perm_flags bit requires the calling
// Proc to be console-attached.
//
// argv encoding: `argv_data` is a flat buffer of `argc` concatenated
// NUL-terminated strings, total `argv_data_len` bytes (the last byte
// MUST be NUL). Example for argv=["pouch-hello-argv","alpha","beta"]:
//   argv_data = "pouch-hello-argv\0alpha\0beta\0"  (28 bytes)
//   argc      = 3
//   argv_data_len = 28
//
// Returns the child PID (>0) on success, -1 on any of:
//   - any field outside its documented bounds
//   - argv_data not NUL-terminated OR NUL count != argc
//   - any inherited fd is not a KOBJ_SPOOR handle
//   - any condition that t_spawn_with_perms returns -1 on
__attribute__((always_inline))
static inline long t_spawn_full_argv(const struct t_sys_spawn_args *req) {
    register long x0 __asm__("x0") = (long)(unsigned long)req;
    register long x8 __asm__("x8") = T_SYS_SPAWN_FULL_ARGV;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// A-5a (login + session). joey's boot->session-transition syscalls.

// t_boot_complete — signal "boot-test asserts passed". The kernel prints the
// "Thylacine boot OK" banner (one-shot). Caller must be console-attached (joey,
// pre-relinquish). Returns 0 on success, -1 if not console-attached.
static inline long t_boot_complete(void) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = T_SYS_BOOT_COMPLETE;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_console_relinquish — drop the caller's OWN console-attach (I-27). joey calls
// this at the bringup->session boundary so corvus is the sole console-attached
// Proc during a session. Returns 0, or -1 if the caller is not console-attached.
static inline long t_console_relinquish(void) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = T_SYS_CONSOLE_RELINQUISH;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_console_open — open /dev/cons and return a R|W KOBJ_SPOOR fd. The getty
// hands this to /sbin/login as its tty (fd 0/1/2). Returns the fd or -1.
static inline long t_console_open(void) {
    register long x0 __asm__("x0");
    register long x8 __asm__("x8") = T_SYS_CONSOLE_OPEN;
    __asm__ volatile (
        "svc #0"
        : "=r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_dma_map — install a user-VA mapping for a KObj_DMA handle and return
// the underlying PA. The PTEs are installed lazily on first access
// (demand-paging). Returns the buffer's PA (always non-negative since
// PA fits in 40 bits at v1.0) on success, -1 on validation failure
// (cap-missing, bad handle, prot validation, vaddr alignment, OOM).
//
// The driver embeds the returned PA into device-visible descriptors
// (e.g., VirtIO virtqueue desc.addr fields). Pinned by
// HwHandleImpliesCap + burrow.tla::NoUseAfterFree.
__attribute__((always_inline))
static inline long t_dma_map(long h, unsigned long vaddr, unsigned long prot) {
    register long x0 __asm__("x0") = h;
    register long x1 __asm__("x1") = (long)vaddr;
    register long x2 __asm__("x2") = (long)prot;
    register long x8 __asm__("x8") = T_SYS_DMA_MAP;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// struct t_pci_info — userspace mirror of the kernel's struct t_pci_info from
// <thylacine/syscall.h> (pci-1c). 208 bytes; field offsets pinned by the
// _Static_asserts below. Filled by t_pci_info (SYS_PCI_INFO). On LP64 aarch64
// `unsigned long` = u64, `unsigned int` = u32, `unsigned short` = u16,
// `unsigned char` = u8 -- matching the kernel fixed-width fields.
struct t_pci_bar {
    unsigned long pa;        //  0
    unsigned long size;      //  8
    unsigned char present;   // 16
    unsigned char is_64;     // 17
    unsigned char _pad[6];   // 18
};
struct t_pci_region {
    unsigned int  offset;    //  0
    unsigned int  length;    //  4
    unsigned char bar;       //  8
    unsigned char present;   //  9
    unsigned char _pad[2];   // 10
};
struct t_pci_info {
    struct t_pci_bar    bars[6];     //   0
    struct t_pci_region regions[4];  // 144
    unsigned int   notify_off_multiplier;  // 192
    unsigned int   intid;            // 196
    unsigned char  intid_valid;      // 200
    unsigned char  bus;              // 201
    unsigned char  dev;              // 202
    unsigned char  fn;               // 203
    unsigned short virtio_device_id; // 204
    unsigned char  _pad[2];          // 206
};
_Static_assert(sizeof(struct t_pci_bar) == 24, "libt t_pci_bar ABI size 24");
_Static_assert(sizeof(struct t_pci_region) == 12, "libt t_pci_region ABI size 12");
_Static_assert(sizeof(struct t_pci_info) == 208, "libt t_pci_info ABI size 208");
_Static_assert(__builtin_offsetof(struct t_pci_info, regions) == 144, "t_pci_info.regions @144");
_Static_assert(__builtin_offsetof(struct t_pci_info, intid)   == 196, "t_pci_info.intid @196");
_Static_assert(__builtin_offsetof(struct t_pci_info, virtio_device_id) == 204,
               "t_pci_info.virtio_device_id @204");

// t_pci_claim — claim the first VirtIO-PCI function matching `virtio_device_id`
// (1 = net, 4 = rng, ...). Returns a non-negative KOBJ_PCI handle (fixed rights
// R|W|MAP, non-transferable) on success, -1 on cap-missing / not-found /
// already-claimed / BAR-assign failure / OOM. Requires T_RIGHT_HW_CREATE.
__attribute__((always_inline))
static inline long t_pci_claim(unsigned long virtio_device_id) {
    register long x0 __asm__("x0") = (long)virtio_device_id;
    register long x8 __asm__("x8") = T_SYS_PCI_CLAIM;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pci_map_bar — map BAR `bar_index` of a KOBJ_PCI handle at `vaddr`. `prot`
// is bounded by the handle rights; EXEC + W-without-R are rejected. Returns 0
// on success, -1 on bad handle / out-of-range BAR / prot violation / overlap.
__attribute__((always_inline))
static inline long t_pci_map_bar(long h, unsigned long vaddr,
                                 unsigned long bar_index, unsigned long prot) {
    register long x0 __asm__("x0") = h;
    register long x1 __asm__("x1") = (long)vaddr;
    register long x2 __asm__("x2") = (long)bar_index;
    register long x3 __asm__("x3") = (long)prot;
    register long x8 __asm__("x8") = T_SYS_PCI_MAP_BAR;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_pci_info — fill `*out` with the KOBJ_PCI handle's resolved BAR + region map
// + INTID + bdf. Returns 0 on success, -1 on bad handle / bad pointer. `out`
// must be a writable buffer of at least sizeof(struct t_pci_info) bytes.
__attribute__((always_inline))
static inline long t_pci_info(long h, struct t_pci_info *out) {
    register long x0 __asm__("x0") = h;
    register long x1 __asm__("x1") = (long)(unsigned long)out;
    register long x8 __asm__("x8") = T_SYS_PCI_INFO;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_poll — block until one of `fds` (a slice of `struct pollfd`)
// becomes ready, or `timeout_ms` elapses. `timeout_ms`:
//   < 0 → block indefinitely;
//   = 0 → return immediately after the first scan;
//   > 0 → block for at most `timeout_ms` milliseconds.
//
// Returns the number of pollfds with `revents != 0` (≥ 0), 0 on
// timeout, or -1 on validation failure.
//
// libt callers consume `struct pollfd` + the T_POLL* event bits from
// <thylacine/poll.h>. The numeric constants match Linux for the future
// musl shim.
__attribute__((always_inline))
static inline long t_poll(void *fds, unsigned long nfds, long timeout_ms) {
    register long x0 __asm__("x0") = (long)(unsigned long)fds;
    register long x1 __asm__("x1") = (long)nfds;
    register long x2 __asm__("x2") = timeout_ms;
    register long x8 __asm__("x8") = 29; // T_SYS_POLL
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_cap_grant — register a pending CAP_HOSTOWNER grant for `target_stripes`
// with the kernel `cap` device (CORVUS-DESIGN.md §5.5.1; P5-hostowner-b-b).
// Caller must hold CAP_GRANT_HOSTOWNER. Returns 0 on success, -1 on gate
// fail / bad args / table full. At v1.0 only CAP_HOSTOWNER is grantable.
__attribute__((always_inline))
static inline long t_cap_grant(unsigned long cap_mask,
                               unsigned long target_stripes) {
    register long x0 __asm__("x0") = (long)cap_mask;
    register long x1 __asm__("x1") = (long)target_stripes;
    register long x8 __asm__("x8") = T_SYS_CAP_GRANT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_walk_open — walk a single path component from `spoor_fd` and open
// the result, returning a new opened KOBJ_SPOOR fd. The minimum walk-
// through-mount primitive at v1.0; composes with t_attach_9p to read a
// file served by a 9P server (P5-stratumd-stub-bringup-e1).
//
// Single component only — `name` must not contain '/' or '\0', and must
// not be "." or "..". Multi-component path resolution lands with the
// production open() syscall. Pass omode = T_OREAD / T_OWRITE / T_ORDWR
// (optionally OR'd with T_OTRUNC); other bits are rejected.
//
// The caller must hold RIGHT_READ on spoor_fd. The returned fd has
// R | W | TRANSFER — the server enforces the actual omode envelope.
//
// Returns the new fd (>=0) on success, -1 on:
//   - spoor_fd not KOBJ_SPOOR / missing RIGHT_READ
//   - the backing Dev has no walk or no open vtable op
//   - name_len == 0 or > 64
//   - name contains '/' / '\0' / equals "." / equals ".."
//   - omode has unrecognized bits
//   - server-side Rlerror on Twalk or Tlopen
//   - kernel-side OOM / handle table full
__attribute__((always_inline))
static inline long t_walk_open(long spoor_fd, const char *name,
                                size_t name_len, unsigned long omode) {
    register long x0 __asm__("x0") = spoor_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)name;
    register long x2 __asm__("x2") = (long)name_len;
    register long x3 __asm__("x3") = (long)omode;
    register long x8 __asm__("x8") = T_SYS_WALK_OPEN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_fd2path — copy the namespace name `fd` was reached by into `buf`
// (NUL-terminated) (#66; the Plan 9 fd2path(2)). Returns the path length
// (excluding NUL), 0 if the name is unknown (a real path begins with '/', so 0
// unambiguously means unknown), or -1 if `fd` is not a held KOBJ_SPOOR handle or
// `buf` is too small. No access RIGHT is required. The name is best-effort
// introspection metadata, never load-bearing (ARCHITECTURE.md I-33): it may be
// unknown OR stale (the path the fd was reached by, not a live lookup) -- do not
// use it as a re-open key.
__attribute__((always_inline))
static inline long t_fd2path(long fd, char *buf, size_t buf_len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)buf_len;
    register long x8 __asm__("x8") = T_SYS_FD2PATH;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_open — open a file at a (possibly multi-component) path resolved by the
// kernel `stalk` resolver (A-5b-0; SYS_OPEN; docs/STALK-DESIGN.md). `start_fd`
// is a KOBJ_SPOOR handle (RIGHT_READ) or T_WALK_OPEN_FROM_ROOT to resolve from
// the Territory root. `path` is '/'-separated (NUL-free); `omode` is as for
// t_walk_open (OREAD/OWRITE/ORDWR/OEXEC + OTRUNC; T_OPATH for a walk-only
// handle). Returns an opened (or O_PATH walkable) KOBJ_SPOOR fd (>= 0) or -1.
// Supersedes t_walk_open for paths of more than one component.
__attribute__((always_inline))
static inline long t_open(long start_fd, const char *path,
                          size_t path_len, unsigned long omode) {
    register long x0 __asm__("x0") = start_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)path;
    register long x2 __asm__("x2") = (long)path_len;
    register long x3 __asm__("x3") = (long)omode;
    register long x8 __asm__("x8") = T_SYS_OPEN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_walk_create — create-then-open the single component `name` inside the
// directory `parent_fd` (KOBJ_SPOOR with RIGHT_WRITE, or T_WALK_OPEN_FROM_ROOT
// for the Territory root). Returns a new opened KOBJ_SPOOR fd referring to the
// created object (file, or directory when perm carries T_WALK_CREATE_DMDIR).
// perm's low 9 bits are the rwxrwxrwx mode; the created object's group is the
// caller's primary_gid. omode is the Plan 9 open mode for the returned fd
// (T_OREAD / T_OWRITE / T_ORDWR [+ T_OTRUNC]); a directory is opened OREAD.
// Returns the new fd (>=0), -1 on any error (see SYS_WALK_CREATE). FS-mutation
// foundation (IDENTITY-DESIGN.md section 9.2).
#define T_WALK_CREATE_DMDIR  0x80000000u
__attribute__((always_inline))
static inline long t_walk_create(long parent_fd, const char *name,
                                  size_t name_len, unsigned long omode,
                                  unsigned long perm) {
    register long x0 __asm__("x0") = parent_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)name;
    register long x2 __asm__("x2") = (long)name_len;
    register long x3 __asm__("x3") = (long)omode;
    register long x4 __asm__("x4") = (long)perm;
    register long x8 __asm__("x8") = T_SYS_WALK_CREATE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_fsync — durability barrier on `fd` (KOBJ_SPOOR, RIGHT_WRITE). datasync 0 =
// full (data + metadata), non-zero = data only. Returns 0 / -1. FS-mutation
// foundation (IDENTITY-DESIGN.md section 9.2).
__attribute__((always_inline))
static inline long t_fsync(long fd, unsigned long datasync) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)datasync;
    register long x8 __asm__("x8") = T_SYS_FSYNC;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_readdir — read the next run of 9P2000.L dirents from a directory `fd`
// (KOBJ_SPOOR, RIGHT_READ) into `buf` (<= SYS_RW_STACK = 4096), advancing the Spoor's
// offset. Returns bytes written (>=0; 0 = end-of-directory), -1 on error. Each
// entry: qid(13) + offset(8 LE) + type(1) + name_len(2 LE) + name. FS-mutation
// foundation (IDENTITY-DESIGN.md section 9.2).
__attribute__((always_inline))
static inline long t_readdir(long fd, void *buf, size_t buf_len) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)buf;
    register long x2 __asm__("x2") = (long)buf_len;
    register long x8 __asm__("x8") = T_SYS_READDIR;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_rename — atomically rename/move the single component `oldname` in directory
// `olddir_fd` to `newname` in `newdir_fd` (both KOBJ_SPOOR directories with
// RIGHT_WRITE, or T_WALK_OPEN_FROM_ROOT). POSIX rename / 9P Trenameat: an
// existing destination is atomically replaced. olddir_fd and newdir_fd must be
// on the same Dev/session. Returns 0 / -1. FS-gamma (IDENTITY-DESIGN.md §9.3).
__attribute__((always_inline))
static inline long t_rename(long olddir_fd, const char *oldname, size_t oldname_len,
                            long newdir_fd, const char *newname, size_t newname_len) {
    register long x0 __asm__("x0") = olddir_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)oldname;
    register long x2 __asm__("x2") = (long)oldname_len;
    register long x3 __asm__("x3") = newdir_fd;
    register long x4 __asm__("x4") = (long)(unsigned long)newname;
    register long x5 __asm__("x5") = (long)newname_len;
    register long x8 __asm__("x8") = T_SYS_RENAME;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_unlink — remove the single component `name` from directory `parent_fd`
// (KOBJ_SPOOR, RIGHT_WRITE, or T_WALK_OPEN_FROM_ROOT). flags 0 = unlink a
// non-directory; T_UNLINK_REMOVEDIR = rmdir an empty directory. Returns 0 / -1.
// FS-gamma (IDENTITY-DESIGN.md §9.3).
__attribute__((always_inline))
static inline long t_unlink(long parent_fd, const char *name, size_t name_len,
                            unsigned long flags) {
    register long x0 __asm__("x0") = parent_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)name;
    register long x2 __asm__("x2") = (long)name_len;
    register long x3 __asm__("x3") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_UNLINK;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_wstat — apply chmod/chown to `fd` (KOBJ_SPOOR, RIGHT_WRITE) via Tsetattr.
// `valid` selects which of (mode, uid, gid) to set (T_WSTAT_* bits; >=1 bit).
// mode is the 9 rwx bits only (setuid/setgid/sticky rejected). Returns 0 / -1.
// A-2a (IDENTITY-DESIGN.md §9.5). The per-file permission policy is A-2d.
__attribute__((always_inline))
static inline long t_wstat(long fd, unsigned long valid, unsigned long mode,
                           unsigned long uid, unsigned long gid) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)valid;
    register long x2 __asm__("x2") = (long)mode;
    register long x3 __asm__("x3") = (long)uid;
    register long x4 __asm__("x4") = (long)gid;
    register long x8 __asm__("x8") = T_SYS_WSTAT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_chmod — set `fd`'s permission bits (9 rwx bits). Convenience over t_wstat.
__attribute__((always_inline))
static inline long t_chmod(long fd, unsigned long mode) {
    return t_wstat(fd, T_WSTAT_MODE, mode, 0, 0);
}

// t_chown — set `fd`'s owner + group. Convenience over t_wstat.
__attribute__((always_inline))
static inline long t_chown(long fd, unsigned long uid, unsigned long gid) {
    return t_wstat(fd, T_WSTAT_UID | T_WSTAT_GID, 0, uid, gid);
}

// t_chroot — stamp the calling Proc's Territory root_spoor to the Spoor
// named by `spoor_fd`. After this call, t_walk_open(T_WALK_OPEN_FROM_ROOT,
// name, ...) walks from this Spoor. The caller may close `spoor_fd`
// afterward; the Territory keeps its own ref on the underlying Spoor
// until a subsequent t_chroot replaces it (releasing the old) or until
// the calling Proc exits (Territory destruction releases it).
// P5-stratumd-stub-bringup-e2.
//
// Idempotent — t_chroot to the same Spoor a second time is a 0 no-op.
//
// Caller must hold T_RIGHT_READ on spoor_fd (the same gate SYS_MOUNT
// applies — a chroot target's only purpose is to serve as a walk
// source).
//
// Returns 0 on success, -1 on:
//   - spoor_fd not KOBJ_SPOOR / out-of-range / missing T_RIGHT_READ
//   - caller has no Territory (kernel invariant; defense-in-depth)
__attribute__((always_inline))
static inline long t_chroot(long spoor_fd) {
    register long x0 __asm__("x0") = spoor_fd;
    register long x8 __asm__("x8") = T_SYS_CHROOT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_chdir -- set the per-Proc cwd (LS-4 dot_path) to `path`. Requires
// X-search on the target dir (kernel perm_check); the target must be a
// directory. Returns 0 on success, -1 on failure.
static inline long t_chdir(const char *path, unsigned long path_len) {
    register long x0 __asm__("x0") = (long)(unsigned long)path;
    register long x1 __asm__("x1") = (long)path_len;
    register long x8 __asm__("x8") = T_SYS_CHDIR;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_cap_use — redeem a pending cap grant for the caller's own stripes.
// Caller must hold PROC_FLAG_CONSOLE_ATTACHED and have a non-expired
// pending grant for its stripes with a matching cap_mask. On success the
// caller's caps gains `cap_mask`; the grant is consumed (one-shot).
// Returns 0 on success, -1 on gate fail / no pending grant / mismatch.
__attribute__((always_inline))
static inline long t_cap_use(unsigned long cap_mask) {
    register long x0 __asm__("x0") = (long)cap_mask;
    register long x8 __asm__("x8") = T_SYS_CAP_USE;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_set_tid_address — register the calling thread's tid address and
// return its tid. A C runtime (pouch) calls this at thread startup; a
// Thylacine-native program rarely needs it directly. `tidptr` is now
// STORED on the Thread (P6-pouch-threads sub-chunk 9): on thread exit
// the kernel atomically zeroes *tidptr + torpor-wakes on it so a
// joiner observes the death. `tidptr == 0` is the "unset" sentinel.
//
// Returns the calling thread's tid (positive int — a per-Thread monotonic
// allocation; for the main Thread of a Proc this is NOT the pid in
// general). Returns -1 on user-VA validation failure (non-zero tidptr
// that is not 4-byte-aligned or outside the user-VA bound).
__attribute__((always_inline))
static inline long t_set_tid_address(void *tidptr) {
    register long x0 __asm__("x0") = (long)tidptr;
    register long x8 __asm__("x8") = T_SYS_SET_TID_ADDRESS;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// P6-pouch-threads (sub-chunk 9): spawn an EL0 Thread in the caller's Proc.
//
// `entry`: function pointer for the new Thread's EL0 entry — invoked with
//          x0 = arg per AAPCS64. The function must NOT return; it should
//          call t_thread_exit (or pouch's pthread_exit) to terminate
//          cleanly.
// `sp_top`: top of the new Thread's user stack. Must be 16-byte aligned
//           (AAPCS64 ABI). The Thread runs on this stack until exit.
// `arg`:    opaque value passed as x0 to `entry`.
// `tls`:    initial TPIDR_EL0 value (TLS base). 0 = no TLS (entry must
//           install one before any TLS deref).
//
// Returns the new Thread's tid (positive int) on success, or one of the
// Linux/musl-numeric -errnos:
//   -EINVAL (-22)   bad alignment / out-of-bound entry / sp / tls
//   -ENOMEM (-12)   Thread / kstack alloc fail
__attribute__((always_inline))
static inline long t_thread_spawn(void (*entry)(void *), void *sp_top,
                                  void *arg, void *tls) {
    register long x0 __asm__("x0") = (long)(unsigned long)entry;
    register long x1 __asm__("x1") = (long)(unsigned long)sp_top;
    register long x2 __asm__("x2") = (long)(unsigned long)arg;
    register long x3 __asm__("x3") = (long)(unsigned long)tls;
    /* #112: SYS_THREAD_SPAWN gained x4 = ptid (CLONE_PARENT_SETTID). libt's
     * C callers (thread-probe / thread-fault-probe) read the new tid from
     * the syscall return and have no parent-side tid word to publish, so x4
     * is pinned to 0 (no publish) -- but it MUST be set explicitly, or the
     * SVC would pass whatever garbage x4 held and the kernel would try to
     * write the tid to a random user VA. A C caller wanting PARENT_SETTID
     * would add the parameter then. */
    register long x4 __asm__("x4") = 0;
    register long x8 __asm__("x8") = T_SYS_THREAD_SPAWN;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// P6-pouch-threads (sub-chunk 9): exit the calling Thread. NEVER returns.
//
// The kernel:
//   1. If t_set_tid_address registered a non-zero tidptr on this Thread,
//      atomically zeroes the tidptr word + torpor-wakes UINT32_MAX
//      threads on it (pthread_join handoff).
//   2. Marks self EXITING.
//   3. If this is the LAST live Thread in the Proc, transitions the
//      Proc to ZOMBIE with exit_status = 0 (mirrors t_exits(0)).
//   4. sched(); never returns.
//
// pouch's pthread_exit wraps this; native callers (rare) can use it
// directly.
__attribute__((always_inline, noreturn))
static inline void t_thread_exit(void) {
    register long x8 __asm__("x8") = T_SYS_THREAD_EXIT;
    __asm__ volatile (
        "svc #0"
        :
        : "r"(x8)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

// SYS_EXIT_GROUP (ARCH 7.9.1, invariant I-24): terminate the WHOLE Proc --
// cascade peer-Thread termination -- with `status` (0 -> "ok", else "fail").
// NEVER returns. POSIX exit_group(2): the whole-process exit a multi-thread
// program wants. Every peer Thread self-exits at its EL0-return die-check; the
// last Thread out reaps the Proc to ZOMBIE with the status. pouch's _Exit /
// exit / abort route through __NR_exit_group -> SYS_EXIT_GROUP. A single-thread
// Proc gets t_exits(status)-equivalent semantics. Distinct from t_thread_exit
// (exits ONE Thread) and t_exits (which v1.0 REQUIRES all peers already
// joined; t_exit_group does the joining-by-shootdown instead).
__attribute__((noreturn, always_inline))
static inline void t_exit_group(long status) {
    register long x0 __asm__("x0") = status;
    register long x8 __asm__("x8") = T_SYS_EXIT_GROUP;
    __asm__ volatile (
        "svc #0"
        :: "r"(x0), "r"(x8)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

// P6-pouch-stratumd-boot 16b-gamma: native file metadata + offset cursor.

// struct t_stat — userspace mirror of the kernel's struct t_stat from
// <thylacine/syscall.h>. 88 bytes (A-2a appended uid+gid -> 80; #100 appended
// devno+pad -> 88); field offsets pinned by _Static_asserts at the end. Native
// callers consume this directly; pouch's fstat() translation layer (patch 0010)
// maps t_stat onto musl's struct stat for POSIX consumers like stratumd.
struct t_stat {
    unsigned long size;             //  0: file size in bytes
    unsigned long qid_path;         //  8: 9P qid.path
    unsigned long atime_sec;        // 16: access time (epoch seconds; 0 at v1.0)
    unsigned long mtime_sec;        // 24: modify time (0 at v1.0)
    unsigned long ctime_sec;        // 32: change time (0 at v1.0)
    unsigned int  mode;             // 40: POSIX mode bits (S_IFREG|0644 typical)
    unsigned int  nlink;            // 44: link count
    unsigned int  qid_vers;         // 48: 9P qid.vers
    unsigned char qid_type;         // 52: 9P qid.type (QTFILE / QTDIR / ...)
    unsigned char _pad_qid[3];      // 53: padding
    unsigned int  blksize;          // 56: preferred I/O size hint
    unsigned int  _pad_blksize;     // 60: padding
    unsigned long blocks;           // 64: count of 512-byte blocks
    unsigned int  uid;              // 72: A-2a owner principal-id
    unsigned int  gid;              // 76: A-2a owning group
    unsigned int  devno;            // 80: #100 per-instance device number (Chan.dev)
    unsigned int  _pad_dev;         // 84: pad to 8-byte alignment
};

_Static_assert(sizeof(struct t_stat) == 88,
               "libt t_stat must match kernel t_stat ABI (88 bytes; #100 devno). "
               "The kernel writes sizeof(t_stat) bytes into this buffer.");
_Static_assert(__builtin_offsetof(struct t_stat, size)      ==  0, "t_stat.size at 0");
_Static_assert(__builtin_offsetof(struct t_stat, qid_path)  ==  8, "t_stat.qid_path at 8");
_Static_assert(__builtin_offsetof(struct t_stat, mode)      == 40, "t_stat.mode at 40");
_Static_assert(__builtin_offsetof(struct t_stat, qid_type)  == 52, "t_stat.qid_type at 52");
_Static_assert(__builtin_offsetof(struct t_stat, blocks)    == 64, "t_stat.blocks at 64");
_Static_assert(__builtin_offsetof(struct t_stat, uid)       == 72, "t_stat.uid at 72");
_Static_assert(__builtin_offsetof(struct t_stat, gid)       == 76, "t_stat.gid at 76");
_Static_assert(__builtin_offsetof(struct t_stat, devno)     == 80, "t_stat.devno at 80 (#100)");

// Whence values for t_lseek — mirror kernel T_SEEK_* + POSIX SEEK_*.
#define T_SEEK_SET 0
#define T_SEEK_CUR 1
#define T_SEEK_END 2

// t_fstat — populate `*out` with metadata for `fd`. Returns 0 on success
// (*out filled), -1 on bad fd / no native stat / uaccess fault. `out`
// must be a writable buffer of at least sizeof(struct t_stat) bytes.
__attribute__((always_inline))
static inline long t_fstat(long fd, struct t_stat *out) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)(unsigned long)out;
    register long x8 __asm__("x8") = T_SYS_FSTAT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// t_stat_path — fill *out with the metadata of the file at `path` (POUNCE;
// SYS_STAT = 88). Resolves through the caller's Territory (absolute from
// root; relative joined with the cwd) with the standard per-component
// X-search; on a walk_attrs-capable Dev this is ONE 9P RPC and creates no
// handle/fid. Returns 0 on success, -errno (-T_E_NOENT / -T_E_ACCES) on a
// resolution failure, -1 on argument faults. `out` is the same 80-byte
// struct t_stat t_fstat fills.
__attribute__((always_inline))
static inline long t_stat_path(const char *path, size_t path_len,
                               struct t_stat *out) {
    register long x0 __asm__("x0") = (long)(unsigned long)path;
    register long x1 __asm__("x1") = (long)path_len;
    register long x2 __asm__("x2") = (long)(unsigned long)out;
    register long x8 __asm__("x8") = T_SYS_STAT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}


// t_lseek — reposition the per-Spoor offset cursor on `fd`. Returns the
// new offset (>= 0) on success, -1 on bad fd / bad whence / underflow /
// overflow / SEEK_END on a Dev without native stat.
__attribute__((always_inline))
static inline long t_lseek(long fd, long offset, long whence) {
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = offset;
    register long x2 __asm__("x2") = whence;
    register long x8 __asm__("x8") = T_SYS_LSEEK;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// P6-pouch-stratumd-boot 16c: 9P attach over a byte-mode KObj_Srv handle.
//
// Wrap the byte-mode SrvConn handle `srv_fd` in a kernel 9P client, drive
// Tversion + Tattach, and return a KOBJ_SPOOR fd for the 9P tree's root.
// Parallel to t_attach_9p but the transport is a SrvConn (the byte-mode
// flavour from a SYS_SRV_CONNECT against a stratumd-style service) instead
// of a Spoor pair. The caller must hold RIGHT_READ + RIGHT_WRITE on srv_fd.
//
// aname is a server-side path or capability string (up to 256 bytes at
// v1.0). n_uname is 0 for no-auth attach at v1.0; a Phase 5+ auth backend
// will use it as the per-user identifier.
//
// flags: 0 (strict close-to-open, the I-38 default) or T_ATTACH_9P_LOOSE
// (the B1 per-attach opt-in -- the caller asserts the single-writer
// premise for this attach; cached-opens then serve full Larder-hint hits
// without the per-open wire revalidation). Unknown bits reject.
//
// Returns the new fd (>=0) on success, -1 on:
//   - invalid srv_fd / wrong kind / missing R+W rights / not byte-mode
//   - aname out of user-VA bound / aname_len > 256
//   - unknown flags bits
//   - server-side Rlerror on Tversion or Tattach
//   - kmalloc OOM / handle table full
#define T_ATTACH_9P_LOOSE 0x1ul
__attribute__((always_inline))
static inline long t_attach_9p_srv(long srv_fd,
                                    const char *aname, size_t aname_len,
                                    unsigned long n_uname,
                                    unsigned long flags) {
    register long x0 __asm__("x0") = srv_fd;
    register long x1 __asm__("x1") = (long)(unsigned long)aname;
    register long x2 __asm__("x2") = (long)aname_len;
    register long x3 __asm__("x3") = (long)n_uname;
    register long x4 __asm__("x4") = (long)flags;
    register long x8 __asm__("x8") = T_SYS_ATTACH_9P_SRV;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

// P6-pouch-stratumd-boot 16c: atomic root_spoor swap for long-running Procs.
//
// Replace the caller's territory root_spoor with the Spoor named by
// `new_root_fd` (a KOBJ_SPOOR handle with RIGHT_READ). The old root_spoor
// is released. The caller MUST already have an initial root_spoor (a prior
// t_chroot); pivot_root rejects -1 otherwise. Idempotent on same-spoor.
//
// Returns 0 on success, -1 on bad fd / wrong kind / missing RIGHT_READ /
// no initial root_spoor.
__attribute__((always_inline))
static inline long t_pivot_root(long new_root_fd) {
    register long x0 __asm__("x0") = new_root_fd;
    register long x8 __asm__("x8") = T_SYS_PIVOT_ROOT;
    __asm__ volatile (
        "svc #0"
        : "+r"(x0)
        : "r"(x8)
        : "memory", "cc"
    );
    return x0;
}

#endif // THYLA_SYSCALL_H
