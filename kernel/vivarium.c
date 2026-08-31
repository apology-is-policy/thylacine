// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// The contract, the admission rule, and why this file is PURE are all in
// <thylacine/vivarium.h>. Read that first; this file is the data plus a lookup.

#include <thylacine/vivarium.h>

#include <thylacine/handle.h>           // V-5c-2: PROC_HANDLE_MAX = the fd clamp
#include <thylacine/notes.h>            // V-6b: the canonical note-name literals
#include <thylacine/page.h>             // D-3: PAGE_SIZE bounds the FILE arm's offset
#include <thylacine/poll.h>             // V-5c: POLL_MAX_NFDS bounds the domain
#include <thylacine/proc.h>             // #150: PRINCIPAL_SYSTEM / GID_SYSTEM
#include <thylacine/syscall.h>
#include <thylacine/types.h>

#include "../mm/slub.h"              // the fork-time sigtab clone (kzalloc)

// The native ceiling, pinned. vivarium.h cannot see syscall.h (the dependency is
// deliberately one-way), so the constant is declared there and checked here --
// the only file that can see both.
//
// This catches a RENUMBER of the current top. It cannot catch a NEW higher
// number by itself, which is why the rows that lean on the ceiling assert
// against VIV_NATIVE_CEILING individually below. The very drift this admits it
// cannot catch DID happen: the ceiling sat at SYS_RFORK (105) while the Warp arc
// landed SYS_DMA_CREATE_GPU_BO (106) and SYS_BURROW_FROM_HOSTMEM (107) above it,
// and this assert -- pinned to SYS_RFORK's identity -- passed the whole time. So
// it is re-pinned to the current top; the lesson is that "add a syscall" must
// include "move the ceiling", which no static_assert can force on a NEW number.
_Static_assert(VIV_NATIVE_CEILING == SYS_OPEN_CREATE,
               "VIV_NATIVE_CEILING must be the highest ASSIGNED native syscall "
               "number. Adding one above it silently voids the collision "
               "argument for every vivarium row at or below the new value -- "
               "bump the constant and re-run that check.");

// Every row that discharges its collision re-check by the ceiling argument
// rather than by a per-number one. The lowest such number is restart_syscall
// (128); execve (221) is the newest. If the ceiling ever rises past one of
// these, the build stops and the reader is forced back to the argument instead
// of inheriting a claim that has quietly become false.
_Static_assert(VIV_LINUX_RESTART_SYSCALL > VIV_NATIVE_CEILING,
               "the signal family's collision argument is the ceiling one");
_Static_assert(VIV_LINUX_SOCKET > VIV_NATIVE_CEILING,
               "the socket family's collision argument is the ceiling one");
_Static_assert(VIV_LINUX_CLONE > VIV_NATIVE_CEILING,
               "clone's collision argument is the ceiling one (LINEAGE L-3d)");
_Static_assert(VIV_LINUX_EXECVE > VIV_NATIVE_CEILING,
               "execve's collision argument is the ceiling one (LINEAGE L-6a)");
_Static_assert(VIV_LINUX_WAIT4 > VIV_NATIVE_CEILING,
               "wait4's collision argument is the ceiling one (LINEAGE L-6b)");
_Static_assert(VIV_LINUX_CLOCK_GETTIME > VIV_NATIVE_CEILING,
               "clock_gettime's collision argument is the ceiling one (time family)");
_Static_assert(VIV_LINUX_GETTIMEOFDAY > VIV_NATIVE_CEILING,
               "gettimeofday's collision argument is the ceiling one (time family)");
_Static_assert(VIV_LINUX_GETSOCKOPT > VIV_NATIVE_CEILING,
               "getsockopt's collision argument is the ceiling one (SO_ERROR row)");
_Static_assert(VIV_LINUX_SENDTO > VIV_NATIVE_CEILING,
               "sendto's collision argument is the ceiling one (send() shape)");
_Static_assert(VIV_LINUX_RECVFROM > VIV_NATIVE_CEILING,
               "recvfrom's collision argument is the ceiling one (recv() shape)");
_Static_assert(VIV_LINUX_RENAMEAT2 > VIV_NATIVE_CEILING,
               "renameat2's collision argument is the ceiling one (#50; its "
               "three sub-ceiling siblings 34/35/38 carry per-number "
               "paragraphs in vivarium.h instead)");
_Static_assert(VIV_LINUX_GETEUID > VIV_NATIVE_CEILING,
               "geteuid's collision argument is the ceiling one (git chunk; "
               "faccessat 48 is its sub-ceiling sibling with a per-number "
               "paragraph in vivarium.h instead)");
_Static_assert(VIV_LINUX_GETEGID > VIV_NATIVE_CEILING,
               "getegid's collision argument is the ceiling one (git chunk)");
_Static_assert(VIV_LINUX_GETRANDOM > VIV_NATIVE_CEILING,
               "getrandom's collision argument is the ceiling one (git chunk)");

// A T1 row: a Linux number, the Thylacine number it renumbers to, and the arity
// that must carry across unchanged. `nargs` is not used to copy (the whole
// six-word vector is copied verbatim — a Linux caller may leave the unused words
// as garbage and the native handler ignores them exactly as it does for a native
// caller). It is recorded because it documents WHICH words the equivalence claim
// actually covers, and the tests assert on it.
struct viv_row {
    u16 linux_nr;
    u16 thyla_nr;
    u8  nargs;
};

// -----------------------------------------------------------------------------
// TIER 1 — pure renumbers.
//
// Every row here satisfies the §4 rule in its strictest form: the argument words
// are identical in order, width, and MEANING, so the translation is the number
// substitution alone. Each row's equivalence was checked against the Thylacine
// side's documented contract in syscall.h, not against argument shape.
//
// The claims, one per row, so a future reader can re-check them without
// re-deriving:
//
//   read(fd, buf, count)        == SYS_READ(fd, buf_va, len)      syscall.h:105
//   write(fd, buf, count)       == SYS_WRITE(fd, buf_va, len)     syscall.h:106
//   close(fd)                   == SYS_CLOSE(fd)                  syscall.h:118
//   exit_group(status)          == SYS_EXIT_GROUP(status)         syscall.h:1276
//   lseek(fd, offset, whence)   == SYS_LSEEK(fd, offset, whence)  syscall.h:978
//
// `lseek` is the only row whose equivalence is not self-evident from the
// signature, because it carries an enumerated argument. It qualifies because
// T_SEEK_SET/CUR/END are 0/1/2 (syscall.h:955) and Linux's SEEK_SET/CUR/END are
// also 0/1/2 — the enumerations coincide, so there is no mapping to apply. Were
// they ever to diverge, this row would drop to T2 (a flag-bit mapping), not stay
// here. Both sides also reject an out-of-range whence, so the error semantics
// agree too.
//
// The negative space is as load-bearing as the rows, and is asserted by the
// tests so it cannot rot silently:
//
//   openat  — NOT a renumber, but a TIER-2 row (built at V-2b, below). Linux
//             passes a NUL-terminated path; SYS_OPEN wants an explicit path_len
//             (syscall.h:1340), so translating means SCANNING user memory for
//             the terminator -- which is why it cannot live in this pure table.
//             CORRECTION to the V-2a note that stood here: it said openat was
//             "total and stateless still". That is not right. V-2b found the
//             translation is total only over a STATED ARGUMENT DOMAIN --
//             O_CREAT and O_DIRECTORY have no SYS_OPEN counterpart at all -- so
//             the T2 translator checks each call's flags and forwards the rest.
//             See <vivarium.h> "THE ARGUMENT DOMAIN".
//   fstat   — NOT a renumber; also a TIER-2 row. `struct t_stat` is 88 bytes
//             (syscall.h:2143); Linux aarch64 `struct stat` is 128 with a
//             different field order. The translation is a struct conversion.
//             This one IS total: every t_stat maps, with no argument domain.
//   newfstatat — TIER-2 (V-2c). `stat()`/`lstat()` compile to THIS on aarch64,
//             so it is the row that matters for real binaries; SYS_STAT (88) is
//             its exact counterpart. See vivarium_fstatat_decide.
//   statx   — FORWARD, and it is the reason newfstatat is not the whole story.
//             musl-on-aarch64 defines no __NR_fstatat, so its fstatat.c compiles
//             the newfstatat path OUT and issues statx (291) instead -- verified
//             in third_party/musl, not assumed. Go and glibc DO use 79, and
//             those are the binaries VIVARIUM exists to run (a musl target we
//             could rebuild through pouch), so 79 is still the right row to
//             build first. statx wants a request MASK and a 256-byte struct with
//             per-field validity bits -- a bigger translator, not this shape.
//   mmap    — TIER-2 since V-2d. The V-2a note that stood here said "addr
//             hints, PROT_*, MAP_* and fd-backed mappings are POLICY ... the
//             'needs judgement' case the rule exists to exclude". The facts
//             were right; the conclusion was overtaken. §4.1 defers V-3, so
//             FORWARD now means ENOSYS rather than "the supervisor handles it",
//             and mmap is on musl's critical path twice (__init_tls.c:137 for
//             TLS, mallocng for every heap area) -- a guest cannot reach main()
//             without it. The STATED ARGUMENT DOMAIN (V-2b's tool, built for
//             exactly this) admits the shape musl actually sends and declines
//             the policy-bearing rest. See vivarium_mmap_decide.
//   munmap  — TIER-2 since V-2d, and still the instructive one. The arguments
//             line up perfectly -- munmap(addr, len) vs
//             SYS_BURROW_DETACH(vaddr, length) -- and it LOOKS like a free row.
//             It is not: burrow_detach requires an exact VMA match and refuses
//             a partial detach (syscall.h:611-620), while Linux permits partial
//             and multi-mapping unmaps AND *succeeds* on an unmapped range. So
//             a bare renumber is wrong in two directions, which is exactly what
//             "total" forbids. What rescues it is that the check already
//             exists: burrow_detach ITSELF enforces the exact match, so the T2
//             shell attempts it and reads the answer -- success means the
//             semantics were exactly Linux's, refusal declines. No pure
//             _decide exists for this row because the domain is a question
//             about STATE, not about arguments.
//   mprotect— ENOSYS, and recorded rather than left to the default. It would
//             reach ENOSYS anyway (no row -> vivarium_translate's fallthrough),
//             but this file's standard is that a number never considered and a
//             number considered and rejected are different facts. Thylacine has
//             NO prot-mutation syscall at all -- an I-12 design choice, not a
//             gap -- so there is nothing to translate to. musl tolerates this
//             BY CONSTRUCTION: mallocng/malloc.c:92 reads `if (mprotect(...)
//             && errno != ENOSYS) return 0;`.
//   brk     — ENOSYS, honestly. Thylacine's heap is Burrow-based; there is no
//             break pointer, so there is nothing to translate to. Reporting
//             ENOSYS lets a libc fall back to its mmap path, which is what musl
//             and glibc both do; faking success would strand the allocator.
// -----------------------------------------------------------------------------

static const struct viv_row g_viv_t1[] = {
    { VIV_LINUX_READ,       SYS_READ,       3 },
    { VIV_LINUX_WRITE,      SYS_WRITE,      3 },
    { VIV_LINUX_CLOSE,      SYS_CLOSE,      1 },
    { VIV_LINUX_LSEEK,      SYS_LSEEK,      3 },
    // pread64/pwrite64 (the git 6.27 clone arm): identical (fd, buf, count,
    // offset) shape to SYS_PREAD/SYS_PWRITE, and the native handlers validate
    // the buffer + gate on off>=0 / RIGHT_* exactly as read/write do -- so a
    // bare renumber is correct where read/write's is. git's index-pack reads
    // the cloned pack via pread; without this it FORWARDs to ENOSYS. The
    // sub-ceiling collision with the LOOM pair is the read/write renumbers'
    // damage-envelope argument (see vivarium.h).
    { VIV_LINUX_PREAD64,    SYS_PREAD,      4 },
    { VIV_LINUX_PWRITE64,   SYS_PWRITE,     4 },
    { VIV_LINUX_EXIT_GROUP, SYS_EXIT_GROUP, 1 },

    // #150. getpid is the ONLY member of the startup batch that is a clean
    // renumber, and stating why the others are not is the point of listing it
    // alone: getuid and getgid have equally exact native twins (73, 74) and
    // equally matching arity, and they are still shells -- because their VALUE
    // needs the sentinel mapping (vivarium_map_uid), which a renumber has no
    // place to perform. Same syscall shape, different row tier; the arity rule
    // is necessary for a T1 row, never sufficient.
    //
    // getpid qualifies on all three counts: zero arguments supplied and zero
    // read, a return that is a plain pid < 2^32 (never in the [-4095,-1] errno
    // band a Linux caller checks), and no error path at all in the native
    // handler beyond the current_thread() sanity check.
    { VIV_LINUX_GETPID,     SYS_GETPID,     0 },
};

#define VIV_T1_COUNT ((u32)(sizeof(g_viv_t1) / sizeof(g_viv_t1[0])))

// The calls we have DECIDED are not table rows. Keeping them as an explicit list
// rather than letting them fall through to a default matters: a Linux number that
// we have never considered and one we have considered and rejected are different
// facts, and collapsing them would lose the analysis. It also makes the V-2b work
// list mechanical — the T2 entries are exactly the FORWARD rows a translator
// later promotes.
struct viv_reject {
    u16              linux_nr;
    enum viv_verdict verdict;
};

static const struct viv_reject g_viv_rejects[] = {
    // THE fd-FREEING SET, and the reason it is listed at all. The V-5 socktab
    // keys on the fd NUMBER, so the entry has to be dropped whenever that
    // number is freed. Miss it and a freed index whose (proto, N) survives is
    // handed to the next fd-creating call, so a later connect() writes its dial
    // verb to a STRANGER'S connection -- the sharpest bug this family can have.
    //
    // TWO of them are rows that pay the obligation in DIFFERENT PLACES (#157),
    // and execve pays it a THIRD way (6b). `close` pays it in the ENTRY HOOK in
    // viv_linux_dispatch, which is correct there because a close whose fd carries
    // an entry always proceeds. `dup3` pays it INSIDE ITS SHELL, after every
    // refusal and immediately before the install, because a dup3 can be REFUSED
    // (bad flags, old == new, bad old) while `new` is a live socket -- an
    // unconditional entry-time drop would destroy the guest's socket state on a
    // call that failed. `execve` pays it in sys_execve_core via
    // viv_socktab_drop_cloexec, after the commit and before handle_close_on_exec:
    // the close-on-exec sweep frees cloexec fds through the handle_close
    // PRIMITIVE, which never enters viv_linux_dispatch and so never runs the hook.
    //
    // So the rule for a future promotion is not "extend the hook" but "pay it
    // in the arm your refusal structure demands". `close_range` still owes it,
    // and still looks like a trivial renumber that would silently reintroduce
    // the bug.
    //
    // `dup` (git-remote-https): git's transport-helper dup()s the helper's
    // output fd to wrap it in a FILE*, so the external-helper transports need
    // it -- FORWARD returned ENOSYS ("can't dup helper output fd"). It is an
    // fd-CREATING row, not fd-freeing (like pipe2 below), so it owes no socktab
    // DROP; but it DECLINES a socket SOURCE (ENOSYS, as dup3 does), because a
    // dup that did not also register the new number would hand back a socket fd
    // the socket path cannot recognize. Its shell arm lives beside dup3's.
    { VIV_LINUX_DUP,         VIV_TIER2   },  // git-remote-https helper pipe
    { VIV_LINUX_DUP3,        VIV_TIER2   },  // #157: vivarium_dup3_decide
    { VIV_LINUX_CLOSE_RANGE, VIV_FORWARD },

    // pipe2 is an fd-CREATING row, not an fd-freeing one, so it needs no hook
    // extension: it puts TWO FRESH indices into the table and frees nothing. It
    // sits in openat's class, which has been a plain T2 row since V-2b for
    // exactly the same reason. (The soundness of every fd-creating row still
    // rests on the block above -- a recycled index must arrive with no stale
    // socktab entry attached, which holds while close is the only freeing row.)
    { VIV_LINUX_PIPE2,      VIV_TIER2   },  // #155: vivarium_pipe2_decide
    { VIV_LINUX_OPENAT,     VIV_TIER2   },  // V-2b: vivarium_openat_decide/_build
                                            //  + #50: the O_CREAT domain
                                            //    (vivarium_openat_create_decide)

    // The path-mutation family (#50; VIVARIUM.md section 6.24) -- the rows
    // git's writes stand on (init alone needs all four: object dirs, the
    // config lockfile, its unlink, its rename-into-place). Each shell splits
    // the path lexically, stalks the parent, and runs the SAME extracted
    // native mechanics (spoor_create_install / spoor_unlink_in_dir /
    // spoor_rename_in_dirs) the fd-based syscalls run -- I-43 by
    // construction. 34/35/38 sit below the native ceiling and carry
    // per-number collision paragraphs in vivarium.h.
    { VIV_LINUX_MKDIRAT,    VIV_TIER2   },  // #50: vivarium_mkdirat_decide
    { VIV_LINUX_UNLINKAT,   VIV_TIER2   },  // #50: vivarium_unlinkat_decide
    { VIV_LINUX_RENAMEAT,   VIV_TIER2   },  // #50: vivarium_renameat_decide
    { VIV_LINUX_RENAMEAT2,  VIV_TIER2   },  // #50: same decide, flags==0 only
    // The directory-read + durability rows (section 6.25). getdents64 is the
    // 9P-dirent -> linux_dirent64 re-encode over the SAME spoor_readdir_run
    // the native SYS_READDIR runs; fsync/fdatasync delegate to the native
    // fsync core with an explicit datasync 0/1 (never a stale register).
    // 61/82/83 sit below the ceiling; damage-envelope paragraphs in vivarium.h.
    { VIV_LINUX_GETDENTS64, VIV_TIER2   },  // getdents64(fd, dirp, count)
    { VIV_LINUX_FSYNC,      VIV_TIER2   },  // fsync(fd)     -> datasync=0
    { VIV_LINUX_FDATASYNC,  VIV_TIER2   },  // fdatasync(fd) -> datasync=1
    { VIV_LINUX_FSTAT,      VIV_TIER2   },  // V-2b: vivarium_stat_to_linux
    { VIV_LINUX_NEWFSTATAT, VIV_TIER2   },  // V-2c: vivarium_fstatat_decide
    { VIV_LINUX_MMAP,       VIV_TIER2   },  // V-2d: vivarium_mmap_decide
    { VIV_LINUX_MUNMAP,     VIV_TIER2   },  // V-2d: the exact-match subset
    { VIV_LINUX_MPROTECT,   VIV_ENOSYS  },  // V-2d: no prot-mutation syscall (I-12)
    { VIV_LINUX_STATX,      VIV_FORWARD },  // wants a mask + a 256-byte struct
    { VIV_LINUX_BRK,        VIV_ENOSYS  },  // no counterpart; libc falls to mmap

    // The signal family (V-6, §6.22).
    { VIV_LINUX_RT_SIGACTION,   VIV_TIER2 },  // V-6b: vivarium_sigaction_decide
    { VIV_LINUX_RT_SIGPROCMASK, VIV_TIER2 },  // V-6b: vivarium_sigprocmask_decide
    //
    // STILL NOT LISTED, deliberately: kill (129), tkill (130), tgkill (131).
    // Their shells do not exist, and a VIV_TIER2 row whose shell is missing
    // would be a table that DECLARES a capability the code does not have --
    // `viv_tier2`'s default arm calls exactly that a "table/shell disagreement"
    // and fails closed. So a row lands WITH its shell, in the same commit, and
    // until then the number FORWARDs like any unclassified call. (They are also
    // the only signal rows that are an AUTHORITY question rather than a
    // disposition one: they name another Proc, so they must reuse an existing
    // cross-Proc gate verbatim -- SYS_POSTNOTE's parent-only check or I-26's
    // two-axis one -- never invent a third.)
    //
    // The ENOSYS rows below are NOT in that state -- they are live decisions,
    // correct today, and each has its own reason rather than a blanket
    // "not yet":
    //   sigaltstack   — an alternate signal stack is only meaningful once
    //                   delivery honours SA_ONSTACK, which it does not.
    //   rt_sigsuspend — atomically swap the mask and sleep; needs the mask to be
    //                   a wait predicate, which note_mask is not.
    //   rt_sigpending — Thylacine's queue is per-Proc and consumed exactly once
    //                   (I-19 N-2); "pending but undelivered" is not a state it
    //                   distinguishes.
    //   rt_sigtimedwait / rt_sigqueueinfo — queued siginfo, which notes do not
    //                   carry (a note has a 16-byte name and one u32 arg).
    //   restart_syscall — the kernel-internal restart continuation, meaningless
    //                   without SA_RESTART.
    { VIV_LINUX_SIGALTSTACK,     VIV_ENOSYS },
    { VIV_LINUX_RT_SIGSUSPEND,   VIV_ENOSYS },
    { VIV_LINUX_RT_SIGPENDING,   VIV_ENOSYS },
    { VIV_LINUX_RT_SIGTIMEDWAIT, VIV_ENOSYS },
    { VIV_LINUX_RT_SIGQUEUEINFO, VIV_ENOSYS },
    { VIV_LINUX_RESTART_SYSCALL, VIV_ENOSYS },

    // The socket family (V-5, section 5.5). socket + connect land at V-5a with
    // their shells; the rest are honest declines until their own sub-chunk,
    // under the same rule the signal family states above -- a VIV_TIER2 row
    // whose shell does not exist would declare a capability the code lacks.
    { VIV_LINUX_SOCKET,      VIV_TIER2   },  // V-5a: vivarium_socket_decide
    { VIV_LINUX_CONNECT,     VIV_TIER2   },  // V-5a: the ctl-write + fd swap
    //
    // V-5b (the server path). A coherent group -- a listening socket is a bind
    // remembered, an announce written, and an accept that re-walks -- so they
    // landed together.
    { VIV_LINUX_BIND,        VIV_TIER2   },  // V-5b: remembered, not written
    { VIV_LINUX_LISTEN,      VIV_TIER2   },  // V-5b: the announce ctl verb
    { VIV_LINUX_ACCEPT,      VIV_TIER2   },  // V-5b: open(listen) -> M -> data
    { VIV_LINUX_ACCEPT4,     VIV_TIER2   },  // V-5b: accept + a refused flags word
    //
    // V-5a's remainder (the client path's edges): each needs the ctl re-walk,
    // which lands with them.
    { VIV_LINUX_SHUTDOWN,    VIV_FORWARD },
    { VIV_LINUX_GETSOCKNAME, VIV_FORWARD },
    { VIV_LINUX_GETPEERNAME, VIV_FORWARD },
    // sendto/recvfrom left the FORWARD block above (curl-demo chunk): aarch64
    // has NO plain send/recv syscall -- musl's send() IS sendto(fd, buf, len,
    // flags, NULL, 0) and recv() IS recvfrom(..., NULL, NULL) -- so every
    // Linux binary that sends on a socket via send() (curl does; busybox wget
    // happens to use write()) reaches these numbers. The T2 shells serve
    // EXACTLY the connected-socket send()/recv() shape by delegating to the
    // native write/read handlers (the same data path a T1-renumbered
    // read/write takes), and decline everything else -- the with-address
    // datagram shape, MSG_PEEK, MSG_DONTWAIT -- back to ENOSYS. The full
    // per-argument argument: vivarium_sendto_decide / vivarium_recvfrom_decide.
    { VIV_LINUX_SENDTO,      VIV_TIER2   },
    { VIV_LINUX_RECVFROM,    VIV_TIER2   },
    //
    // Live decisions, not deferrals:
    //   socketpair    — AF_UNIX only in practice, and /srv has no
    //                   mint-a-connected-pair operation; a guest that wanted a
    //                   pipe should use pipe2.
    //   setsockopt    — /net exposes no option surface at all. Answering
    //                   "success" to a TCP_NODELAY the stack ignores is the
    //                   silent lie this tier exists to prevent; ENOSYS lets the
    //                   guest's own fallback run. (Linux libcs routinely
    //                   tolerate a failing setsockopt for tuning options.)
    //   sendmsg /     — scatter-gather plus control messages (SCM_RIGHTS is fd
    //   recvmsg         passing, which is I-4's domain, not a socket detail).
    { VIV_LINUX_SOCKETPAIR,  VIV_ENOSYS },
    { VIV_LINUX_SETSOCKOPT,  VIV_ENOSYS },
    // getsockopt LEFT the blanket refusal above (curl-demo chunk): its
    // (SOL_SOCKET, SO_ERROR) point is a READ of connection state, not a tuning
    // knob -- and refusing it turns every libcurl consumer's SUCCEEDED connect
    // into a reported failure (verifyconnect treats a getsockopt error as a
    // connect error). The T2 shell serves exactly that point and declines the
    // rest to ENOSYS, so every other option behaves as it did under the
    // blanket row. The full honesty argument: vivarium_getsockopt_decide's
    // header comment.
    { VIV_LINUX_GETSOCKOPT,  VIV_TIER2  },
    // sendmsg LEFT at ENOSYS: musl's DNS resolver uses it only for the TCP
    // fallback (start_tcp, on a truncated UDP reply), which is off the git-clone
    // happy path (A records fit one UDP datagram). recvmsg is SERVED (N-2b): the
    // T2 shell reads one datagram + synthesizes msg_name from the recorded
    // remote, which is how res_msend RECEIVES every UDP reply. Control messages
    // (SCM_RIGHTS fd-passing, I-4's domain) are NOT served -- msg_control is
    // reported empty; a cmsg-bearing recvmsg gets no ancillary data, not a lie.
    { VIV_LINUX_SENDMSG,     VIV_ENOSYS },
    { VIV_LINUX_RECVMSG,     VIV_TIER2  },  // N-2b: viv_sock_recvmsg

    // Readiness (V-5c, section 5.5.4). aarch64 has no plain poll/select, so
    // these two carry the whole family -- musl's poll() and select() are
    // wrappers over them, and a server that multiplexes reaches them either way.
    { VIV_LINUX_PPOLL,       VIV_TIER2   },  // V-5c: the ready-file translation
    { VIV_LINUX_PSELECT6,    VIV_TIER2   },  // V-5c-2: the fd_set reshape

    // Process creation (LINEAGE L-3d). One row, and it lands WITH its shell in
    // the same commit under this file's own rule -- a VIV_TIER2 row whose shell
    // is missing declares a capability the code does not have.
    //
    { VIV_LINUX_CLONE,       VIV_TIER2   },  // L-3d: vivarium_clone_decide
    { VIV_LINUX_EXECVE,      VIV_TIER2   },  // L-6a: viv_execve
    { VIV_LINUX_WAIT4,       VIV_TIER2   },  // L-6b: vivarium_wait4_decide

    // The startup batch (#150, LINEAGE L-6c). Every row here was MEASURED --
    // this is the census a running busybox printed, not a guess at what a libc
    // might want -- and each is a shell rather than a renumber for a reason
    // named at its declaration in vivarium.h.
    { VIV_LINUX_WRITEV,          VIV_TIER2 },  // the arity rule's sharpest case
    { VIV_LINUX_GETCWD,          VIV_TIER2 },  // +1 on the length, and ERANGE
    { VIV_LINUX_GETPPID,         VIV_TIER2 },  // no native twin exists
    { VIV_LINUX_GETUID,          VIV_TIER2 },  // the PRINCIPAL_SYSTEM mapping
    { VIV_LINUX_GETGID,          VIV_TIER2 },  // the GID_SYSTEM mapping
    { VIV_LINUX_UNAME,           VIV_TIER2 },  // a fabrication; see the header
    { VIV_LINUX_SET_TID_ADDRESS, VIV_TIER2 },  // an errno translation, only
    { VIV_LINUX_SETUID,          VIV_TIER2 },  // EPERM, except the no-op
    { VIV_LINUX_SETGID,          VIV_TIER2 },

    // fcntl -- TIER-2 since #151, and the row whose disposition was MEASURED
    // rather than reasoned twice over. The #150 note here recorded it as ENOSYS
    // because THYLACINE HAD NO CLOSE-ON-EXEC AT ALL, so both cmds busybox
    // actually issues (F_SETFD(FD_CLOEXEC) and F_DUPFD_CLOEXEC) could only have
    // been answered by silently succeeding -- storing nothing, reporting done,
    // and letting the guest exec with the fd still open having been told
    // otherwise. That is still true of a translation-only fix, which is why
    // #151 built the FEATURE (a per-slot flag in struct HandleTable, consumed
    // by a sweep in execve) before serving the row.
    //
    // The cmd-level decisions live in vivarium_fcntl_decide; the header carries
    // why the served set is what it is. What belongs HERE is the reason this is
    // Tier-2 rather than a renumber, which is the arity rule at its sharpest
    // after writev: F_DUPFD_CLOEXEC(fd, min) looks like a candidate for SYS_DUP,
    // and SYS_DUP's second argument is a RIGHTS MASK, not a minimum fd. A
    // renumber would read `10` as a set of capability bits and hand back a
    // descriptor with arbitrary authority -- silently, for a legal input.
    { VIV_LINUX_FCNTL,           VIV_TIER2 },

    // The time family. TIER-2 rather than a renumber for two different reasons,
    // one per row. clock_gettime's timespec IS byte-identical to the native one,
    // so it LOOKS like a T1 renumber -- but its clk_id domain is not total (Linux
    // has ids Thylacine cannot serve), so it drops to T2 (the lseek precedent)
    // and the shell maps the id. gettimeofday has no native counterpart at all
    // and its timeval carries MICROseconds where the native clock speaks
    // nanoseconds -- a real struct conversion. Both shells land in this commit.
    { VIV_LINUX_CLOCK_GETTIME,   VIV_TIER2 },  // clk_id map -> SYS_CLOCK_GETTIME
    { VIV_LINUX_GETTIMEOFDAY,    VIV_TIER2 },  // realtime ns -> timeval {sec,usec}

    // The git chunk (VIVARIUM.md section 6.26). faccessat is a real translation
    // (stat + perm_check); geteuid/getegid are the one-principal twins of
    // getuid/getgid.
    { VIV_LINUX_FACCESSAT,       VIV_TIER2 },  // stat + perm_check(mode&7)
    { VIV_LINUX_CHDIR,           VIV_TIER2 },  // measure len -> SYS_CHDIR
    { VIV_LINUX_FCHMODAT,        VIV_TIER2 },  // open O_PATH -> wstat(MODE)
    { VIV_LINUX_READLINKAT,      VIV_TIER2 },  // NOFOLLOW stalk -> .readlink
    { VIV_LINUX_GETEUID,         VIV_TIER2 },  // == getuid (one principal)
    { VIV_LINUX_GETEGID,         VIV_TIER2 },  // == getgid (one principal)
    { VIV_LINUX_GETRANDOM,       VIV_TIER2 },  // -> SYS_GETRANDOM (CAP-gated)
};

#define VIV_REJECT_COUNT ((u32)(sizeof(g_viv_rejects) / sizeof(g_viv_rejects[0])))

enum viv_verdict vivarium_translate(u64 linux_nr, const u64 *args_in,
                                    struct viv_call *out) {
    // Fail closed. A caller that ignores the verdict must never be handed a
    // dispatchable number, so a bad call site degrades to ENOSYS rather than to
    // an arbitrary syscall.
    if (!args_in || !out) return VIV_ENOSYS;

    // A number outside the u16 band cannot be in either table; reject before the
    // narrowing comparison so a 64-bit value cannot alias a real row.
    if (linux_nr > 0xFFFFu) return VIV_FORWARD;

    for (u32 i = 0; i < VIV_T1_COUNT; i++) {
        if (g_viv_t1[i].linux_nr != (u16)linux_nr) continue;

        out->nr = (u64)g_viv_t1[i].thyla_nr;
        for (u32 a = 0; a < VIV_NARGS; a++) out->args[a] = args_in[a];
        return VIV_TRANSLATED;
    }

    for (u32 i = 0; i < VIV_REJECT_COUNT; i++)
        if (g_viv_rejects[i].linux_nr == (u16)linux_nr)
            return g_viv_rejects[i].verdict;

    // Not yet classified. FORWARD is the right default rather than ENOSYS: the
    // supervisor is where unclassified calls belong under Option C, and claiming
    // "this does not exist" about a call we simply have not reached would be a
    // lie the guest cannot distinguish from a real one.
    return VIV_FORWARD;
}

// =============================================================================
// TIER 2 — the translators (V-2b).
// =============================================================================

// The Plan 9 open modes SYS_OPEN's `omode` argument carries. The KERNEL declares
// no constants for these (only the `SYS_WALK_OPEN_OMODE_VALID` mask at
// syscall.h:2376 and the prose at :2363); userspace spells them T_OREAD.. in
// libt's syscall.h:232. Named here so the flag map below reads as a mapping
// between two vocabularies rather than a shuffle of magic numbers.
enum {
    VIV_OMODE_READ   = 0u,
    VIV_OMODE_WRITE  = 1u,
    VIV_OMODE_RDWR   = 2u,
    VIV_OMODE_TRUNC  = 0x10u,   // == T_OTRUNC
    VIV_OMODE_APPEND = 0x40u,   // == SYS_WALK_OPEN_OAPPEND (the git 6.27 arm)
};

// The `openat` flags whose effect SYS_OPEN can honour EXACTLY. Every bit outside
// this set forwards. Each admission is a claim about behaviour, so each is
// justified individually -- "we ignore it and nothing seems to break" is not a
// justification, it is the bug.
//
//   O_RDONLY/O_WRONLY/O_RDWR  the access mode; maps onto OREAD/OWRITE/ORDWR.
//   O_TRUNC                   maps onto OTRUNC.
//   O_PATH                    maps onto SYS_WALK_OPEN_OPATH -- the same "walk to
//                             it, do not open it" object on both sides.
//
//   O_CLOEXEC   HONOURED FOR REAL since #151, via the `cloexec_out` output --
//               the flag names a property of the resulting DESCRIPTOR, not of
//               the open, so it cannot ride in the omode and the shell applies
//               it after SYS_OPEN returns.
//
//               IT WAS ADMITTED AS A NO-OP BEFORE THAT, on this rationale: "a
//               spawned child inherits no Spoor handles and SYS_SPAWN_WITH_FDS
//               passes an EXPLICIT list, so the behaviour O_CLOEXEC asks for is
//               what we do unconditionally". That was TRUE WHEN WRITTEN and the
//               LINEAGE arc voided it -- L-2a's execve preserves the handle
//               table across an image replacement, L-3c-1's rfork copies it --
//               so from those commits onward an fd without the flag really did
//               cross exec and this admission became a silent wrong answer.
//               Neither commit had any reason to look at an openat flag table.
//               A rationale is only as durable as the fact under it.
//   O_NOCTTY    Same shape. It requests "do not make this my controlling
//               terminal"; Thylacine acquires a ct only through the explicit
//               SYS_TTY_ACQUIRE (PTY-1), never implicitly on open. Already
//               relied on: the pouch pty patch opens /dev/pts/ptmx O_RDWR|O_NOCTTY.
//   O_LARGEFILE Requests ">2 GiB offsets permitted". Every Thylacine offset is
//               64-bit, so this is unconditionally true -- exactly as it is on
//               64-bit Linux, whose kernel force-sets it internally.
//
// The three admissions above share one structure: the flag requests behaviour we
// ALREADY provide unconditionally, so honouring it is a no-op AND correct. That
// is the only reason a flag may be ignored. Contrast the rejects below.
//
//   O_NOFOLLOW  ADMITTED AND TRANSLATED since DISTRO D-1 -- the resolver now
//               follows symlinks, so the flag maps 1:1 onto
//               SYS_WALK_OPEN_NOFOLLOW (real semantics, not a no-op: a final
//               symlink answers ELOOP, and with O_PATH the returned handle IS
//               the link -- both the Linux contours). This closes the very trap
//               the V-2b reject named ("would become wrong the moment symlinks
//               land, with nothing to catch it"): the moment arrived, and the
//               flag rode the same chunk that landed the feature.
#define VIV_OPENAT_ADMITTED                                                  \
    ((u32)(VIV_O_ACCMODE | VIV_O_TRUNC | VIV_O_APPEND | VIV_O_PATH |         \
           VIV_O_NOFOLLOW | VIV_O_CLOEXEC | VIV_O_NOCTTY | VIV_O_LARGEFILE))

// Why each notable rejected flag is rejected -- i.e. why ignoring it would be a
// SILENT WRONG ANSWER rather than a no-op:
//
//   O_CREAT      SYS_OPEN cannot create. Creation is SYS_WALK_CREATE, a different
//                syscall taking a `perm`. Ignoring the bit turns "create it if
//                absent" into ENOENT.
//                CORRECTION to the V-2b note that stood here ("V-2c could route
//                it; that is a second target, not a flag map"): V-2c checked,
//                and routing it is NOT admissible -- three independent blockers,
//                any one fatal. (1) SHAPE: SYS_WALK_CREATE takes a SINGLE
//                COMPONENT name and rejects '/' (syscall.h:1105); openat takes a
//                path. Routing means splitting the path, resolving the parent as
//                a separate O_PATH open, and closing that handle on every exit --
//                two syscalls and an intermediate handle, i.e. the state and
//                logic section 4 excludes. (2) SEMANTICS: plain O_CREAT (no
//                O_EXCL) means "create if absent, OPEN if present", and
//                SYS_WALK_CREATE always creates, returning -EEXIST otherwise. A
//                try-create-then-open retry is control flow, not a mapping.
//                (3) The sharpest, because it is silent: SYS_WALK_CREATE's
//                FROM_ROOT sentinel resolves at the caller's Territory ROOT
//                (syscall.c:2968, no cwd join), while SYS_OPEN's identical-looking
//                sentinel joins a relative path against the LS-4 cwd first
//                (syscall.c:2870). So the "obvious" AT_FDCWD mapping would create
//                the file in the WRONG DIRECTORY whenever cwd != "/" -- wrong for
//                a legal class of inputs with no error, which is exactly the
//                munmap failure this tier exists to refuse.
//                #50 BUILT the resolution (VIVARIUM.md section 6.24): the
//                verdict above stands against ROUTING to SYS_WALK_CREATE; the
//                shell now routes O_CREAT-without-O_PATH to
//                vivarium_openat_create_decide + SYS_OPEN_CREATE's kernel
//                core, whose cwd join is SYS_OPEN's own helper (blocker 3
//                closed structurally) and whose create-else-open composition
//                is kernel-side (blockers 1+2 dissolved the Plan 9 way).
//                O_CREAT WITH O_PATH stays on the plain decide, which STRIPS
//                it (Linux's O_PATH ignores O_CREAT) -- served exactly, not
//                declined; the in-body arm below carries the contour.
//   O_DIRECTORY  Requires the target BE a directory (Linux: ENOTDIR otherwise).
//                SYS_OPEN has no such check, so ignoring it turns an error into
//                a successful open of a regular file. The worst kind of wrong.
//                RETIRED as a reject at the getdents64 chunk (the O_NOFOLLOW
//                pattern: the blocker was real until the mechanism landed):
//                the decide now TRANSLATES it as the dir_required output and
//                the SHELL enforces the postcondition on the minted handle's
//                own qid -- the error Linux promises, produced at the layer
//                that can see the answer. The CREATE arm still declines it
//                (not in its admitted set; open(O_CREAT|O_DIRECTORY) stays
//                census-visible).
//   O_APPEND     ADMITTED AND TRANSLATED since the git chunk (VIVARIUM 6.27):
//                maps 1:1 onto SYS_WALK_OPEN_OAPPEND, which dev9p forwards to
//                the 9P Tlopen as O_APPEND so Stratum positions each write at
//                EOF server-side. The kernel gains no append MODE (its write
//                path is unchanged; the FS does the seek-to-end, the append
//                face of "the filesystem is the OS"). This closes the V-2b trap
//                the reject named ("silently corrupts a log writer"): the
//                mechanism landed, so the flag rode the chunk that added it.
//   O_EXCL, O_NONBLOCK, O_SYNC/O_DSYNC, O_DIRECT, O_NOATIME, O_TMPFILE, O_ASYNC
//                each carry semantics with no SYS_OPEN counterpart.
//                (O_NOFOLLOW moved to the ADMITTED list at DISTRO D-1 -- the
//                resolver grew symlinks, and the flag translates for real.)

enum viv_verdict vivarium_openat_decide(u64 dirfd, u64 flags,
                                        u64 *start_fd_out, u32 *omode_out,
                                        bool *cloexec_out,
                                        bool *dir_required_out) {
    // Fail toward the supervisor, never toward a dispatch (cf. vivarium_translate).
    if (!start_fd_out || !omode_out || !cloexec_out ||
        !dir_required_out)                           return VIV_FORWARD;
    bool dirreq = false;   // written to *dir_required_out only on TRANSLATED
                           // (a forwarded call leaves every output alone --
                           // the contract the domain test enforces per-output)

    // Linux passes `dirfd` and `flags` as `int`, so ONLY the low 32 bits are
    // significant. This matters concretely for AT_FDCWD: a caller may leave x0
    // sign-extended (0xFFFFFFFFFFFFFF9C) or merely zero-extended (0xFFFFFF9C),
    // and both mean -100. Comparing the raw u64 would recognise one and forward
    // the other -- i.e. work on some toolchains and not others.
    s32 dfd = (s32)(u32)dirfd;
    u32 fl  = (u32)flags;

    // AT_FDCWD only, at V-2b.
    //
    // A REAL dirfd is excluded. The V-2b note here said the blocker was that
    // Linux ignores the dirfd for an ABSOLUTE path, so deciding needs the path's
    // first byte -- and that "V-2c can revisit it with the path already
    // measured". V-2c looked, and that framing was too shallow: measuring the
    // path would not rescue the RELATIVE case, because the blocker there is
    // handle STATE, which section 4 excludes outright.
    //
    // Concretely: a Linux dirfd comes from open(dir, O_RDONLY|O_DIRECTORY) --
    // a NORMALLY-OPENED handle. "9P forbids Twalk from an OPENED fid, so a
    // normally-opened handle is NOT a valid base for ... walking ... CHILDREN;
    // an O_PATH handle IS" (syscall.h:2370). So the dirfd Linux programs
    // actually produce is not a usable SYS_OPEN start_fd; only an O_PATH one is.
    // The failure is loud (a walk error, not corruption) but it is still wrong
    // for the common legal input, and telling the two handles apart means
    // reading the handle table -- state this function is forbidden to touch.
    //
    // Left as a FORWARD rather than a hack, because the supervisor holds the
    // process's fd view anyway and is the right place to resolve one. The reach
    // lost is small: musl compiles every open() to AT_FDCWD, and only the *at()
    // family passes a real fd.
    if (dfd != VIV_AT_FDCWD) return VIV_FORWARD;

    // #50 close (SA-1): Linux's O_PATH IGNORES O_CREAT (open(2): an O_PATH
    // open honours only CLOEXEC/DIRECTORY/NOFOLLOW), so O_CREAT composed WITH
    // O_PATH is served HERE by stripping it -- the exact contour, and the
    // reason the shell's routing sends only O_CREAT-without-O_PATH to the
    // create decide. O_CREAT WITHOUT O_PATH remains out of THIS decide's
    // domain: a direct call answers FORWARD, never a silent ENOENT-shaped
    // ignore (the V-2b hazard the rejected-flag block below narrates) -- the
    // decide owns its whole domain without leaning on the shell's routing.
    if (fl & VIV_O_CREAT) {
        if (!(fl & VIV_O_PATH)) return VIV_FORWARD;
        fl &= ~(u32)VIV_O_CREAT;
    }

    // getdents64 chunk: O_DIRECTORY translates as a SHELL-ENFORCED
    // postcondition, not an ignore (ignoring it would turn Linux's ENOTDIR
    // into a successful open of a regular file -- the exact V-2b hazard).
    // The decide reports the requirement; the shell checks the minted
    // handle's own qid (QTDIR) and answers ENOTDIR on a miss. One of the
    // three flags Linux's O_PATH does NOT ignore, so it rides both arms.
    // musl opendir (open O_RDONLY|O_CLOEXEC|O_DIRECTORY) is the forcing
    // caller -- without this arm no phenotype ever reaches getdents64.
    if (fl & VIV_O_DIRECTORY) {
        dirreq = true;
        fl &= ~(u32)VIV_O_DIRECTORY;
    }

    if (fl & ~VIV_OPENAT_ADMITTED) return VIV_FORWARD;

    u32 omode;
    if (fl & VIV_O_PATH) {
        // O_PATH dominates on BOTH sides: Linux ignores the access mode (and
        // O_TRUNC) for an O_PATH open, and SYS_WALK_OPEN_OPATH "ignores the
        // access bits" (syscall.h:2373). Emitting the bare OPATH -- rather than
        // OR-ing in whatever else was set -- keeps the agreement exact instead
        // of resting on how Thylacine happens to treat OPATH|OTRUNC.
        omode = SYS_WALK_OPEN_OPATH;
    } else {
        switch (fl & VIV_O_ACCMODE) {
        case VIV_O_RDONLY: omode = VIV_OMODE_READ;  break;
        case VIV_O_WRONLY: omode = VIV_OMODE_WRITE; break;
        case VIV_O_RDWR:   omode = VIV_OMODE_RDWR;  break;
        default:
            // (flags & O_ACCMODE) == 3 is EINVAL on Linux. Forwarding rather
            // than inventing the error keeps error semantics in one place --
            // §4 forbids the table from minting new ones.
            return VIV_FORWARD;
        }
        // O_TRUNC is dropped when O_DIRECTORY is also set: a directory is never
        // truncated on Linux, and O_DIRECTORY|O_TRUNC on a REGULAR file is
        // ENOTDIR *before* any truncation. Carrying TRUNC here would truncate
        // the regular file's contents and only THEN the shell's QTDIR
        // postcondition answers ENOTDIR -- silent data loss the Linux row never
        // incurs. Clearing it makes the open non-destructive so the ENOTDIR (or
        // the kernel's own EISDIR on a write-open of a dir) fires cleanly.
        if ((fl & VIV_O_TRUNC) && !dirreq) omode |= VIV_OMODE_TRUNC;
        // O_APPEND (the git 6.27 arm) rides the same non-O_PATH arm as O_TRUNC:
        // it is a WRITE modifier (an O_PATH handle cannot write, and Linux's
        // O_PATH ignores O_APPEND), and it maps 1:1 onto SYS_WALK_OPEN_OAPPEND
        // -- dev9p forwards it to the 9P Tlopen so Stratum positions each write
        // at EOF. Dropped under O_DIRECTORY for the SAME structural reason but a
        // DIFFERENT hazard than TRUNC: a directory is opened read-only, so
        // append is simply vacuous there (no data-loss risk -- appending to a
        // dir just fails the write); the drop keeps a nonsensical flag off the
        // dir's Tlopen.
        if ((fl & VIV_O_APPEND) && !dirreq) omode |= VIV_OMODE_APPEND;
    }
    // D-1: O_NOFOLLOW rides BOTH arms -- it is one of the three flags Linux's
    // O_PATH does NOT ignore (the #151 comment below lists them), and
    // O_PATH|O_NOFOLLOW is the open-the-link-itself idiom (the returned handle
    // IS the symlink; fstat on it reports the link's own metadata). Without
    // O_PATH it is the ELOOP-on-a-final-link contour. Both are exactly what
    // SYS_WALK_OPEN_NOFOLLOW means, so the map is 1:1.
    if (fl & VIV_O_NOFOLLOW) omode |= SYS_WALK_OPEN_NOFOLLOW;

    // Belt-and-braces: never emit an omode SYS_OPEN would reject. A future
    // admission that forgets to add its bit to the kernel's mask fails HERE, as
    // a forward, rather than downstream as an unexplained -1.
    if (omode & ~SYS_WALK_OPEN_OMODE_VALID) return VIV_FORWARD;

    // AT_FDCWD <-> SYS_WALK_OPEN_FROM_ROOT is an exact correspondence, not an
    // approximation: SYS_OPEN with the sentinel joins a RELATIVE path against
    // the per-Proc cwd (LS-4) and resolves an ABSOLUTE one from the Territory
    // root -- which is precisely what AT_FDCWD specifies. The absolute/relative
    // split therefore needs no inspection here; both sides make it identically.
    *start_fd_out = SYS_WALK_OPEN_FROM_ROOT;
    *omode_out    = omode;
    // #151. Set on EVERY translated path including O_PATH: Linux honours
    // O_CLOEXEC on an O_PATH open (it is one of the three flags O_PATH does not
    // ignore, alongside O_DIRECTORY and O_NOFOLLOW), and the flag belongs to the
    // descriptor, which an O_PATH open produces like any other.
    *cloexec_out  = (fl & VIV_O_CLOEXEC) != 0;
    // getdents64 chunk: the O_DIRECTORY requirement, enforced by the SHELL as
    // a postcondition on the minted handle (the decide cannot see the object;
    // the arm above stripped the bit so the omode never carries it).
    *dir_required_out = dirreq;
    return VIV_TRANSLATED;
}

// #50 (VIVARIUM.md section 6.24): the O_CREAT domain of openat. The header
// carries the domain; this body is the map. The shell routes here when
// O_CREAT is set WITHOUT O_PATH (Linux's O_PATH ignores O_CREAT, so that
// combination stays on the plain decide -- the exact Linux contour).
enum viv_verdict vivarium_openat_create_decide(u64 dirfd, u64 flags, u64 mode,
                                               u32 *omode_out, u32 *perm_out,
                                               bool *cloexec_out) {
    // Fail toward the supervisor, never toward a dispatch.
    if (!omode_out || !perm_out || !cloexec_out) return VIV_FORWARD;

    // int-typed on the Linux side: only the low 32 bits are significant (the
    // sibling decides' narrowing, for the same sign/zero-extension reason).
    s32 dfd = (s32)(u32)dirfd;
    u32 fl  = (u32)flags;
    u32 md  = (u32)mode;

    // AT_FDCWD only -- the plain row's gate, for the plain row's reason
    // (a real dirfd is blocked by handle STATE; section 6.20 Correction 2).
    if (dfd != VIV_AT_FDCWD) return VIV_FORWARD;

    // The admitted set: the plain decide's, minus O_PATH (the shell already
    // excluded it), plus O_CREAT + O_EXCL. O_APPEND now rides through (the git
    // 6.27 arm -- a fresh reflog is O_CREAT|O_WRONLY|O_APPEND, so the create
    // path is exactly where git's first ref update lands). Everything else
    // outside the set declines to the supervisor -- O_DIRECTORY / O_TMPFILE
    // keep their V-2b rejections through this arm.
    u32 admitted = (VIV_OPENAT_ADMITTED & ~(u32)VIV_O_PATH)
                 | (u32)VIV_O_CREAT | (u32)VIV_O_EXCL;
    if (fl & ~admitted) return VIV_FORWARD;

    // mode: the low-9 permission bits only, after discarding the file-TYPE
    // field. S_IFMT is ignored on this argument by definition (the type comes
    // from the call), and callers pass it routinely -- busybox `tar` hands
    // `file_header->mode` straight through, so a regular file arrives as
    // S_IFREG|0644 and the un-masked gate below refused every extraction.
    // Masking it is exact; masking 07000 would not be, which is why a
    // setuid/sgid/sticky request still DECLINES rather than being silently
    // stripped -- that strip would record LESS restrictive metadata than the
    // caller asked for, wrong with nothing to catch it. See VIV_S_IFMT.
    md &= ~VIV_S_IFMT;
    if (md & ~0777u) return VIV_FORWARD;

    u32 omode;
    switch (fl & VIV_O_ACCMODE) {
    case VIV_O_RDONLY: omode = VIV_OMODE_READ;  break;
    case VIV_O_WRONLY: omode = VIV_OMODE_WRITE; break;
    case VIV_O_RDWR:   omode = VIV_OMODE_RDWR;  break;
    default:           return VIV_FORWARD;   // (fl & ACCMODE) == 3: Linux EINVAL
    }
    if (fl & VIV_O_TRUNC)    omode |= VIV_OMODE_TRUNC;
    // O_APPEND (the git 6.27 arm) on the create path: a fresh reflog is
    // O_CREAT|O_WRONLY|O_APPEND, so this arm is the one git's FIRST ref update
    // exercises. No dirreq gate here (O_DIRECTORY is not admitted on create),
    // so it maps unconditionally onto SYS_WALK_OPEN_OAPPEND -> the Tlcreate
    // flags -> Stratum appends server-side.
    if (fl & VIV_O_APPEND)   omode |= VIV_OMODE_APPEND;
    if (fl & VIV_O_NOFOLLOW) omode |= SYS_WALK_OPEN_NOFOLLOW;
    if (fl & VIV_O_EXCL)     omode |= SYS_WALK_OPEN_OEXCL;

    // Belt-and-braces, as the plain decide: never emit an omode the target
    // rejects -- a future admission that forgets the kernel mask fails HERE
    // as a forward, not downstream as an unexplained error.
    if (omode & ~(u32)SYS_OPEN_CREATE_OMODE_VALID) return VIV_FORWARD;

    *omode_out   = omode;
    *perm_out    = md;          // low-9 only, gated above
    *cloexec_out = (fl & VIV_O_CLOEXEC) != 0;
    return VIV_TRANSLATED;
}

// #50: mkdirat -- the exclusive arm with DMDIR (mkdir has no open-if-present).
enum viv_verdict vivarium_mkdirat_decide(u64 dirfd, u64 mode, u32 *perm_out) {
    if (!perm_out) return VIV_FORWARD;
    s32 dfd = (s32)(u32)dirfd;
    u32 md  = (u32)mode;
    if (dfd != VIV_AT_FDCWD) return VIV_FORWARD;
    // Low-9 only, after discarding the file-TYPE field -- the openat-create
    // arm's reasoning exactly (VIV_S_IFMT): S_IFMT is ignored here by
    // definition and busybox `tar` passes S_IFDIR|0755, so the un-masked gate
    // refused every `tar` directory. 07000 still declines -- but NOTE the
    // justification differs from the openat-create arm's and is weaker here:
    // Linux's own `vfs_mkdir` masks `mode & (S_IRWXUGO|S_ISVTX)`, so a setgid
    // bit passed to mkdirat is stripped BY LINUX, and setgid dirs materialise
    // from parent inheritance or a later chmod (git's `adjust_shared_perm`
    // chmods). So no caller is wronged by a strip on THIS path; the decline is
    // chosen for stance symmetry -- we refuse what we will not record, rather
    // than recording something other than what was asked -- not to protect a
    // caller. The wronged-caller argument is real on openat-create, where
    // Linux keeps 07000 via S_IALLUGO and O_CREAT|04755 really does record
    // setuid.
    md &= ~VIV_S_IFMT;
    if (md & ~0777u)         return VIV_FORWARD;
    *perm_out = md | SYS_WALK_CREATE_DMDIR;
    return VIV_TRANSLATED;
}

// #50: unlinkat -- flags map 1:1 onto SYS_UNLINK's (0 <-> file,
// AT_REMOVEDIR <-> SYS_UNLINK_REMOVEDIR); anything else declines.
enum viv_verdict vivarium_unlinkat_decide(u64 dirfd, u64 flags,
                                          u32 *tflags_out) {
    if (!tflags_out) return VIV_FORWARD;
    s32 dfd = (s32)(u32)dirfd;
    u32 fl  = (u32)flags;
    if (dfd != VIV_AT_FDCWD)               return VIV_FORWARD;
    if (fl & ~(u32)VIV_AT_REMOVEDIR)       return VIV_FORWARD;
    *tflags_out = (fl & VIV_AT_REMOVEDIR) ? SYS_UNLINK_REMOVEDIR : 0u;
    return VIV_TRANSLATED;
}

// #50: renameat/renameat2 -- the domain gate alone (the map is 1:1: Linux's
// replace-existing atomicity IS SYS_RENAME's documented contract). renameat
// passes literal 0 for flags; renameat2 admits exactly 0 (NOREPLACE /
// EXCHANGE / WHITEOUT decline, census-visible).
enum viv_verdict vivarium_renameat_decide(u64 olddirfd, u64 newdirfd,
                                          u64 flags) {
    s32 odfd = (s32)(u32)olddirfd;
    s32 ndfd = (s32)(u32)newdirfd;
    if (odfd != VIV_AT_FDCWD || ndfd != VIV_AT_FDCWD) return VIV_FORWARD;
    if ((u32)flags != 0)                              return VIV_FORWARD;
    return VIV_TRANSLATED;
}

// #151. The header carries the served set and why. This body is a classifier;
// the only judgement inside it is the SETFD mask, called out below.
enum viv_verdict vivarium_pipe2_decide(u64 flags, bool *cloexec_out) {
    if (!cloexec_out) return VIV_FORWARD;
    *cloexec_out = false;

    // Linux passes `flags` as an int, so only the low 32 bits are significant --
    // the narrowing every sibling decide performs, for the same reason (a caller
    // may leave the register sign- or zero-extended).
    u32 fl = (u32)flags;

    // THE ALLOW-LIST. Written as "nothing outside the admitted set" rather than
    // as a list of rejects, so a flag nobody here has heard of cannot be served
    // by having been forgotten. O_DIRECT and O_NONBLOCK are the two Linux pipe2
    // flags this deliberately excludes, and each is excluded because devpipe has
    // no counterpart to it -- not because they are unusual.
    if ((fl & ~(u32)VIV_O_CLOEXEC) != 0) return VIV_FORWARD;

    *cloexec_out = (fl & (u32)VIV_O_CLOEXEC) != 0;
    return VIV_TRANSLATED;
}

enum viv_verdict vivarium_dup3_decide(u64 flags, bool *cloexec_out) {
    if (!cloexec_out) return VIV_FORWARD;
    *cloexec_out = false;

    // Same narrowing as every sibling, and it MATTERS more here than usual:
    // musl's __dup3 emits `sxtw x2, w2`, so a negative flags word arrives with
    // the whole high half set. Linux reads `int flags` -- the low 32 bits -- and
    // so must this.
    u32 fl = (u32)flags;

    // THE ALLOW-LIST, which for this row is EQUAL to Linux's accepted set
    // rather than a subset of it: ksys_dup3 rejects `flags & ~O_CLOEXEC` with
    // EINVAL and admits nothing else. So an out-of-domain word is not something
    // we decline to serve -- it is something we serve by refusing, exactly as
    // Linux refuses it. The SHELL turns this VIV_FORWARD into EINVAL for that
    // reason; it must not be collapsed into the ENOSYS decline (V-2d's munmap
    // note: a specific errno the guest can act on must not be replaced by
    // "this surface is absent", which here would be a lie).
    if ((fl & ~(u32)VIV_O_CLOEXEC) != 0) return VIV_FORWARD;

    *cloexec_out = (fl & (u32)VIV_O_CLOEXEC) != 0;
    return VIV_TRANSLATED;
}

enum viv_verdict vivarium_fcntl_decide(u64 cmd, u64 arg,
                                       enum viv_fcntl_op *op_out,
                                       bool *cloexec_out, u64 *min_fd_out) {
    if (!op_out || !cloexec_out || !min_fd_out) return VIV_FORWARD;

    *op_out      = VIV_FCNTL_UNSERVED;
    *cloexec_out = false;
    *min_fd_out  = 0;

    // Linux passes `cmd` as an int, so only the low 32 bits are significant --
    // the same narrowing vivarium_openat_decide does to `dirfd`, for the same
    // reason (a caller may leave the register sign- or zero-extended).
    switch ((u32)cmd) {
    case VIV_F_GETFD:
        *op_out = VIV_FCNTL_GETFD;
        return VIV_TRANSLATED;

    case VIV_F_SETFD:
        // MASK, do not validate. Linux's F_SETFD is `set_close_on_exec(fd, arg &
        // FD_CLOEXEC)` -- it ignores every other bit rather than rejecting the
        // call, and FD_CLOEXEC is the only flag the descriptor-flags word has
        // ever defined. Rejecting a stray bit would be STRICTER than Linux for a
        // guest that legally passes one, which is its own mistranslation.
        *op_out      = VIV_FCNTL_SETFD;
        *cloexec_out = (arg & VIV_FD_CLOEXEC) != 0;
        return VIV_TRANSLATED;

    case VIV_F_DUPFD:
        *op_out     = VIV_FCNTL_DUPFD;
        *min_fd_out = arg;
        return VIV_TRANSLATED;

    case VIV_F_DUPFD_CLOEXEC:
        *op_out      = VIV_FCNTL_DUPFD;
        *cloexec_out = true;
        *min_fd_out  = arg;
        return VIV_TRANSLATED;

    case VIV_F_GETFL:
        // Read the open-file status flags. The shell composes the access mode
        // (from the fd's Spoor) with O_NONBLOCK (the CNONBLOCK flag); no payload
        // is decoded here.
        *op_out = VIV_FCNTL_GETFL;
        return VIV_TRANSLATED;

    case VIV_F_SETFL:
        // Set the open-file status flags. Linux F_SETFL honors only
        // O_NONBLOCK / O_APPEND / O_ASYNC / O_DIRECT and silently IGNORES the
        // rest (including the access mode), so the classic
        // `F_GETFL -> F_SETFL(flags | O_NONBLOCK)` round-trip works even though
        // we serve only O_NONBLOCK. The shell reads `arg & O_NONBLOCK` itself
        // (it has the arg), so nothing is decoded here -- unlike SETFD, which
        // needs the decider only because the shell lacks a generic arg path.
        *op_out = VIV_FCNTL_SETFL;
        return VIV_TRANSLATED;

    default:
        // The locking family (F_GETLK / F_SETLK / ...) + everything else. See
        // the header on why this is ENOSYS at the shell, not Linux's EINVAL.
        return VIV_FORWARD;
    }
}

// C2-k1 (interactive git): classify a terminal ioctl request. PURE -- request
// code only. The shell owns the fd's tty-ness and the user-struct copy.
enum viv_verdict vivarium_ioctl_decide(u64 request, enum viv_ioctl_op *op_out) {
    if (!op_out) return VIV_FORWARD;
    *op_out = VIV_IOCTL_UNSERVED;

    // Linux passes the ioctl request as an int (32-bit) -- narrow like fcntl,
    // so a sign- or zero-extended register still classifies.
    switch ((u32)request) {
    case VIV_TCGETS:
        *op_out = VIV_IOCTL_TCGETS;
        return VIV_TRANSLATED;

    case VIV_TCSETS:
    case VIV_TCSETSW:
    case VIV_TCSETSF:
        // TCSANOW / TCSADRAIN / TCSAFLUSH all apply the mode. The cons/pts line
        // discipline has no separate termios drain/flush stage, so the three are
        // indistinguishable here -- one op (documented divergence).
        *op_out = VIV_IOCTL_TCSETS;
        return VIV_TRANSLATED;

    case VIV_TIOCGWINSZ:
        *op_out = VIV_IOCTL_TIOCGWINSZ;
        return VIV_TRANSLATED;

    case VIV_TIOCSWINSZ:
        *op_out = VIV_IOCTL_TIOCSWINSZ;
        return VIV_TRANSLATED;

    default:
        // Not a terminal request this surface serves. The shell (C2-k1b) turns
        // this into the terminal errno -- a real tty answers ENOTTY for an
        // unknown request on Linux; the T_E_NOTTY ABI decision owns that. The
        // decode only reports "not one of ours".
        return VIV_FORWARD;
    }
}

void vivarium_openat_build(u64 start_fd, u64 path_va, u32 path_len, u32 omode,
                           struct viv_call *out) {
    if (!out) return;

    for (u32 i = 0; i < VIV_NARGS; i++) out->args[i] = 0;

    out->nr      = (u64)SYS_OPEN;
    out->args[0] = start_fd;
    out->args[1] = path_va;
    out->args[2] = (u64)path_len;
    out->args[3] = (u64)omode;
}

void vivarium_stat_to_linux(const struct t_stat *in, struct viv_linux_stat *out) {
    if (!in || !out) return;

    // Zero FIRST, then fill. The kernel links no `memset`, so this is the tree's
    // byte-loop idiom (dev9p.c:701 `t_stat_from_p9_attr` does the same for the
    // same reason). Zeroing wholesale rather than assigning every field
    // individually is an I-13 obligation: this buffer is copied to a guest, so
    // any word left unwritten -- a reserved field today, a field added
    // tomorrow -- would ship a slice of the kernel stack.
    for (u64 i = 0; i < (u64)sizeof(*out); i++) ((u8 *)out)[i] = 0;

    // st_dev / st_ino: the (devno, qid.path) pair IS Thylacine's file identity
    // (#100), and it is already the pair userspace maps onto (st_dev, st_ino) --
    // pouch patch 0010 does exactly this, and gopls's robustio keys FileID on it.
    // So this is not a new correspondence invented here; it is the established
    // one, applied one layer lower.
    out->st_dev = (u64)in->devno;
    out->st_ino = in->qid_path;

    out->st_mode  = in->mode;
    out->st_nlink = in->nlink;
    out->st_uid   = in->uid;
    out->st_gid   = in->gid;

    out->st_size    = (s64)in->size;
    out->st_blksize = (s32)in->blksize;
    out->st_blocks  = (s64)in->blocks;

    // t_stat carries whole seconds only, so the nsec words stay 0 -- an honest
    // "unknown sub-second", the same answer the native surface gives. st_rdev
    // stays 0: Thylacine has no dev_t for its character objects, and fabricating
    // one would invite a guest to switch on it.
    out->st_atime_sec = (s64)in->atime_sec;
    out->st_mtime_sec = (s64)in->mtime_sec;
    out->st_ctime_sec = (s64)in->ctime_sec;
}

// The `newfstatat` flags SYS_STAT can honour EXACTLY. Same admission rule as
// openat's: a flag may be ignored ONLY when it asks for behaviour we already
// provide unconditionally.
//
//   AT_NO_AUTOMOUNT  ADMITTED AS A NO-OP. It asks "do not trigger an automount
//                    while resolving". A Thylacine namespace is composed
//                    EXPLICITLY -- SYS_MOUNT / bind, per-Territory -- and
//                    nothing mounts as a side effect of traversal. That is a
//                    property of the Plan 9 namespace model, not a v1.0 gap:
//                    there is no automount to defer, ever. (Nothing known sets
//                    it -- glibc and Go both pass 0 -- so this is admitted for
//                    correctness, not reach. The V-2b precedent is O_LARGEFILE,
//                    equally unset by musl-aarch64 and equally correct.)
//
//   AT_SYMLINK_NOFOLLOW  ADMITTED AND TRANSLATED since DISTRO D-1. The V-2c
//                    reject that stood here said it plainly: "the day symlinks
//                    land there is nothing in this file, or in a build, that
//                    would fail" -- so it forwarded every lstat() rather than
//                    silently reporting the TARGET instead of the link. The day
//                    arrived, and the flag rode the same chunk that landed the
//                    feature: it now maps onto the resolver's STALK_NOFOLLOW
//                    (via sys_stat_for_proc's stalk_flags), so lstat() reports
//                    the LINK's own record -- S_IFLNK mode, target-length size
//                    -- exactly the Linux answer. The reject's reasoning is
//                    preserved above as the model for every flag whose
//                    correctness depends on a feature being absent.
//
// And the rejects, each because ignoring it would be a silent wrong answer:
//
//   AT_EMPTY_PATH    An empty path means "operate on dirfd itself" (with
//                    AT_FDCWD: the cwd). SYS_STAT requires path_len >= 1, and
//                    serving it would mean SYNTHESISING a "." argument the
//                    caller never passed. Translating is mapping what you were
//                    given, not inventing what you were not.
//   AT_REMOVEDIR / AT_SYMLINK_FOLLOW
//                    Not valid on fstatat at all (they belong to unlinkat /
//                    linkat); Linux answers EINVAL. Forwarded rather than
//                    rejected here, on the same ground as openat's
//                    (flags & O_ACCMODE) == 3: minting errors is not this
//                    table's job.
#define VIV_FSTATAT_ADMITTED \
    ((u32)(VIV_AT_NO_AUTOMOUNT | VIV_AT_SYMLINK_NOFOLLOW))

enum viv_verdict vivarium_fstatat_decide(u64 dirfd, u64 flags,
                                         u32 *stalk_flags_out) {
    // Fail toward the decline, never toward a dispatch (the openat shape).
    if (!stalk_flags_out) return VIV_FORWARD;
    *stalk_flags_out = 0;

    // Both are `int` in the Linux ABI, so only the low 32 bits are significant;
    // `dirfd` is compared signed for the AT_FDCWD sign-extension reason spelled
    // out in vivarium_openat_decide.
    s32 dfd = (s32)(u32)dirfd;
    u32 fl  = (u32)flags;

    // AT_FDCWD only -- here not as a v1.0 restriction but because SYS_STAT has
    // no base argument to carry anything else. See vivarium_fstatat_decide's
    // header comment.
    if (dfd != VIV_AT_FDCWD)        return VIV_FORWARD;
    if (fl & ~VIV_FSTATAT_ADMITTED) return VIV_FORWARD;

    // D-1: lstat. STALK_NOFOLLOW's value is deliberately not leaked into this
    // pure table -- the shell passes the out-param to sys_stat_for_proc, which
    // owns the resolver vocabulary.
    if (fl & VIV_AT_SYMLINK_NOFOLLOW) *stalk_flags_out = 1;

    return VIV_TRANSLATED;
}

// faccessat -- the whole decision is the AT_FDCWD gate. See the header comment
// for why this single check is both the cwd-form contract and the native-48
// collision defense; the mode's EINVAL contour and the stat + perm_check are in
// the shell (the mmap-judges-len precedent), so this stays pure and memoryless.
enum viv_verdict vivarium_faccessat_decide(u64 dirfd) {
    // Signed, for the AT_FDCWD sign-extension reason vivarium_openat_decide
    // spells out: an `int` -100 arrives sign-extended and must compare equal.
    return ((s32)(u32)dirfd == VIV_AT_FDCWD) ? VIV_TRANSLATED : VIV_FORWARD;
}

// =============================================================================
// TIER 2 — mmap (V-2d). See <thylacine/vivarium.h> and VIVARIUM.md §6.21.
// =============================================================================

// The `prot` bits SYS_BURROW_ATTACH_LAZY's fixed RW/XN mapping can stand in for.
//
// An ALLOW-LIST of two bits, not "everything except PROT_EXEC", and the
// difference is load-bearing: aarch64 musl also defines PROT_BTI and PROT_MTE,
// and generic musl PROT_GROWSDOWN/PROT_GROWSUP. None of those is honourable
// either, and a deny-list would have admitted all four silently.
//
// PROT_NONE (== 0) is INSIDE the list, and is the one deliberate fidelity
// degradation: it yields a writable mapping. That is argued in full in the
// header -- musl's own ENOSYS-tolerant mprotect is the evidence it is the
// sanctioned outcome -- and published in VIVARIUM.md §9's DEGRADED tier.
#define VIV_MMAP_PROT_ADMITTED ((u32)(VIV_PROT_READ | VIV_PROT_WRITE))

enum viv_verdict vivarium_mmap_decide(u64 addr, u64 prot, u64 flags,
                                      u64 fd, u64 offset) {
    // `addr` is IGNORED, not merely unchecked. Without MAP_FIXED, Linux
    // specifies it as a hint the kernel may disregard, and the caller reads the
    // real address out of the return value -- so ignoring it is conforming.
    // MAP_FIXED and MAP_FIXED_NOREPLACE, where the address becomes a
    // REQUIREMENT, are outside VIV_MMAP_FLAGS_ADMITTED and so decline below.
    (void)addr;

    // Linux passes `prot`, `flags` and `fd` as `int`: only the low 32 bits are
    // significant, exactly as in vivarium_openat_decide.
    u32 pr = (u32)prot;
    u32 fl = (u32)flags;

    // PROT_EXEC is refused rather than degraded. An executable anonymous
    // mapping is CAP_JIT / I-42 territory (JIT-ON-WX-DESIGN.md), and W^X (I-12)
    // forbids the RW-and-X region the naive translation would produce. It falls
    // out of the allow-list below, but it is the one bit worth naming: a future
    // widening that reaches for "all prots" must not take it with them.
    if (pr & ~VIV_MMAP_PROT_ADMITTED) return VIV_FORWARD;

    // EXACT equality, not a mask test: a flag we have not reasoned about must
    // decline rather than ride along.
    if (fl != VIV_MMAP_FLAGS_ADMITTED) return VIV_FORWARD;

    // Anonymous. Linux ignores `fd` under MAP_ANONYMOUS, so requiring -1 is
    // stricter than the letter -- and is what musl and glibc both emit.
    if ((s32)(u32)fd != -1) return VIV_FORWARD;
    if (offset != 0)        return VIV_FORWARD;

    // NOTE the absence of a length check. `len` is a SEMANTIC question, not a
    // domain one: Linux answers EINVAL for 0 and ENOMEM for too-large, and the
    // shell reproduces both exactly. Forwarding on length here would answer
    // ENOSYS for a call Linux gives a specific errno.
    return VIV_TRANSLATED;
}

// -----------------------------------------------------------------------------
// TIER 2 — the FILE mmap arm (DISTRO D-3). Domain measured off musl's
// map_library (dynlink.c:809), not derived from Linux. See the header.
// -----------------------------------------------------------------------------

enum viv_verdict vivarium_mmap_file_decide(u64 prot, u64 flags,
                                           u64 fd, u64 offset) {
    // Same 32-bit narrowing as the anon arm, and for the same reason: Linux
    // declares prot/flags/fd as `int`, so the narrowing IS the ABI.
    u32 pr = (u32)prot;
    u32 fl = (u32)flags;

    // PROT_WRITE is the one refusal worth naming, because it is the refusal
    // that keeps I-36 intact. Everything this arm maps rides a shared
    // BURROW_TYPE_FILE Burrow demand-paged from the file, and that Burrow has
    // no write-back path -- so a writable file mapping here would either lose
    // the guest's writes or leak them into a file (and into every OTHER Proc
    // sharing the Image). Refusing keeps "no userspace writable file mapping
    // exists" true on this path by construction rather than by care.
    //
    // An ALLOW-LIST for the same reason the anon arm uses one: PROT_BTI /
    // PROT_MTE / PROT_GROWSDOWN are all outside it without being enumerated.
    if (pr & ~VIV_MMAP_FILE_PROT_ADMITTED) return VIV_FORWARD;

    // PROT_NONE (== 0) DECLINES here, and that is a real difference from the
    // anon arm, which admits it as a documented degradation to writable. There
    // is no analogous degradation available: a PROT_NONE file map is a pure
    // address-space RESERVATION whose bytes must fault, and serving it as a
    // readable file map would hand the guest bytes it asked not to be given.
    // Declining costs nothing measured -- map_library's whole-span call always
    // carries the lowest PT_LOAD's prot, which always includes PF_R.
    if ((pr & VIV_PROT_READ) == 0)         return VIV_FORWARD;

    // EXACT equality: MAP_PRIVATE alone. This is what excludes MAP_FIXED,
    // MAP_FIXED_NOREPLACE and MAP_SHARED without naming them -- and excluding
    // MAP_SHARED matters most, since a shared file mapping is the write-back
    // semantics the prot gate above just refused, arriving by another door.
    if (fl != VIV_MMAP_FILE_FLAGS_ADMITTED) return VIV_FORWARD;

    // A real descriptor. The (s32) cast is what makes -1 (the anon sentinel)
    // land below zero rather than at 4294967295.
    if ((s32)(u32)fd < 0)                   return VIV_FORWARD;

    // Page-aligned offset. Not fidelity (Linux says EINVAL) but STRUCTURE: the
    // FILE fault arm reads each page at `burrow->file_offset + page-floored
    // burrow offset`, which is the mapping's own bytes only when the Burrow's
    // offset 0 is the mapping's start. See the header.
    if (offset & (u64)(PAGE_SIZE - 1))      return VIV_FORWARD;

    return VIV_TRANSLATED;
}

// -----------------------------------------------------------------------------
// TIER 2 — the two MAP_FIXED arms (DISTRO D-3b). Domains measured off
// map_library's overlay calls (dynlink.c:842 and :851). See the header.
// -----------------------------------------------------------------------------

// Shared by both fixed arms: with MAP_FIXED the address is a REQUIREMENT, not
// the hint it is on the non-fixed arms, so it has to be a real page. Zero is
// refused separately from misalignment because a fixed map at NULL is a distinct
// mistake -- it would put a mapping where a null-pointer dereference must fault.
static bool fixed_addr_ok(u64 addr) {
    if (addr == 0)                    return false;
    if (addr & (u64)(PAGE_SIZE - 1))  return false;
    return true;
}

enum viv_verdict vivarium_mmap_fixed_file_decide(u64 addr, u64 prot, u64 flags,
                                                 u64 fd, u64 offset) {
    u32 pr = (u32)prot;
    u32 fl = (u32)flags;

    if (!fixed_addr_ok(addr))                    return VIV_FORWARD;

    // Unlike the D-3a arm, PROT_WRITE IS admitted here -- served by an eager
    // private copy, never by a writable file mapping (see the header). What is
    // NOT admitted is W together with X: vma_alloc would reject it anyway, but
    // I-12 is worth failing closed on at the domain edge too, so a W+X request
    // is declined before it can reach anything that has to argue about it.
    if (pr & ~VIV_MMAP_FIXED_FILE_PROT_ADMITTED) return VIV_FORWARD;
    if ((pr & VIV_PROT_WRITE) && (pr & VIV_PROT_EXEC)) return VIV_FORWARD;
    if ((pr & VIV_PROT_READ) == 0)               return VIV_FORWARD;

    if (fl != VIV_MMAP_FIXED_FILE_FLAGS_ADMITTED) return VIV_FORWARD;
    if ((s32)(u32)fd < 0)                         return VIV_FORWARD;

    // Page-aligned offset, for the same STRUCTURAL reason as the D-3a arm: the
    // FILE fault arm derives each page's file position from the Burrow's own
    // offset 0, which is the mapping's start only when the offset is aligned.
    // The eager-copy path does not need it (it reads at an explicit offset), but
    // requiring it uniformly keeps ONE domain for the arm rather than a domain
    // that silently depends on which backing the prot happens to select.
    if (offset & (u64)(PAGE_SIZE - 1))            return VIV_FORWARD;

    return VIV_TRANSLATED;
}

enum viv_verdict vivarium_mmap_fixed_anon_decide(u64 addr, u64 prot, u64 flags,
                                                 u64 fd, u64 offset) {
    u32 pr = (u32)prot;
    u32 fl = (u32)flags;

    if (!fixed_addr_ok(addr))                    return VIV_FORWARD;

    if (pr & ~VIV_MMAP_FIXED_ANON_PROT_ADMITTED) return VIV_FORWARD;
    // PROT_NONE declines rather than degrading to writable, which is the
    // non-fixed anon arm's documented behaviour. A FIXED PROT_NONE over an
    // existing mapping is a GUARD; handing back a writable page instead would
    // not be a degradation but a hole. See the header.
    if ((pr & VIV_PROT_READ) == 0)               return VIV_FORWARD;

    if (fl != VIV_MMAP_FIXED_ANON_FLAGS_ADMITTED) return VIV_FORWARD;
    if ((s32)(u32)fd != -1)                       return VIV_FORWARD;
    if (offset != 0)                              return VIV_FORWARD;

    return VIV_TRANSLATED;
}

bool vivarium_mmap_arms_disjoint(u64 addr, u64 prot, u64 flags,
                                 u64 fd, u64 offset) {
    int admitted = 0;
    if (vivarium_mmap_decide(addr, prot, flags, fd, offset) == VIV_TRANSLATED)
        admitted++;
    if (vivarium_mmap_file_decide(prot, flags, fd, offset) == VIV_TRANSLATED)
        admitted++;
    if (vivarium_mmap_fixed_file_decide(addr, prot, flags, fd, offset) == VIV_TRANSLATED)
        admitted++;
    if (vivarium_mmap_fixed_anon_decide(addr, prot, flags, fd, offset) == VIV_TRANSLATED)
        admitted++;
    return admitted <= 1;
}

// =============================================================================
// PROCESS CREATION — the pure layer (LINEAGE L-3d). See docs/LINEAGE.md §5.3.
// =============================================================================

enum viv_verdict vivarium_clone_decide(u64 flags, u64 stack,
                                       bool *share_mem_out) {
    if (!share_mem_out) return VIV_FORWARD;     // fail closed
    *share_mem_out = false;
    // THE COMPARISON IS FULL-WIDTH, and that is a deliberate difference from
    // vivarium_mmap_decide / vivarium_openat_decide, which narrow to 32 bits.
    // Those calls take `int` parameters in the Linux ABI, so the narrowing is
    // the ABI. `clone` takes an `unsigned long`, so it is not -- and whether
    // Linux's legacy clone entry ignores the high half is a fact about ITS
    // source, which is not in this tree and therefore not something to assert
    // from memory.
    //
    // Under that uncertainty the stricter reading is the correct one, because
    // this file's own rule is that DECLINING IS ALWAYS SAFE while admitting a
    // bit we have not reasoned about is the failure mode the tier exists to
    // prevent. And it costs nothing measurable: musl's clone.s emits
    // `uxtw x0,w2` (verified in the built object: `ubfx x0, x2, #0, #32`), so
    // the high half is ALWAYS ZERO from the real consumer. The only caller this
    // can turn away is a hand-rolled one setting a bit nobody has considered,
    // which is exactly who should be turned away.
    //
    // Note also that the low BYTE is the exit signal, not a flag, which is why
    // the admitted word carries SIGCHLD as a VALUE rather than as a mask member.

    // EXACT equality. Three separate things are being refused here and each
    // matters on its own, so a mask test would quietly admit all of them:
    //
    //   * CLONE_VM without CLONE_VFORK -- the caller said "do not suspend me",
    //     and serving it anyway turns a working program into a deadlock. The
    //     header carries the full argument for why L-3c-2's fail-safe reasoning
    //     does not extend here.
    //   * CLONE_THREAD (and the rest of the thread set) -- a genuinely
    //     concurrent child has a correct target already, SYS_THREAD_SPAWN, and
    //     it is not this row.
    //   * CLONE_SETTLS / CLONE_PARENT_SETTID / CLONE_CHILD_{SET,CLEAR}TID --
    //     each makes one of x2/x3/x4 MEANINGFUL, and this translator's whole
    //     safety argument is that it never reads them (the header's garbage-
    //     register note). Excluding them is what makes "pass a literal 0 for
    //     child_tls" correct rather than merely convenient.
    //
    // An exit signal other than SIGCHLD is refused by the same equality, and
    // deliberately: `exits()` posts `child_exit` unconditionally (I-19), so a
    // caller asking for a different signal -- or for none, which is what a
    // detached child asks for by leaving the low byte 0 -- would get one it did
    // not request.
    // THE FORK SHAPE (LINEAGE L-6a): `clone(SIGCHLD, 0)`, what musl's fork()
    // emits. Admitted here and NOT at L-3d, where the reason for refusing it
    // was that a private copy-on-write address space did not exist. L-4 and
    // L-5 built one, so that reason is discharged -- and it is worth naming
    // that this row's own probe leg carried the stale reason in a comment
    // while still passing, which is why the expiry had to be looked for rather
    // than waited for.
    //
    // The `stack` rule INVERTS between the two shapes, and that is the kernel's
    // contract rather than a choice made here: under RFMEM a zero child_sp is
    // refused (two Procs pushing on one stack corrupt each other), while under
    // RFPROC alone zero is the NORMAL value and means INHERIT -- each Proc has
    // its own copy at that address, which is exactly what fork() means. So the
    // vfork arm rejects zero below and this one does not.
    if (flags == (u64)VIV_CLONE_FLAGS_FORK) {
        *share_mem_out = false;
        return VIV_TRANSLATED;
    }

    if (flags != (u64)VIV_CLONE_FLAGS_ADMITTED) return VIV_FORWARD;

    // The vfork shape splits on `stack`, and the split is option B (the
    // 2026-08-31 vote; lineage-aligned per LINEAGE.md 3.1).
    //
    //   stack != 0 -- posix_spawn's shape. The child runs on its OWN stack
    //   while sharing the parent's memory, so it is a true RFMEM vfork: the
    //   parent suspends (vfork_await_release), the child execs/exits on its own
    //   stack, and the two never push a shared frame concurrently.
    //   share_mem = true.
    //
    //   stack == 0 -- `vfork()` proper: "run on the parent's ONE stack, and
    //   trust me not to disturb it before exec/exit." Plan 9 has NO such shape.
    //   rfork always gives the child its own stack -- it has no stack argument
    //   precisely because two Procs never share one (LINEAGE.md 3.1) -- and our
    //   RFMEM child_sp rule is that invariant wearing a Linux parameter.
    //   Weakening it to serve this (option A) was rejected as anti-lineage.
    //   Instead serve it as a FORK: a private copy-on-write child. POSIX makes
    //   anything but _exit/exec after vfork UNDEFINED, so a copy is a
    //   conformant implementation (the classic `#define vfork fork`), and the
    //   only observable differences -- the parent not suspending, the child's
    //   pre-exec writes not reaching the parent -- are exactly what POSIX
    //   leaves undefined. The alternative is an ENOSYS the caller has no
    //   fallback for: busybox's spawn aborts with "vfork: Function not
    //   implemented" and no tar/gzip pipeline runs.
    //
    //   This arm is provably equivalent to the fork arm above: share_mem=false
    //   + stack=0 reaches the shell as sys_rfork_core(RFPROC, 0, 0), the exact
    //   translation `clone(SIGCHLD, 0)` already produces. It is not a new path,
    //   it is the same path reached by a different flags word.
    if (stack == 0) {
        *share_mem_out = false;
        return VIV_TRANSLATED;
    }
    *share_mem_out = true;

    // NOTE what is NOT checked: alignment, range, and overlap with the caller's
    // own stack. Those are SYS_RFORK's own gate, they are identical for a
    // native and a translated caller, and re-stating them here would be a
    // second copy of a rule that must not drift. The shell calls the same core
    // the native handler does, which is the V-8 `sys_fstat_for_proc` discipline
    // -- one implementation, reached two ways.
    return VIV_TRANSLATED;
}

// =============================================================================
// SIGNALS — the pure layer (V-6). See VIVARIUM.md §6.22.
// =============================================================================

enum viv_signote viv_signal_note(u64 signum) {
    // Every row is a note that ALREADY EXISTS in notes.h. That is the whole
    // reason Tier 0 is a decode rather than new machinery -- and it is also the
    // reason the DEFAULT dispositions are already correct without any code
    // here: `interrupt` already default-terminates (LS-5), `kill` is already
    // non-catchable (I-19 N-4), the `tty:*` family already carries PTY-1
    // semantics.
    switch (signum) {
    // V-6b BROKE THE SIGINT/SIGTERM COLLAPSE. V-6a inherited the pouch mapping
    // (POUCH-DESIGN §6.4) where both land on `interrupt`, and called it "a
    // stated imprecision, not an oversight" on the grounds that both
    // default-terminate. Building dispositions showed the collapse is not
    // imprecise, it is UNREPRESENTABLE: `sigaction(SIGINT, SIG_IGN)` with
    // SIGTERM at SIG_DFL has no answer on a shared note -- honour it and
    // SIGTERM goes silent too, refuse it and a Proc that asked to ignore Ctrl-C
    // dies on Ctrl-C. And that particular call is the one a shell makes.
    //
    // So `interrupt` belongs to SIGINT alone -- it IS the Ctrl-C note (LS-5;
    // the console path posts it by that name) -- and SIGTERM declines until it
    // has a note of its own. NOTHING REGRESSES: no v1.0 path posts SIGTERM (no
    // `kill` row exists yet), so its entry here was reachable only through the
    // mask conversion. A guest asking about SIGTERM got ENOSYS before this
    // change and gets ENOSYS after it.
    //
    // THE SEAM: a real SIGTERM wants its own supported note name (`terminate`)
    // and its own NOTE_BIT -- an addition to I-19's closed supported set, which
    // is ABI-bearing and needs signoff (the tasks #15/#16 shape). It becomes
    // load-bearing when `kill` lands, because THAT is what would first generate
    // a SIGTERM.
    case VIV_SIGINT:                      return VIV_SIGNOTE_INTERRUPT;
    case VIV_SIGKILL:                     return VIV_SIGNOTE_KILL;
    case VIV_SIGPIPE:                     return VIV_SIGNOTE_PIPE;
    case VIV_SIGCHLD:                     return VIV_SIGNOTE_CHILD_EXIT;
    case VIV_SIGSEGV:                     return VIV_SIGNOTE_SNARE_SEGV;
    case VIV_SIGBUS:                      return VIV_SIGNOTE_SNARE_BUS;
    case VIV_SIGILL:                      return VIV_SIGNOTE_SNARE_ILL;
    case VIV_SIGFPE:                      return VIV_SIGNOTE_SNARE_FPE;
    case VIV_SIGHUP:                      return VIV_SIGNOTE_TTY_HUP;
    case VIV_SIGQUIT:                     return VIV_SIGNOTE_TTY_QUIT;
    case VIV_SIGWINCH:                    return VIV_SIGNOTE_TTY_WINCH;
    case VIV_SIGTSTP:                     return VIV_SIGNOTE_TTY_SUSP;
    case VIV_SIGCONT:                     return VIV_SIGNOTE_TTY_CONT;

    // NOT a gap, and worth naming so a future reader does not "fix" it by
    // inventing a delivery:
    //   SIGTERM  — see above: it wants its own note, and inventing a shared one
    //              is what V-6b just undid.
    //   SIGALRM  — no timer note exists; setitimer/alarm are themselves ENOSYS,
    //              so nothing could post it.
    //   SIGUSR1/2 — no general-purpose note; a note has a fixed 16-byte name
    //              from a closed set, so there is nothing to carry them.
    //   SIGABRT  — reachable only via raise() == tkill(self), which terminates.
    //   SIGSTOP  — uncatchable by POSIX and rejected by the sigaction domain.
    //   32..64   — the realtime range, which requires queued siginfo (Tier 2).
    default: return VIV_SIGNOTE_NONE;
    }
}

bool viv_signal_owns_note_exclusively(u64 signum) {
    enum viv_signote mine = viv_signal_note(signum);
    if (mine == VIV_SIGNOTE_NONE) return false;

    // SCAN rather than list. If a future row adds a second signal to a note,
    // this narrows the domain by itself -- no separate table to keep in step,
    // and no chance of the map and the exclusivity claim disagreeing.
    for (u64 other = 1; other <= VIV_NSIG; other++) {
        if (other == signum) continue;
        if (viv_signal_note(other) == mine) return false;
    }
    return true;
}

bool viv_signote_is_deliverable(enum viv_signote note) {
    switch (note) {
    // In `g_known_notes` (notes.c), so `notes_post` accepts them and the
    // EL0-return-tail dispatcher can see them.
    case VIV_SIGNOTE_INTERRUPT:
    case VIV_SIGNOTE_KILL:
    case VIV_SIGNOTE_PIPE:
    case VIV_SIGNOTE_CHILD_EXIT:
    case VIV_SIGNOTE_TTY_HUP:
    case VIV_SIGNOTE_TTY_QUIT:
    case VIV_SIGNOTE_TTY_WINCH:
    case VIV_SIGNOTE_TTY_SUSP:
    case VIV_SIGNOTE_TTY_CONT:
        return true;

    // NOT in g_known_notes, and `proc_fault_terminate` never calls notes_post
    // -- an EL0 fault runs `exits(name)` straight away. There is no queue entry
    // to catch, ignore or mask, so the only truthful disposition is SIG_DFL.
    case VIV_SIGNOTE_SNARE_SEGV:
    case VIV_SIGNOTE_SNARE_BUS:
    case VIV_SIGNOTE_SNARE_ILL:
    case VIV_SIGNOTE_SNARE_FPE:
    case VIV_SIGNOTE_NONE:
    default:
        return false;
    }
}

// =============================================================================
// SOCKETS (V-5) -- the pure half. The header carries the design; these are the
// decisions and the string work, kept out of the shells so they unit-test with
// synthetic input and no Proc.
// =============================================================================

// Linux socket constants, taken from third_party/musl (not from memory):
//   AF_INET / AF_INET6      include/netinet/in.h  (2 / 10)
//   SOCK_STREAM / SOCK_DGRAM  arch/aarch64/bits/socket.h via include/sys/socket.h
//   SOCK_NONBLOCK / SOCK_CLOEXEC  the type-word flag bits (04000 / 02000000)
//   IPPROTO_TCP / IPPROTO_UDP  include/netinet/in.h (6 / 17)
// VIV_SOCK_NONBLOCK / VIV_SOCK_CLOEXEC moved to vivarium.h (the socket shell in
// syscall.c also needs them to apply the flags to the ctl fd).
enum {
    VIV_AF_INET        = 2,
    VIV_AF_INET6       = 10,
    VIV_SOCK_STREAM    = 1,
    VIV_SOCK_DGRAM     = 2,
    VIV_IPPROTO_TCP    = 6,
    VIV_IPPROTO_UDP    = 17,
};

struct viv_sock *viv_socktab_find(struct viv_socktab *tab, s32 fd) {
    if (!tab || fd < 0) return NULL;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) {
        if (tab->s[i].state != VIV_SOCK_FREE && tab->s[i].fd == fd)
            return &tab->s[i];
    }
    return NULL;
}

struct viv_sock *viv_socktab_claim(struct viv_socktab *tab, s32 fd,
                                   enum viv_net_proto proto, u32 n) {
    if (!tab || fd < 0) return NULL;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) {
        if (tab->s[i].state == VIV_SOCK_FREE) {
            tab->s[i].fd          = fd;
            tab->s[i].proto       = (u8)proto;
            tab->s[i].state       = VIV_SOCK_FRESH;
            tab->s[i].n           = n;
            tab->s[i].bound_addr  = 0;   // a fresh socket carries no bind
            tab->s[i].bound_port  = 0;
            tab->s[i].remote_addr = 0;   // and no unconnected-sendto destination
            tab->s[i].remote_port = 0;
            return &tab->s[i];
        }
    }
    return NULL;   // full -> EMFILE
}

bool viv_socktab_has_room(const struct viv_socktab *tab) {
    if (!tab) return false;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++)
        if (tab->s[i].state == VIV_SOCK_FREE) return true;
    return false;
}

void viv_socktab_drop_slot(struct viv_sock *e) {
    if (!e) return;
    // Clear the WHOLE entry, not just the state. A stale field left in a freed
    // slot would be visible to any future claim that forgets to set it -- and
    // the bind fields make that concrete: a recycled slot still carrying the
    // previous socket's port would let a listen() announce a port this socket
    // never asked for.
    e->fd          = -1;
    e->proto       = 0;
    e->state       = VIV_SOCK_FREE;
    e->n           = 0;
    e->bound_addr  = 0;
    e->bound_port  = 0;
    e->remote_addr = 0;   // else a recycled slot reports a stranger's peer as
    e->remote_port = 0;   // this socket's, exactly as a stale bound_port would
}

void viv_socktab_drop(struct viv_socktab *tab, s32 fd) {
    viv_socktab_drop_slot(viv_socktab_find(tab, fd));
}

// execve's close-on-exec socktab sweep. A socket fd marked FD_CLOEXEC is about
// to be closed by handle_close_on_exec, which knows nothing of this table; drop
// its entry first, or the freed fd number would carry a stale (proto, n) row
// into the new image (whose first fd-creating call is handed that number, then
// viv_socktab_find returns the stale connection). Reads the cloexec bit LIVE
// from the handle table -- the entry does not cache it, so two entries that
// ever share an fd necessarily agree on it. Clears BY SLOT (the pointer is in
// hand) rather than via viv_socktab_drop's re-find. The caller (sys_execve_core)
// runs this in execve's sole-live-thread window (proc_exec_alone), so no lock is
// taken. NULL-safe: a native Proc has no socktab.
void viv_socktab_drop_cloexec(struct Proc *p) {
    if (!p) return;
    struct viv_socktab *tab = __atomic_load_n(&p->socktab, __ATOMIC_ACQUIRE);
    if (!tab) return;
    for (u32 i = 0; i < VIV_SOCK_MAX; i++) {
        struct viv_sock *e = &tab->s[i];
        if (e->state != VIV_SOCK_FREE &&
            handle_get_cloexec(p, (hidx_t)e->fd) == 1)
            viv_socktab_drop_slot(e);
    }
}

bool vivarium_socket_decide(u64 domain, u64 type, u64 protocol,
                            enum viv_net_proto *out_proto, s32 *out_err) {
    s32 err = T_E_INVAL;
    if (!out_proto || !out_err) { if (out_err) *out_err = T_E_INVAL; return false; }

    if (domain == VIV_AF_INET6) {
        // Honest, not silent: serving a v6 socket over v4 /net would connect
        // the guest to somewhere it did not ask for.
        *out_err = T_E_AFNOSUPPORT;
        return false;
    }
    if (domain != VIV_AF_INET) {
        *out_err = T_E_AFNOSUPPORT;
        return false;
    }

    // The type word carries SOCK_NONBLOCK/SOCK_CLOEXEC in its high bits. Mask
    // them off before the base-type switch and ADMIT them: the shell applies
    // NONBLOCK as the open-file's CNONBLOCK and CLOEXEC as the fd's cloexec bit,
    // so the guest gets exactly the socket it asked for. (They were refused
    // until N-1a, when musl's DNS resolver -- socket(DGRAM|CLOEXEC|NONBLOCK) at
    // res_msend.c:123 -- became the consumer that needs them; a blocking socket
    // there would hang the recvmsg drain loop, and an un-cloexec'd one would
    // leak the resolver fd across the git exec.) Reject any OTHER high bit: a
    // type word carrying an unknown flag is a request we cannot honour truthfully.
    u64 base_type = type & ~(u64)(VIV_SOCK_NONBLOCK | VIV_SOCK_CLOEXEC);
    if (base_type & ~(u64)0xFFu) {   // no base SOCK_* value exceeds a byte
        *out_err = T_E_INVAL;
        return false;
    }

    enum viv_net_proto proto;
    switch (base_type) {
    case VIV_SOCK_STREAM:
        proto = VIV_NET_TCP;
        if (protocol != 0 && protocol != VIV_IPPROTO_TCP) {
            *out_err = T_E_PROTONOSUPPORT;
            return false;
        }
        break;
    case VIV_SOCK_DGRAM:
        proto = VIV_NET_UDP;
        if (protocol != 0 && protocol != VIV_IPPROTO_UDP) {
            *out_err = T_E_PROTONOSUPPORT;
            return false;
        }
        break;
    default:
        // SOCK_SEQPACKET / SOCK_RAW / anything else: no /net analogue.
        *out_err = T_E_PROTONOSUPPORT;
        return false;
    }

    (void)err;
    *out_proto = proto;
    return true;
}

bool vivarium_getsockopt_decide(u64 level, u64 optname, s32 *out_err) {
    if (!out_err) return false;                  // fail closed
    *out_err = T_E_NOSYS;
    // Narrowed to 32 bits: both are C `int`s in the Linux ABI (the openat
    // precedent -- the narrowing IS the ABI; contrast clone's full-width read,
    // whose argument is an unsigned long).
    if ((u32)level   != (u32)VIV_SOL_SOCKET) return false;
    if ((u32)optname != (u32)VIV_SO_ERROR)   return false;
    *out_err = 0;
    return true;
}

bool vivarium_sendto_decide(enum viv_net_proto proto, u8 state, u64 flags,
                            u64 addr_va, u64 addrlen, s32 *out_err) {
    if (!out_err) return false;                  // fail closed
    *out_err = T_E_NOSYS;

    // flags is a C int: narrowed (the openat precedent). MSG_NOSIGNAL is
    // admitted as a TRUTHFUL no-op: it suppresses SIGPIPE, and the phenotype
    // socket data path is a 9P Spoor write -- the pipe EPIPE-note machinery
    // never runs there, so there is no SIGPIPE to suppress in the first
    // place. Everything else would lie: MSG_DONTWAIT on a blocking-only
    // socket, MSG_MORE on a stack that cannot coalesce.
    if (((u32)flags & ~(u32)VIV_MSG_NOSIGNAL) != 0) return false;

    // sendto WITH a destination -- the connectionless datagram shape, which is
    // exactly what musl's DNS resolver uses (res_msend.c:189: one bound UDP
    // socket, a sendto per nameserver). SERVED FOR UDP (N-2a): the shell writes
    // `connect ip!port` to the ctl fd per datagram (netd re-points conn N) and
    // moves the bytes on a transient data fid. It requires a socket whose fd is
    // still its `ctl` -- FRESH: a CONNECTED fd has already been swapped onto
    // `data` (no ctl to dial), and a LISTENING one is a server. TCP has no
    // connectionless send, so it stays ENOSYS -- census-visible unbuilt, not a
    // fake state errno.
    if (addr_va != 0 || addrlen != 0) {
        if (proto != VIV_NET_UDP)             return false;   // ENOSYS
        if (state == (u8)VIV_SOCK_CONNECTED) { *out_err = T_E_ISCONN; return false; }
        if (state == (u8)VIV_SOCK_LISTENING) { *out_err = T_E_ISCONN; return false; }
        *out_err = 0;
        return true;                                          // state FRESH
    }

    // sendto with NO destination -- the connected send()/write() shape. Only the
    // CONNECTED shape is served; an unconnected send with no addr is genuinely
    // UNBUILT (there is no dial without a destination), so it declines to ENOSYS
    // -- census-visible, NOT a fabricated ENOTCONN (the R2-F1 correction: that
    // both mismatched Linux and hid the missing capability from the census).
    if (state != (u8)VIV_SOCK_CONNECTED) return false;   // *out_err == T_E_NOSYS

    *out_err = 0;
    return true;
}

bool vivarium_recvfrom_decide(u8 state, u64 flags, u64 addr_va, s32 *out_err) {
    if (!out_err) return false;                  // fail closed
    *out_err = T_E_NOSYS;

    // A non-NULL source-address out-pointer asks for the peer's address
    // written back -- state the socktab does not carry. Declined until a
    // consumer needs it (recv() passes NULL).
    if (addr_va != 0) return false;

    // No recv flag is truthfully servable: MSG_PEEK needs a non-consuming
    // read the 9P data path does not have, MSG_WAITALL changes the return
    // contract, MSG_DONTWAIT lies on a blocking-only socket.
    if ((u32)flags != 0) return false;

    // Only CONNECTED is served (R2-F1, as sendto): a recv on an unconnected
    // socket is UNBUILT -- notably the bound-UDP-server idiom (bind then recv)
    // that Linux serves -- so it declines to ENOSYS, census-visible, rather
    // than claiming a fake ENOTCONN over a state Linux considers valid.
    if (state != (u8)VIV_SOCK_CONNECTED) return false;   // *out_err == T_E_NOSYS

    *out_err = 0;
    return true;
}

bool vivarium_recvmsg_decide(u8 state, u64 flags, bool has_remote, s32 *out_err) {
    if (!out_err) return false;                  // fail closed
    *out_err = T_E_NOSYS;

    // Same flag stance as recvfrom: no recv flag is truthfully servable here
    // (MSG_PEEK has no non-consuming 9P read, MSG_WAITALL changes the return
    // contract, MSG_DONTWAIT is redundant with the socket's own CNONBLOCK).
    if ((u32)flags != 0) return false;

    // CONNECTED (the fd is `data`) OR a FRESH socket that has been sendto'd (a
    // recorded remote -- the DNS datagram path) can produce a datagram. A FRESH
    // socket that was never sent to has no connection to read, and a LISTENING
    // one is a server; both decline to ENOSYS rather than fabricate a state error.
    if (state == (u8)VIV_SOCK_CONNECTED)               { *out_err = 0; return true; }
    if (state == (u8)VIV_SOCK_FRESH && has_remote)     { *out_err = 0; return true; }

    return false;                                // *out_err == T_E_NOSYS
}

const char *vivarium_net_proto_dir(enum viv_net_proto proto) {
    switch (proto) {
    case VIV_NET_UDP: return "udp";
    case VIV_NET_TCP:
    default:          return "tcp";
    }
}

bool vivarium_sockaddr_in_parse_any(const u8 *sa, u64 salen,
                                    u8 out_ip4[4], u16 *out_port) {
    // struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port;
    //                      struct in_addr sin_addr; ... } -- family at 0 (2
    //  bytes), port at 2 (2 bytes, NETWORK order), addr at 4 (4 bytes, the
    //  octets a.b.c.d in memory order).
    if (!sa || !out_ip4 || !out_port) return false;
    if (salen < 8)                    return false;

    u16 family = (u16)((u16)sa[0] | ((u16)sa[1] << 8));   // little-endian host
    if (family != VIV_AF_INET)        return false;

    // Network order: high byte first. Read the bytes directly rather than
    // depending on an ntohs, exactly as the pouch boundary-line does.
    *out_port  = (u16)(((u16)sa[2] << 8) | (u16)sa[3]);
    out_ip4[0] = sa[4];
    out_ip4[1] = sa[5];
    out_ip4[2] = sa[6];
    out_ip4[3] = sa[7];
    return true;
}

bool vivarium_sockaddr_in_parse(const u8 *sa, u64 salen,
                                u8 out_ip4[4], u16 *out_port) {
    u16 port = 0;
    if (!vivarium_sockaddr_in_parse_any(sa, salen, out_ip4, &port)) return false;
    if (port == 0) return false;   // netd's dial parser refuses it
    *out_port = port;
    return true;
}

u32 vivarium_sockaddr_in_build(u8 *buf, u32 buflen, const u8 ip4[4], u16 port) {
    if (!buf || !ip4 || buflen < 16) return 0;
    for (u32 i = 0; i < 16; i++) buf[i] = 0;   // sin_zero[8] must be zero
    buf[0]  = (u8)(VIV_AF_INET & 0xFF);        // family, little-endian host
    buf[1]  = (u8)((VIV_AF_INET >> 8) & 0xFF);
    buf[2]  = (u8)(port >> 8);                 // port, NETWORK order
    buf[3]  = (u8)(port & 0xFF);
    buf[4]  = ip4[0];
    buf[5]  = ip4[1];
    buf[6]  = ip4[2];
    buf[7]  = ip4[3];
    return 16;
}

// Append a decimal u32. Returns the new offset, or `buflen + 1` to signal
// overflow -- a sentinel strictly greater than any valid offset, so one check
// at the end catches an overflow from any stage.
static u32 viv_put_dec(char *buf, u32 buflen, u32 off, u32 v) {
    char tmp[10];
    u32  n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (off > buflen || buflen - off < n) return buflen + 1;
    for (u32 i = 0; i < n; i++) buf[off + i] = tmp[n - 1 - i];
    return off + n;
}

static u32 viv_put_char(char *buf, u32 buflen, u32 off, char c) {
    if (off >= buflen) return buflen + 1;
    buf[off] = c;
    return off + 1;
}

u32 vivarium_net_cmd_ipport(char *buf, u32 buflen, const char *verb,
                            const u8 ip4[4], u16 port) {
    if (!buf || !verb || !ip4 || buflen == 0) return 0;

    u32 off = 0;
    for (u32 i = 0; verb[i] != '\0'; i++) off = viv_put_char(buf, buflen, off, verb[i]);
    off = viv_put_char(buf, buflen, off, ' ');
    for (u32 i = 0; i < 4; i++) {
        if (i) off = viv_put_char(buf, buflen, off, '.');
        off = viv_put_dec(buf, buflen, off, ip4[i]);
    }
    off = viv_put_char(buf, buflen, off, '!');
    off = viv_put_dec(buf, buflen, off, port);

    // One check for every stage: any overflow poisoned `off` past buflen.
    if (off > buflen) return 0;
    return off;
}

u32 vivarium_net_cmd_announce(char *buf, u32 buflen, const u8 ip4[4], u16 port) {
    if (!buf || !ip4 || buflen == 0) return 0;

    // 0.0.0.0 is INADDR_ANY, whose Plan 9 spelling is the `*` wildcard. A
    // concrete address keeps its dotted quad -- see the header for why these
    // two reach different listeners in netd and must not be collapsed.
    if (ip4[0] == 0 && ip4[1] == 0 && ip4[2] == 0 && ip4[3] == 0) {
        u32 off = 0;
        const char *verb = "announce *!";
        for (u32 i = 0; verb[i] != '\0'; i++) off = viv_put_char(buf, buflen, off, verb[i]);
        off = viv_put_dec(buf, buflen, off, port);
        if (off > buflen) return 0;
        return off;
    }
    return vivarium_net_cmd_ipport(buf, buflen, "announce", ip4, port);
}

bool vivarium_parse_ipport(const char *buf, u32 len, u8 out_ip4[4], u16 *out_port) {
    if (!buf || !out_ip4 || !out_port || len == 0) return false;

    u8  oct[4] = {0, 0, 0, 0};
    u32 i      = 0;

    for (u32 k = 0; k < 4; k++) {
        if (k > 0) {
            if (i >= len || buf[i] != '.') return false;
            i++;
        }
        u32 v    = 0;
        u32 seen = 0;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') {
            v = v * 10u + (u32)(buf[i] - '0');
            if (v > 255) return false;         // not an octet
            i++;
            seen++;
            if (seen > 3) return false;
        }
        if (seen == 0) return false;
        oct[k] = (u8)v;
    }

    if (i >= len || buf[i] != '!') return false;
    i++;

    u32 port = 0;
    u32 seen = 0;
    while (i < len && buf[i] >= '0' && buf[i] <= '9') {
        port = port * 10u + (u32)(buf[i] - '0');
        if (port > 65535) return false;
        i++;
        seen++;
    }
    if (seen == 0) return false;

    // Anything after the port must be line padding, never more address.
    while (i < len) {
        char c = buf[i];
        if (c != '\n' && c != ' ' && c != '\0') return false;
        i++;
    }

    for (u32 k = 0; k < 4; k++) out_ip4[k] = oct[k];
    *out_port = (u16)port;
    return true;
}

bool vivarium_listen_decide(enum viv_net_proto proto, enum viv_sock_state state,
                            u16 bound_port, s32 *out_err) {
    if (!out_err) return false;

    // A datagram socket has no listen file at all: netd's walk rejects `listen`
    // outside /net/tcp, and ctl_announce refuses a non-TCP slot. EOPNOTSUPP is
    // the POSIX code for exactly this ("not a type that supports listen").
    if (proto != VIV_NET_TCP) { *out_err = T_E_OPNOTSUPP; return false; }

    switch (state) {
    case VIV_SOCK_LISTENING:
        // Already listening. POSIX lets a repeat listen() succeed (it may only
        // adjust the backlog, which netd owns), so this is a quiet 0 rather
        // than an error -- and re-announcing would fail anyway, the socket
        // being already open.
        *out_err = 0;
        return false;
    case VIV_SOCK_CONNECTED:
        *out_err = T_E_INVAL;
        return false;
    case VIV_SOCK_FRESH:
        break;
    default:
        *out_err = T_E_INVAL;
        return false;
    }

    // netd's announce parser rejects port 0, and inventing an ephemeral one
    // here would be a translation the guest did not ask for.
    if (bound_port == 0) { *out_err = T_E_OPNOTSUPP; return false; }

    *out_err = 0;
    return true;
}

bool vivarium_parse_conn_n(const char *buf, u32 len, u32 *out_n) {
    if (!buf || !out_n || len == 0) return false;

    u64 v     = 0;
    u32 seen  = 0;
    for (u32 i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n' || c == ' ' || c == '\0') break;   // netd may pad the line
        if (c < '0' || c > '9')                 return false;
        v = v * 10u + (u64)(c - '0');
        if (v > 0xFFFFFFFFull)                  return false;
        seen++;
    }
    if (seen == 0) return false;
    *out_n = (u32)v;
    return true;
}

// -----------------------------------------------------------------------------
// Readiness (V-5c, section 5.5.4).
// -----------------------------------------------------------------------------

bool vivarium_timespec_to_ms(s64 sec, s64 nsec, s32 *out_ms, s32 *out_err) {
    if (!out_ms || !out_err) return false;
    *out_err = 0;

    // Linux's own validation, and it must come first: a negative field is not a
    // short wait, it is a malformed request.
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
        *out_err = (s32)T_E_INVAL;
        return false;
    }

    // Saturate before multiplying rather than after, so the multiply itself can
    // never overflow. INT32_MAX ms is ~24.8 days; a caller wanting longer gets a
    // shorter wait and loops.
    const s64 max_ms = 0x7FFFFFFF;
    if (sec > max_ms / 1000) {
        *out_ms = (s32)max_ms;
        return true;
    }

    // Round UP: a non-zero nanosecond remainder must not vanish, because 0 means
    // "do not wait at all" and would silently convert a brief wait into a spin.
    s64 ms = sec * 1000 + (nsec + 999999) / 1000000;
    if (ms > max_ms) ms = max_ms;
    *out_ms = (s32)ms;
    return true;
}

bool vivarium_ppoll_decide(u64 nfds, u64 sigmask_va, s32 *out_err) {
    if (!out_err) return false;
    *out_err = 0;

    // The atomic mask swap has no counterpart -- see the header. Checked FIRST
    // so a caller using ppoll for its signal semantics learns that is the
    // unserved part, rather than being told its nfds is wrong.
    if (sigmask_va != 0) {
        *out_err = (s32)T_E_NOSYS;
        return false;
    }

    // A real bad value, so a real POSIX error.
    if (nfds > POLL_MAX_NFDS) {
        *out_err = (s32)T_E_INVAL;
        return false;
    }

    // nfds == 0 is DELIBERATELY NOT REJECTED HERE. It is Linux's "sleep for the
    // timeout", and the caller routes it to viv_timed_sleep rather than to the
    // native poll (which does reject nfds == 0). See the header.
    return true;
}

// -----------------------------------------------------------------------------
// pselect6 -- the fd_set reshape (V-5c-2). All PURE; the header carries the
// reasoning, including the four defects in pouch's userspace twin (task #99)
// that each of these decisions is the counterpart of.
// -----------------------------------------------------------------------------

// Read bit `fd` of an fd_set, NULL-safe (a NULL set is "no bits requested").
//
// ENDIANNESS. musl's FD_SET is
//     fds_bits[d / (8*sizeof(long))] |= 1UL << (d % (8*sizeof(long)))
// i.e. bit d of a 64-bit long. On a LITTLE-endian target -- aarch64 as Thylacine
// builds it -- byte b of that long holds bits [8b, 8b+8), so bit d lands at byte
// d/8, bit d%8, and the byte-wise form below is exact. It would be wrong on a
// big-endian port, which is why this is spelled out rather than assumed.
static bool viv_fdset_bit(const u8 *set, u32 fd) {
    if (!set) return false;
    return ((set[fd >> 3] >> (fd & 7u)) & 1u) != 0;
}

static void viv_fdset_set_bit(u8 *set, u32 fd) {
    set[fd >> 3] |= (u8)(1u << (fd & 7u));
}

bool vivarium_pselect6_decide(u64 nfds_raw, u64 sigmask_va, u32 *out_nfds,
                              s32 *out_err) {
    if (!out_nfds || !out_err) return false;
    *out_err  = 0;
    *out_nfds = 0;

    // Checked first, exactly as in ppoll: a caller reaching for pselect6's
    // signal semantics should learn THAT is the unserved part.
    if (sigmask_va != 0) {
        *out_err = (s32)T_E_NOSYS;
        return false;
    }

    // Linux's own check, and it must be signed AND 32-bit: nfds is an `int`, so
    // only the low word is significant. A caller may leave x0 sign-extended
    // (0xFFFFFFFFFFFFFFFF) or merely zero-extended (0x00000000FFFFFFFF) and both
    // mean -1; testing the raw 64-bit value would refuse one and silently CLAMP
    // the other to PROC_HANDLE_MAX -- working on some toolchains and not others,
    // exactly what vivarium_openat_decide's `(s32)(u32)dirfd` exists to avoid.
    s64 nfds = (s64)(s32)(u32)nfds_raw;
    if (nfds < 0) {
        *out_err = (s32)T_E_INVAL;
        return false;
    }

    // Clamp to the fd table, which is Linux's `if (n > max_fds) n = max_fds`.
    // PROC_HANDLE_MAX is NAMED rather than copied on purpose -- pouch's select
    // hardcoded this bound, the kernel later split PROC_HANDLE_MAX from
    // POLL_MAX_NFDS, and the copy silently became a bug (task #99 F-a).
    u64 n = (u64)nfds;
    if (n > PROC_HANDLE_MAX) n = PROC_HANDLE_MAX;
    *out_nfds = (u32)n;
    return true;
}

u32 vivarium_fdset_bytes(u32 nfds) {
    // Linux FDS_BYTES: FDS_LONGS(nr) * sizeof(long), i.e. whole 8-byte units.
    u32 longs = (nfds + 63u) / 64u;
    u32 bytes = longs * 8u;
    if (bytes > VIV_FD_SET_BYTES) bytes = VIV_FD_SET_BYTES;
    return bytes;
}

bool vivarium_fdset_to_pollfds(const u8 *rd, const u8 *wr, const u8 *ex,
                               u32 nfds, struct pollfd *out, u32 out_cap,
                               u32 *out_count, s32 *out_err) {
    if (!out || !out_count || !out_err) return false;
    *out_err   = 0;
    *out_count = 0;

    u32 n = 0;
    for (u32 fd = 0; fd < nfds; fd++) {
        // Decline BEFORE building anything. A set exceptfds bit has no native
        // representation, and the alternative -- dropping it and polling the
        // rest -- turns a wait that can never be satisfied into an infinite
        // block instead of an error.
        if (viv_fdset_bit(ex, fd)) {
            *out_err = (s32)T_E_NOSYS;
            return false;
        }

        s16 events = 0;
        if (viv_fdset_bit(rd, fd)) events |= POLLIN;
        if (viv_fdset_bit(wr, fd)) events |= POLLOUT;
        if (events == 0) continue;

        // THE CEILING IS ON CONTRIBUTING FDS, NOT ON FD VALUES. A select over
        // fds 200 and 201 is two pollfds and fits; a select over 65 low fds does
        // not. This is pouch's F-a inverted.
        if (n >= out_cap) {
            *out_err = (s32)T_E_INVAL;
            return false;
        }
        out[n].fd      = (s32)fd;
        out[n].events  = events;
        out[n].revents = 0;
        n++;
    }

    *out_count = n;
    return true;
}

bool vivarium_pollfds_to_fdset(const struct pollfd *pfds, u32 count,
                               u8 *rd, u8 *wr, u8 *ex, u32 *out_bits,
                               s32 *out_err) {
    if ((!pfds && count != 0) || !out_bits || !out_err) return false;
    *out_err  = 0;
    *out_bits = 0;

    // POLLNVAL anywhere fails the WHOLE call -- select has no per-fd error
    // channel the way poll does. Checked before any output buffer is touched,
    // because on an error Linux leaves the caller's sets unmodified.
    for (u32 i = 0; i < count; i++) {
        if ((pfds[i].revents & POLLNVAL) != 0) {
            *out_err = (s32)T_E_BADF;
            return false;
        }
    }

    // select reports by OVERWRITING: a bit the caller asked about and did not
    // get back must come home clear.
    for (u32 i = 0; i < VIV_FD_SET_BYTES; i++) {
        if (rd) rd[i] = 0;
        if (wr) wr[i] = 0;
        if (ex) ex[i] = 0;   // every set bit already declined; stays zero.
    }

    for (u32 i = 0; i < count; i++) {
        s16 rev = pfds[i].revents;
        if (rev == 0) continue;

        s32 fd = pfds[i].fd;
        // Defensive: the scan above only ever emits fds inside the clamp, so
        // this cannot fire -- but the write below is an unchecked bit index.
        if (fd < 0 || fd >= (s32)VIV_FD_SETSIZE) continue;

        // POLLIN_SET = POLLIN|POLLHUP|POLLERR, gated on having been REQUESTED
        // for reading, so an errored fd listed only in readfds comes back only
        // in readfds.
        if (rd && (pfds[i].events & POLLIN) != 0 &&
            (rev & (POLLIN | POLLHUP | POLLERR)) != 0) {
            viv_fdset_set_bit(rd, (u32)fd);
            (*out_bits)++;
        }

        // POLLOUT_SET = POLLOUT|POLLERR -- NO POLLHUP, which is the asymmetry
        // pouch's F-c gets wrong. A hung-up peer leaves data readable; writing
        // to it is an error, not a completion.
        if (wr && (pfds[i].events & POLLOUT) != 0 &&
            (rev & (POLLOUT | POLLERR)) != 0) {
            viv_fdset_set_bit(wr, (u32)fd);
            (*out_bits)++;
        }
    }

    // *out_bits counts BITS, so an fd ready both ways has just counted twice.
    // That is Linux's contract and pouch's F-d.
    return true;
}

// Bounded name compare, the notes.c `notes_name_eq` discipline: stop at
// NOTE_NAME_MAX so an unterminated 16-byte buffer is still safe to read.
static bool viv_name_eq(const char *a, const char *b) {
    for (u32 i = 0; i < NOTE_NAME_MAX; i++) {
        if (a[i] != b[i]) return false;
        if (a[i] == 0)    return true;
    }
    return false;
}

enum viv_signote viv_signote_from_note_name(const char *name) {
    if (!name) return VIV_SIGNOTE_NONE;

    // The names are notes.h's canonical literals, so this cannot drift from the
    // posters. `kill` is present for completeness of the decode; nothing can put
    // it in a disposition table (vivarium_sigaction_decide refuses SIGKILL).
    if (viv_name_eq(name, NOTE_NAME_INTERRUPT))  return VIV_SIGNOTE_INTERRUPT;
    if (viv_name_eq(name, NOTE_NAME_KILL))       return VIV_SIGNOTE_KILL;
    if (viv_name_eq(name, NOTE_NAME_PIPE))       return VIV_SIGNOTE_PIPE;
    if (viv_name_eq(name, NOTE_NAME_CHILD_EXIT)) return VIV_SIGNOTE_CHILD_EXIT;
    if (viv_name_eq(name, NOTE_NAME_TTY_HUP))    return VIV_SIGNOTE_TTY_HUP;
    if (viv_name_eq(name, NOTE_NAME_TTY_QUIT))   return VIV_SIGNOTE_TTY_QUIT;
    if (viv_name_eq(name, NOTE_NAME_TTY_WINCH))  return VIV_SIGNOTE_TTY_WINCH;
    if (viv_name_eq(name, NOTE_NAME_TTY_SUSP))   return VIV_SIGNOTE_TTY_SUSP;
    if (viv_name_eq(name, NOTE_NAME_TTY_CONT))   return VIV_SIGNOTE_TTY_CONT;

    // snare:* deliberately absent -- viv_signote_is_deliverable says why, and a
    // name that never reaches notes_post has nothing to decode here.
    return VIV_SIGNOTE_NONE;
}

const char *viv_signote_note_name(enum viv_signote note) {
    switch (note) {
    case VIV_SIGNOTE_INTERRUPT:  return NOTE_NAME_INTERRUPT;
    case VIV_SIGNOTE_KILL:       return NOTE_NAME_KILL;
    case VIV_SIGNOTE_PIPE:       return NOTE_NAME_PIPE;
    case VIV_SIGNOTE_CHILD_EXIT: return NOTE_NAME_CHILD_EXIT;
    case VIV_SIGNOTE_TTY_HUP:    return NOTE_NAME_TTY_HUP;
    case VIV_SIGNOTE_TTY_QUIT:   return NOTE_NAME_TTY_QUIT;
    case VIV_SIGNOTE_TTY_WINCH:  return NOTE_NAME_TTY_WINCH;
    case VIV_SIGNOTE_TTY_SUSP:   return NOTE_NAME_TTY_SUSP;
    case VIV_SIGNOTE_TTY_CONT:   return NOTE_NAME_TTY_CONT;
    default:                     return NULL;
    }
}

// THE canonical NOTE_BIT_* map (vivarium.h says why there is exactly one).
// This file already includes notes.h for the canonical note-name literals, so
// the numbering is taken from its defining header rather than retyped.
const struct viv_notebit_map g_viv_notebits = {
    .interrupt  = NOTE_BIT_INTERRUPT,
    .kill       = NOTE_BIT_KILL,
    .pipe       = NOTE_BIT_PIPE,
    .child_exit = NOTE_BIT_CHILD_EXIT,
    .snare      = NOTE_BIT_SNARE,
    .tty        = NOTE_BIT_TTY,
};

u64 viv_signote_to_signal(enum viv_signote note) {
    if (note == VIV_SIGNOTE_NONE) return 0;
    for (u64 sig = 1; sig <= VIV_NSIG; sig++)
        if (viv_signal_note(sig) == note) return sig;
    return 0;
}

bool viv_signote_default_is_ignore(enum viv_signote note) {
    switch (note) {
    // SIGCHLD / SIGWINCH: POSIX default is to discard. SIGCONT's default is
    // "continue", which for a Proc already running (and a Proc at its EL0
    // return tail IS running) is indistinguishable from discarding.
    case VIV_SIGNOTE_CHILD_EXIT:
    case VIV_SIGNOTE_TTY_WINCH:
    case VIV_SIGNOTE_TTY_CONT:
        return true;

    // Everything else terminates, stops, or has no note. TTY_SUSP is the one
    // worth naming: its default is STOP -- applied at post time by job_stop_cb
    // or at delivery by the tail's NDFLT-stop arm (434c3fd9 built it; the
    // header's paragraph has the history). Reporting "ignore" for it would be
    // a lie in the opposite direction from the one V-6b spent its time
    // removing.
    default:
        return false;
    }
}

bool viv_signote_default_is_terminate(enum viv_signote note) {
    switch (note) {
    // SIGPIPE / SIGINT / SIGHUP / SIGQUIT: the POSIX default is to terminate.
    // PIPE is the row that earns this function its existence: the native
    // `pipe` note has no terminate latch (a Plan 9 ABI question, task #237),
    // so the native uncaught arm never acts on it -- and a Linux guest, which
    // has no notes fd, would otherwise carry a SIG_DFL SIGPIPE at the head of
    // its queue for the rest of its life. The Linux default is not in
    // question, so the phenotype branch answers it here, for its own Procs.
    case VIV_SIGNOTE_PIPE:
    case VIV_SIGNOTE_INTERRUPT:
    case VIV_SIGNOTE_TTY_HUP:
    case VIV_SIGNOTE_TTY_QUIT:
        return true;

    // KILL also terminates, but it is answered by the dispatcher's kill branch
    // before any disposition is consulted (non-catchable both sides), and it
    // can never sit in a sigtab. The snare:* rows never reach a queue. TTY_SUSP
    // stops; the three above ignore; NONE has no note.
    default:
        return false;
    }
}

// VIV_SIGNOTE_NONE is a SENTINEL -- "no note in this tree carries that signal"
// -- and it is simultaneously a valid array index, 0. Excluding it in EVERY
// accessor is what stops those two facts meeting: act[0] is then neither
// readable nor writable through this API, so a decode miss cannot alias onto a
// real disposition.
//
// It was already unreachable, but only by an argument that spanned two files.
// vivarium_sigaction_decide refuses a signum whose note is NONE, so nothing
// could WRITE act[0]; the zeroed slot then read back as SIG_DFL and every
// caller behaved. True, and fragile in the same breath -- the READ sites in
// notes.c pass viv_signote_from_note_name(name) straight in, and that decode
// answers NONE for any name absent from its table. Add a name to g_known_notes
// (task #95's `terminate` is the one already queued) without adding its decode
// row, and every disposition lookup for that name silently consults slot 0.
//
// The guard makes the answer local and total: an unknown note has no slot.
static bool viv_signote_indexable(enum viv_signote note) {
    return (u32)note != (u32)VIV_SIGNOTE_NONE && (u32)note < VIV_SIGNOTE_COUNT;
}

// THE ACCESS DISCIPLINE FOR `viv_sigtab` ENTRIES (main#243 F2/F6; aux#254).
//
// The table is read LOCK-FREE from other Procs' CPUs (notes_post's SIG_IGN hook,
// notes_proc_has_live_handler, notes_proc_default_applies -- each takes an
// arbitrary Proc) while its own Proc's thread writes it (rt_sigaction,
// SA_RESETHAND, exec's reset). Three facts make that sound, and each is a
// CONSTRUCTION here rather than a codegen property someone once measured:
//
//   1. Every field is accessed with a __atomic_* op on a naturally aligned u64
//      -- single-copy-atomic per FIELD by C11, so no reader ever observes a word
//      NO WRITER STORED (the byte loop this replaced was measured to compile to
//      halfword stores under -ffreestanding -fno-builtin, and could publish a
//      handler half old / half zero that passed the `> VIV_SIG_IGN` gate).
//   2. An INSTALL publishes `handler` LAST with RELEASE and a reader loads it
//      with ACQUIRE -- so a reader that sees a live handler sees the flags /
//      restorer / mask installed WITH it (message passing), never a torn entry
//      from a concurrent rt_sigaction.
//   3. A RESET zeroes `handler` FIRST: a reader either sees SIG_DFL and stops,
//      or sees the old live handler beside fields that may already be zero.
//      That second view is the ONLY inconsistent entry a reader can observe,
//      and it is harmless for exactly one reason -- every CROSS-Proc reader
//      acts on `handler` alone (notes_proc_has_live_handler discards its copy;
//      note_ignored reads nothing else), and every reader that CONSUMES the
//      copied fields (delivery at EL0 return, the handler-mask compose) runs
//      on a thread of the table's own Proc, which cannot run during that
//      Proc's exec (proc_exec_alone). A future cross-Proc consumer of the
//      copied fields re-opens this argument; say so at the site.
//
// What is deliberately NOT promised: entry-to-entry consistency. A reader may
// see a mix of pre- and post-reset ENTRIES, which is the latitude POSIX gives a
// sigaction racing a signal already in flight.
static inline u64 sigact_ld_handler(const struct viv_ksigaction *a) {
    return __atomic_load_n(&a->handler, __ATOMIC_ACQUIRE);
}
static inline void sigact_st_default(struct viv_ksigaction *a) {
    __atomic_store_n(&a->handler,  VIV_SIG_DFL, __ATOMIC_RELAXED);   // gate first
    __atomic_store_n(&a->flags,    0ull,        __ATOMIC_RELAXED);
    __atomic_store_n(&a->restorer, 0ull,        __ATOMIC_RELAXED);
    __atomic_store_n(&a->mask,     0ull,        __ATOMIC_RELAXED);
}

bool viv_sigtab_note_ignored(const struct viv_sigtab *tab,
                             enum viv_signote note) {
    if (!tab) return false;                       // no table == nothing ignored
    if (!viv_signote_indexable(note)) return false;
    return sigact_ld_handler(&tab->act[(u32)note]) == VIV_SIG_IGN;
}

bool viv_sigtab_note_handler(const struct viv_sigtab *tab,
                             enum viv_signote note,
                             struct viv_ksigaction *out) {
    if (!tab || !out) return false;
    if (!viv_signote_indexable(note)) return false;

    const struct viv_ksigaction *a = &tab->act[(u32)note];
    // SIG_DFL is 0 and SIG_IGN is 1 -- neither is an address to jump to. A
    // zeroed table therefore reads as all-default with no separate init.
    // The handler is loaded ONCE (acquire) and gated on that value: a second
    // load could see a different disposition and hand out a stale address.
    u64 h = sigact_ld_handler(a);
    if (h == VIV_SIG_DFL || h == VIV_SIG_IGN) return false;

    out->handler  = h;
    out->flags    = __atomic_load_n(&a->flags,    __ATOMIC_RELAXED);
    out->restorer = __atomic_load_n(&a->restorer, __ATOMIC_RELAXED);
    out->mask     = __atomic_load_n(&a->mask,     __ATOMIC_RELAXED);
    return true;
}

bool viv_sigtab_set(struct viv_sigtab *tab, enum viv_signote note,
                    const struct viv_ksigaction *act) {
    if (!tab || !act) return false;
    if (!viv_signote_indexable(note)) return false;
    struct viv_ksigaction *a = &tab->act[(u32)note];
    // Fields first, the gate LAST with release (discipline point 2 above).
    __atomic_store_n(&a->flags,    act->flags,    __ATOMIC_RELAXED);
    __atomic_store_n(&a->restorer, act->restorer, __ATOMIC_RELAXED);
    __atomic_store_n(&a->mask,     act->mask,     __ATOMIC_RELAXED);
    __atomic_store_n(&a->handler,  act->handler,  __ATOMIC_RELEASE);
    return true;
}

// exec's disposition reset, in place -- the table object SURVIVES.
//
// Zeroing is not an approximation of the POSIX reset, it IS the reset: by the
// note_handler rule twenty lines up, SIG_DFL is 0 and SIG_IGN is 1, so neither
// is an address, and a zeroed entry reads as all-default. viv_sigtab_of
// allocates with kzalloc, so the zeroed table is byte-identical to a fresh one.
//
// The object surviving is the POINT, not a convenience (#254). exec used to
// kfree the table and NULL the pointer while holding no lock, and `p->sigtab`
// is loaded and dereferenced from OTHER Procs' CPUs -- notes_post's SIG_IGN
// hook, notes_arm_intr_terminate_locked, and the ^Z fan's disposition gate all
// take an arbitrary Proc. That was a use-after-free read. Keeping one immortal
// table per Proc removes the dangling pointer instead of racing to guard it,
// which is what makes the reader set safe to GROW: a lock only protects the
// readers who remember to take it, and the third reader here was added by the
// same session that found the bug.
//
// PER-FIELD, never per-byte, and the width is a CONSTRUCTION (the access
// discipline above; main#243 F6): each field is one __atomic_store_n on an
// aligned u64, `handler` first. Because the readers are lock-free, that width
// is an ABI with them, not an implementation detail -- the byte loop this
// replaced was measured to compile to `sturh` halves under the kernel's flags,
// and the struct assignment that replaced THAT gave the right `stp` only
// because the compiler happened to emit it.
//
// What this does NOT promise, deliberately: a reader still sees an arbitrary
// MIX of pre- and post-reset entries. That is the POSIX exec-vs-signal race and
// is fine -- each entry it sees is one that was genuinely installed or the
// default. The guarantee is per-field integrity, not a snapshot.
void viv_sigtab_reset(struct viv_sigtab *tab) {
    if (!tab) return;
    for (u32 i = 0; i < (u32)VIV_SIGNOTE_COUNT; i++)
        sigact_st_default(&tab->act[i]);
    // Publish the zeroing rather than leaving it to whatever lock the caller
    // happens to take next. Free on this path, and it removes a question a
    // future reader would otherwise have to re-answer.
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

// execve's reset for the PHENOTYPE (POSIX execve(2); ARCH 7.6's fork/exec
// signal-state rule, 2026-08-17): CAUGHT rows go back to SIG_DFL -- the handler
// was an address in the OLD image and its flags/restorer/mask went with it --
// while SIG_IGN rows survive intact ("signals set to be ignored by the calling
// process image shall be set to be ignored by the new process image"). SIG_DFL
// rows are already default. Same in-place, per-8-byte-field discipline as
// viv_sigtab_reset: the object survives (#254), a reader sees whole fields.
void viv_sigtab_reset_caught(struct viv_sigtab *tab) {
    if (!tab) return;
    for (u32 i = 0; i < (u32)VIV_SIGNOTE_COUNT; i++) {
        struct viv_ksigaction *a = &tab->act[i];
        u64 h = sigact_ld_handler(a);
        if (h == VIV_SIG_DFL || h == VIV_SIG_IGN) continue;
        sigact_st_default(a);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

// fork's copy for the PHENOTYPE (POSIX fork(2); the same rule): the child gets
// its OWN table holding every one of the parent's dispositions, caught and
// ignored. Its own, not a shared pointer, because the table's whole safety
// argument is one immortal object per Proc read lock-free from other CPUs
// (#254) -- sharing would make one Proc's exec reset the other's dispositions
// and one Proc's proc_free dangle the other's pointer. A parent with no table
// is all-SIG_DFL: the child's stays NULL. The child is under construction and
// unpublished, so the plain store into child->sigtab needs no CAS (contrast
// viv_sigtab_of, which races peer threads); the parent's table is read once
// under an acquire load -- a concurrent rt_sigaction on the parent may leave
// the child with either the old or the new row, the latitude POSIX gives a
// fork racing sigaction. Returns 0, or -1 on OOM with child->sigtab left NULL
// (proc_free's kfree(NULL) is a clean no-op).
int viv_sigtab_clone_into(struct Proc *child, const struct Proc *parent) {
    if (!child || !parent) return -1;
    const struct viv_sigtab *src =
        __atomic_load_n(&parent->sigtab, __ATOMIC_ACQUIRE);
    if (!src) return 0;
    struct viv_sigtab *dst = (struct viv_sigtab *)kzalloc(sizeof(*dst), 0);
    if (!dst) return -1;
    for (u32 i = 0; i < (u32)VIV_SIGNOTE_COUNT; i++) dst->act[i] = src->act[i];
    __atomic_store_n(&child->sigtab, dst, __ATOMIC_RELEASE);
    return 0;
}

void vivarium_build_sigframe(struct viv_sigframe_head *out, u64 signum,
                             u64 sigmask, const u64 *regs31,
                             u64 sp, u64 pc, u64 pstate) {
    if (!out) return;

    // Zero EVERY byte first, pads included. The buffer is a kernel stack frame
    // and the copy-out hands it to a guest, so an unwritten gap is a kernel
    // memory disclosure (I-13) rather than untidiness.
    u8 *raw = (u8 *)out;
    for (u32 i = 0; i < (u32)sizeof(*out); i++) raw[i] = 0;

    out->info.si_signo = (s32)signum;
    out->info.si_errno = 0;
    out->info.si_code  = VIV_SI_KERNEL;
    // _sifields stays zero: a note carries a 16-byte name and one u32 arg, so
    // si_pid / si_uid / si_status have no source. SI_KERNEL is the si_code that
    // claims nothing about them.

    out->uc.uc_flags = 0;
    out->uc.uc_link  = 0;

    // stack_t: SS_DISABLE. sigaltstack(2) is an explicit ENOSYS row, so there
    // is never an alternate stack to describe, and reporting one would be the
    // stored lie the whole signal surface is written to avoid.
    out->uc.ss_sp    = 0;
    out->uc.ss_flags = 2;                          // SS_DISABLE
    out->uc.ss_size  = 0;

    out->uc.uc_sigmask[0] = sigmask;               // words 1..15 stay zero

    // The interrupted context, field-for-field. This is what makes the frame
    // worth writing: a crash reporter reading uc_mcontext.pc gets the real
    // faulting PC even though writing to it changes nothing.
    out->uc.uc_mcontext.fault_address = 0;
    if (regs31)
        for (u32 i = 0; i < 31; i++) out->uc.uc_mcontext.regs[i] = regs31[i];
    out->uc.uc_mcontext.sp     = sp;
    out->uc.uc_mcontext.pc     = pc;
    out->uc.uc_mcontext.pstate = pstate;

    // The _aarch64_ctx chain terminator: magic 0, size 0. Already zero from the
    // wipe above, but written explicitly because it is a PROTOCOL value the
    // guest parses, not incidental padding -- a reader deleting the wipe must
    // see that these two must survive.
    out->uc.uc_mcontext.end_magic = 0;
    out->uc.uc_mcontext.end_size  = 0;
}

enum viv_verdict vivarium_sigaction_decide(u64 signum, u64 handler, u64 flags,
                                           u64 sigsetsize) {
    // Linux checks sizeof(sigset_t) FIRST and answers EINVAL; musl passes
    // _NSIG/8 == 8 at every call site. A different size means a caller whose
    // sigset layout we have not reasoned about.
    if (sigsetsize != 8) return VIV_FORWARD;

    // 1..64. Linux's own check is `sig-1u >= _NSIG-1`, i.e. the same range.
    if (signum < 1 || signum > VIV_NSIG) return VIV_FORWARD;

    // SIGKILL and SIGSTOP are uncatchable by POSIX and Linux answers EINVAL for
    // both. Declining is the honest answer: pretending to install a handler for
    // SIGKILL would be a stored lie, and I-19's N-4 makes `kill` non-catchable
    // on the Thylacine side too, so the two agree.
    if (signum == VIV_SIGKILL || signum == VIV_SIGSTOP) return VIV_FORWARD;

    // SIG_ERR is POSIX-invalid. Without this the recorded handler is -1 and a
    // later delivery jumps there -- the pouch layer's F11 audit close found the
    // identical hole in the userspace bootstrap.
    if (handler == VIV_SIG_ERR) return VIV_FORWARD;

    // No note carries this signal, so a disposition for it could be recorded
    // but never acted on. Recording it would be storing a lie; forwarding says
    // "this system cannot do that", which is true.
    if (viv_signal_note(signum) == VIV_SIGNOTE_NONE) return VIV_FORWARD;

    // THE ARGUMENT DOMAIN. Installing a REAL handler requires SA_RESTORER,
    // because the guest's own trampoline is how the handler returns -- musl
    // always supplies one (measured: it compiles with -D_XOPEN_SOURCE=700,
    // which exposes SA_RESTORER, so sigaction.c fills ksa.restorer with
    // __restore_rt). Thylacine will not synthesise a substitute: the only
    // alternative is a vDSO sigreturn trampoline, and the vDSO page is
    // deliberately RO+XN (I-12/I-13). Weakening an audited surface to serve a
    // compatibility row is not a trade this arc makes.
    //
    // SIG_DFL and SIG_IGN need no trampoline -- nothing returns from them -- so
    // they are admitted without the flag. That matters: `signal(SIGPIPE,
    // SIG_IGN)` is the single most common signal call in real programs, and it
    // works here with no handler machinery at all.
    if (handler != VIV_SIG_DFL && handler != VIV_SIG_IGN &&
        !(flags & VIV_SA_RESTORER))
        return VIV_FORWARD;

    // A disposition is per-SIGNAL; a note carries no signal identity. Where two
    // signals share a note the request is not approximable, it is unanswerable
    // -- see viv_signal_owns_note_exclusively. SIG_DFL is exempt: it is the
    // state every signal is already in, so recording it changes nothing and can
    // disagree with nothing.
    if (handler != VIV_SIG_DFL && !viv_signal_owns_note_exclusively(signum))
        return VIV_FORWARD;

    // Same exemption, same reason: SIG_DFL for an undeliverable note is already
    // true (an EL0 fault terminates), so admitting it stores no lie. Catching or
    // ignoring one would.
    if (handler != VIV_SIG_DFL && !viv_signote_is_deliverable(viv_signal_note(signum)))
        return VIV_FORWARD;

    // SIGCHLD + SIG_IGN is Linux's AUTO-REAP, not "ignore" (POSIX leaves it
    // unspecified; Linux made it the no-zombies idiom and programs use it for
    // exactly that). Thylacine reaps only through wait_pid. Admitting this by
    // its surface meaning would leave a guest with zombies it believes are
    // gone -- the failure V-6a's own comment calls a stored lie.
    if (signum == VIV_SIGCHLD && handler == VIV_SIG_IGN) return VIV_FORWARD;

    // V-6b's handler-declines line stood HERE and is GONE at V-6c: the Tier-1
    // frame exists, so a real handler is now installable and will actually run.
    // The SA_RESTORER gate above is what remains, and it is permanent rather
    // than provisional -- the guest supplying its own return trampoline is the
    // reason Thylacine needs no executable vDSO page.
    return VIV_TRANSLATED;
}

enum viv_verdict vivarium_sigprocmask_decide(u64 how, u64 sigsetsize) {
    // A caller may pass `set == NULL` to read the mask without setting it, so
    // `how` is only meaningful when setting -- but Linux validates it
    // regardless, and so do we: a bad `how` is EINVAL on Linux, and admitting
    // it here would mean choosing an arbitrary interpretation.
    if (how != VIV_SIG_BLOCK && how != VIV_SIG_UNBLOCK && how != VIV_SIG_SETMASK)
        return VIV_FORWARD;
    if (sigsetsize != 8) return VIV_FORWARD;
    return VIV_TRANSLATED;
}

u64 viv_sigset_to_notemask(u64 sigset, const struct viv_notebit_map *m) {
    if (!m) return 0;

    u64 out = 0;
    // Signals are 1-based; bit (n-1) of the sigset word names signal n.
    for (u64 sig = 1; sig <= VIV_NSIG; sig++) {
        if (!(sigset & (1ULL << (sig - 1)))) continue;

        switch (viv_signal_note(sig)) {
        case VIV_SIGNOTE_INTERRUPT:  out |= 1ULL << m->interrupt;  break;
        case VIV_SIGNOTE_PIPE:       out |= 1ULL << m->pipe;       break;
        case VIV_SIGNOTE_CHILD_EXIT: out |= 1ULL << m->child_exit; break;

        case VIV_SIGNOTE_SNARE_SEGV:
        case VIV_SIGNOTE_SNARE_BUS:
        case VIV_SIGNOTE_SNARE_ILL:
        case VIV_SIGNOTE_SNARE_FPE:  out |= 1ULL << m->snare;      break;

        case VIV_SIGNOTE_TTY_HUP:
        case VIV_SIGNOTE_TTY_QUIT:
        case VIV_SIGNOTE_TTY_WINCH:
        case VIV_SIGNOTE_TTY_SUSP:
        case VIV_SIGNOTE_TTY_CONT:   out |= 1ULL << m->tty;        break;

        // KILL is NEVER maskable. I-19's N-4 makes it non-catchable and
        // mask-bypassing on the Thylacine side, and POSIX says the same of
        // SIGKILL, so the two agree and there is nothing to translate. musl's
        // __block_all_sigs sets every bit including SIGKILL's; silently
        // dropping it here is what makes that call translatable at all.
        case VIV_SIGNOTE_KILL: break;

        // No note carries it, so blocking it is a no-op -- CONSISTENT rather
        // than lossy, because nothing can deliver it either. Declining the
        // whole call instead would refuse the wide masks musl routinely sends.
        case VIV_SIGNOTE_NONE: break;
        }
    }
    return out;
}

u64 viv_notemask_to_sigset(u64 notemask, const struct viv_notebit_map *m) {
    if (!m) return 0;

    // Built by asking, of each signal, "is the bit that would block YOU set?".
    // Running it in this direction is what makes the coarse tty bit report
    // honestly: five signals ask about the same bit and all five answer yes.
    u64 out = 0;
    for (u64 sig = 1; sig <= VIV_NSIG; sig++) {
        u64 bit;
        switch (viv_signal_note(sig)) {
        case VIV_SIGNOTE_INTERRUPT:  bit = 1ULL << m->interrupt;  break;
        case VIV_SIGNOTE_PIPE:       bit = 1ULL << m->pipe;       break;
        case VIV_SIGNOTE_CHILD_EXIT: bit = 1ULL << m->child_exit; break;

        case VIV_SIGNOTE_SNARE_SEGV:
        case VIV_SIGNOTE_SNARE_BUS:
        case VIV_SIGNOTE_SNARE_ILL:
        case VIV_SIGNOTE_SNARE_FPE:  bit = 1ULL << m->snare;      break;

        case VIV_SIGNOTE_TTY_HUP:
        case VIV_SIGNOTE_TTY_QUIT:
        case VIV_SIGNOTE_TTY_WINCH:
        case VIV_SIGNOTE_TTY_SUSP:
        case VIV_SIGNOTE_TTY_CONT:   bit = 1ULL << m->tty;        break;

        // SIGKILL is never blocked, so it is never reported blocked -- the
        // exact mirror of the forward direction dropping it. A guest that
        // blocks everything and reads it back correctly does NOT see SIGKILL.
        case VIV_SIGNOTE_KILL:
        case VIV_SIGNOTE_NONE:
        default:                     continue;
        }
        if (notemask & bit) out |= 1ULL << (sig - 1);
    }
    return out;
}

u64 vivarium_handler_mask(u64 note_mask, u64 sa_mask, u64 sa_flags, u64 signum,
                          const struct viv_notebit_map *m) {
    // Linux kernel/signal.c signal_delivered():
    //     sigorsets(&blocked, &current->blocked, &ka->sa.sa_mask);
    //     if (!(ka->sa.sa_flags & SA_NODEFER)) sigaddset(&blocked, sig);
    // Both additions go through the forward translation, so they inherit its
    // two properties: SIGKILL is dropped, and a tty-family signal blocks the
    // family (the coarseness the mask row already reports honestly).
    u64 out = note_mask | viv_sigset_to_notemask(sa_mask, m);
    if (!(sa_flags & VIV_SA_NODEFER) && signum >= 1 && signum <= VIV_NSIG)
        out |= viv_sigset_to_notemask(1ULL << (signum - 1), m);
    return out;
}

// =============================================================================
// REAPING -- the pure layer (LINEAGE L-6b). See docs/LINEAGE.md section 5.5.
// =============================================================================

enum viv_verdict vivarium_wait4_decide(u64 options, u64 rusage,
                                       struct viv_wait_opts *out) {
    if (!out) return VIV_FORWARD;               // fail closed
    out->nohang = out->untraced = out->continued = false;

    // rusage FIRST, because it is the cheapest refusal and the one least
    // entangled with the option word. A non-zero pointer means the caller
    // wants figures this kernel does not keep per-child; see the header for
    // why zeroing them would be worse than declining.
    if (rusage != 0) return VIV_FORWARD;

    // NARROWED TO 32 BITS, unlike vivarium_clone_decide. The difference is the
    // ABI, not a preference: Linux declares wait4's `options` as `int`, so the
    // high half carries no meaning and a caller that leaves x2 sign- or
    // zero-extended must be read identically. clone's `flags` is an
    // `unsigned long`, which is why THAT comparison is full-width.
    u32 opts = (u32)options;

    // THE ALLOW-LIST. Every bit outside it declines -- and the one that makes
    // this load-bearing rather than tidy is WEXITED (4), which is numerically
    // Thylacine's WAIT_CONTINUED. A mask-and-pass would hand a guest that
    // passed WEXITED an opt-in to continue-reports AND to the packed status
    // encoding, silently, with no error anywhere. Refusing the whole word is
    // what keeps that from being expressible.
    if (opts & ~VIV_WAIT_OPTS_ADMITTED) return VIV_FORWARD;

    // The map, bit by bit. Two of the three happen to be the identity; writing
    // them out anyway is deliberate, because the interesting property of this
    // function is that NOTHING here is a passthrough -- a reader checking the
    // WCONTINUED line should not have to wonder whether the other two were
    // handled by a different mechanism.
    out->nohang    = (opts & (u32)VIV_WNOHANG)    != 0;
    out->untraced  = (opts & (u32)VIV_WUNTRACED)  != 0;
    out->continued = (opts & (u32)VIV_WCONTINUED) != 0;

    // `pid` is absent from this signature on purpose: wait_pid_for's selectors
    // ARE Linux's (-1 / >0 / 0 / <-1), so there is one rule and it lives in
    // proc.h. A check here would be a second opinion about a correspondence
    // that is already exact.
    return VIV_TRANSLATED;
}

// -----------------------------------------------------------------------------
// The startup batch (#150). The pure halves; their uaccess shells are in
// syscall.c::viv_tier2.
// -----------------------------------------------------------------------------

enum viv_verdict vivarium_writev_decide(u64 iovcnt, u32 *out_count) {
    if (!out_count) return VIV_FORWARD;

    // Linux's third argument is an `int`, so the guest's own view of a value
    // with bit 31 set is NEGATIVE, and Linux answers EINVAL. Comparing the
    // register as u64 against UIO_MAXIOV rejects both that case and a genuinely
    // huge count in one test -- no sign-extension reasoning required, because
    // every value Linux would call negative is far above 1024 when read
    // unsigned.
    if (iovcnt > (u64)VIV_UIO_MAXIOV) return VIV_FORWARD;

    // Zero IS in domain. Linux looks the fd up before it looks at the array, so
    // writev(badfd, whatever, 0) is EBADF rather than 0 -- the shell reproduces
    // that by issuing a zero-length write instead of returning 0 here.
    *out_count = (u32)iovcnt;
    return VIV_TRANSLATED;
}

bool vivarium_writev_accumulate(u64 *total, u64 add) {
    if (!total) return false;

    // SSIZE_MAX. Linux rejects a writev whose lengths sum past it BEFORE
    // writing anything, and the reason is the return value: a total above
    // SSIZE_MAX cannot be reported without colliding with the negative-errno
    // band the caller checks. Testing the ROOM rather than the sum keeps the
    // addition itself from wrapping.
    const u64 ssize_max = 0x7FFFFFFFFFFFFFFFull;
    if (add > ssize_max - *total) return false;
    *total += add;
    return true;
}

// A tiny local copy so this file stays free of a string.h dependency it
// otherwise has no use for. Copies at most `cap - 1` bytes and always
// terminates; `out` is expected to be pre-zeroed by the caller.
static void viv_uts_set(char *out, const char *s) {
    u32 i = 0;
    while (s[i] != '\0' && i < (u32)(VIV_UTS_FIELD_LEN - 1)) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

void vivarium_uname_fill(struct viv_linux_utsname *out) {
    if (!out) return;

    // Zero the WHOLE struct first. This is an I-13 obligation, not tidiness:
    // the shell copies all 390 bytes to EL0, so every byte past each string's
    // terminator has to be a defined 0 rather than whatever the caller's kernel
    // stack happened to hold. Each field is then written over a known-zero
    // background, and viv_uts_set terminates within the field regardless.
    u8 *p = (u8 *)out;
    for (u32 i = 0; i < (u32)sizeof(*out); i++) p[i] = 0;

    // The values, and the argument for each one, are in vivarium.h next to the
    // struct -- deliberately, because WHAT this claims is the whole decision and
    // it belongs where a reader meets the type rather than buried in the filler.
    viv_uts_set(out->sysname,    "Linux");
    viv_uts_set(out->nodename,   "thylacine");
    viv_uts_set(out->release,    "4.4.0");
    viv_uts_set(out->version,    "#1 Thylacine VIVARIUM");
    viv_uts_set(out->machine,    "aarch64");
    viv_uts_set(out->domainname, "(none)");
}

u32 vivarium_map_uid(u32 principal_id) {
    // PRINCIPAL_SYSTEM is Thylacine's TCB identity; 0 is Unix's. Everything
    // else passes through, and cannot collide, because PRINCIPAL_INVALID == 0
    // means no real principal is ever 0 to begin with.
    return (principal_id == PRINCIPAL_SYSTEM) ? 0u : principal_id;
}

u32 vivarium_map_gid(u32 gid) {
    return (gid == GID_SYSTEM) ? 0u : gid;
}

bool vivarium_setid_is_noop(u32 requested, u32 current_mapped) {
    // Compared in the GUEST's number space on purpose: `current_mapped` is what
    // getuid() told it, and setuid(getuid()) is the exact call this exists to
    // let through. Comparing against the raw principal would refuse that call
    // for a PRINCIPAL_SYSTEM Proc -- the one case it most needs to serve.
    return requested == current_mapped;
}

// The Linux clockid_t VALUES clock_gettime's first argument carries. Argument
// values, not syscall numbers, so they live here beside their translator exactly
// as VIV_OMODE_* do -- not in the VIV_LINUX_* number enum in vivarium.h.
enum {
    VIV_CLOCK_REALTIME           = 0,
    VIV_CLOCK_MONOTONIC          = 1,
    VIV_CLOCK_PROCESS_CPUTIME_ID = 2,
    VIV_CLOCK_THREAD_CPUTIME_ID  = 3,
    VIV_CLOCK_MONOTONIC_RAW      = 4,
    VIV_CLOCK_REALTIME_COARSE    = 5,
    VIV_CLOCK_MONOTONIC_COARSE   = 6,
    VIV_CLOCK_BOOTTIME           = 7,
};

// clock_gettime(clk_id, tp): the clk_id map. PURE -- it maps the Linux clock id
// onto one of Thylacine's two clocks and nothing else; the shell in syscall.c
// does the validated write through the native SYS_CLOCK_GETTIME handler, whose
// timespec is byte-identical to Linux's, so the number map is the ONLY
// translation. Returns false for a clk_id this kernel has no clock for; the
// caller answers -EINVAL, which is also Linux's answer for a clk_id IT cannot
// serve -- the error semantics coincide on the unknown case even though the
// KNOWN sets differ (why this is T2, not a T1 renumber: a T1 row must be total
// over the argument domain, and this one is not -- the lseek precedent).
//
// Thylacine has exactly two clocks: T_CLOCK_REALTIME (wall ns) and
// T_CLOCK_MONOTONIC (ns since boot, raw CNTVCT, never backward). Each admitted
// Linux id is an individual CLAIM that one of those two satisfies its contract:
//
//   CLOCK_REALTIME        (0) -> REALTIME. Identical.
//   CLOCK_MONOTONIC       (1) -> MONOTONIC. Identical.
//   CLOCK_MONOTONIC_RAW   (4) -> MONOTONIC. Linux's RAW is monotonic WITHOUT the
//                                NTP slew; Thylacine performs no slewing, so its
//                                monotonic already IS raw. Exact, not approximate.
//   CLOCK_REALTIME_COARSE (5) -> REALTIME. COARSE requests a cheap last-tick
//                                value; returning the precise one is a valid
//                                refinement (more accurate, never wrong).
//   CLOCK_MONOTONIC_COARSE(6) -> MONOTONIC. Same refinement.
//   CLOCK_BOOTTIME        (7) -> MONOTONIC. BOOTTIME is monotonic INCLUDING
//                                suspend; a QEMU guest does not suspend and
//                                Thylacine's monotonic is ns-since-boot, so the
//                                two coincide here.
//
// DECLINED (-> the caller's -EINVAL, a served answer, not a "declined row"):
//   CLOCK_PROCESS_CPUTIME_ID (2), CLOCK_THREAD_CPUTIME_ID (3) -- per-process /
//   per-thread CPU time, which Thylacine does not expose. EINVAL is honest, and
//   a program that probes clock support tolerates it. Any other id is unknown
//   to both sides and is EINVAL on both.
bool vivarium_clock_gettime_map(u64 linux_clk_id, u64 *thyla_clk_id_out) {
    if (!thyla_clk_id_out) return false;
    switch (linux_clk_id) {
    case VIV_CLOCK_REALTIME:
    case VIV_CLOCK_REALTIME_COARSE:
        *thyla_clk_id_out = T_CLOCK_REALTIME;
        return true;
    case VIV_CLOCK_MONOTONIC:
    case VIV_CLOCK_MONOTONIC_RAW:
    case VIV_CLOCK_MONOTONIC_COARSE:
    case VIV_CLOCK_BOOTTIME:
        *thyla_clk_id_out = T_CLOCK_MONOTONIC;
        return true;
    default:
        return false;
    }
}
