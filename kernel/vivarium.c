// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// The contract, the admission rule, and why this file is PURE are all in
// <thylacine/vivarium.h>. Read that first; this file is the data plus a lookup.

#include <thylacine/vivarium.h>

#include <thylacine/notes.h>            // V-6b: the canonical note-name literals
#include <thylacine/syscall.h>
#include <thylacine/types.h>

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
    { VIV_LINUX_EXIT_GROUP, SYS_EXIT_GROUP, 1 },
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
    { VIV_LINUX_OPENAT,     VIV_TIER2   },  // V-2b: vivarium_openat_decide/_build
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
    { VIV_LINUX_SENDTO,      VIV_FORWARD },
    { VIV_LINUX_RECVFROM,    VIV_FORWARD },
    //
    // Live decisions, not deferrals:
    //   socketpair    — AF_UNIX only in practice, and /srv has no
    //                   mint-a-connected-pair operation; a guest that wanted a
    //                   pipe should use pipe2.
    //   setsockopt /  — /net exposes no option surface at all. Answering
    //   getsockopt      "success" to a TCP_NODELAY the stack ignores is the
    //                   silent lie this tier exists to prevent; ENOSYS lets the
    //                   guest's own fallback run. (Linux libcs routinely
    //                   tolerate a failing setsockopt for tuning options.)
    //   sendmsg /     — scatter-gather plus control messages (SCM_RIGHTS is fd
    //   recvmsg         passing, which is I-4's domain, not a socket detail).
    { VIV_LINUX_SOCKETPAIR,  VIV_ENOSYS },
    { VIV_LINUX_SETSOCKOPT,  VIV_ENOSYS },
    { VIV_LINUX_GETSOCKOPT,  VIV_ENOSYS },
    { VIV_LINUX_SENDMSG,     VIV_ENOSYS },
    { VIV_LINUX_RECVMSG,     VIV_ENOSYS },
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
//   O_CLOEXEC   ACCEPTED AS A NO-OP, and this is a claim worth stating. The flag
//               requests "this fd must not survive exec". Thylacine has no
//               close-on-exec concept because it has nothing to opt out of: a
//               spawned child "inherits no Spoor handles" (syscall.h:327) and
//               SYS_SPAWN_WITH_FDS passes an EXPLICIT list. So the behaviour
//               O_CLOEXEC asks for is what we do unconditionally, for every fd.
//               (The converse -- a guest opening WITHOUT O_CLOEXEC and expecting
//               the fd to cross exec -- is NOT served, but that is a property of
//               the process model, identical whether or not we admit this bit.
//               It is a V-7 seam, not a flag-map question.)
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
#define VIV_OPENAT_ADMITTED                                                  \
    ((u32)(VIV_O_ACCMODE | VIV_O_TRUNC | VIV_O_PATH |                        \
           VIV_O_CLOEXEC | VIV_O_NOCTTY | VIV_O_LARGEFILE))

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
//                munmap failure this tier exists to refuse. Task #50 tracks the
//                userspace half; the kernel half wants a create-by-PATH syscall
//                that does not exist.
//   O_DIRECTORY  Requires the target BE a directory (Linux: ENOTDIR otherwise).
//                SYS_OPEN has no such check, so ignoring it turns an error into
//                a successful open of a regular file. The worst kind of wrong.
//   O_APPEND     Every write must seek to end. SYS_OPEN's omode mask has no
//                append bit; ignoring it silently corrupts a log writer.
//   O_NOFOLLOW   Thylacine's resolver has no symlinks at v1.0, so ignoring it is
//                harmless TODAY -- and would become wrong the moment symlinks
//                land, with nothing to catch it. Rejected on that basis: a flag
//                whose correctness depends on a feature being absent is a trap.
//   O_EXCL, O_NONBLOCK, O_SYNC/O_DSYNC, O_DIRECT, O_NOATIME, O_TMPFILE, O_ASYNC
//                each carry semantics with no SYS_OPEN counterpart.

enum viv_verdict vivarium_openat_decide(u64 dirfd, u64 flags,
                                        u64 *start_fd_out, u32 *omode_out) {
    // Fail toward the supervisor, never toward a dispatch (cf. vivarium_translate).
    if (!start_fd_out || !omode_out) return VIV_FORWARD;

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
        if (fl & VIV_O_TRUNC) omode |= VIV_OMODE_TRUNC;
    }

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
    return VIV_TRANSLATED;
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
// And the rejects, each because ignoring it would be a silent wrong answer:
//
//   AT_SYMLINK_NOFOLLOW  REJECTED -- and this is the one that costs reach, so
//                    the reasoning matters. It is what musl/glibc/Go compile
//                    `lstat()` into, so rejecting it forwards every lstat.
//                    SYS_STAT's own contract says "Symlinks do not exist at
//                    v1.0 (G11), so stat == lstat" (syscall.h:1615), which looks
//                    like a licence to admit it. It is not. That equivalence is
//                    scoped to v1.0 and holds only because the feature is
//                    ABSENT -- exactly the O_NOFOLLOW trap V-2b named, and the
//                    day symlinks land there is nothing in this file, or in a
//                    build, that would fail. Admitting it would mean every
//                    lstat() in every Linux guest silently reporting the TARGET
//                    instead of the link, with no tripwire. Forwarding costs a
//                    supervisor round trip; admitting costs correctness later.
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
#define VIV_FSTATAT_ADMITTED ((u32)VIV_AT_NO_AUTOMOUNT)

enum viv_verdict vivarium_fstatat_decide(u64 dirfd, u64 flags) {
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

    return VIV_TRANSLATED;
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
enum {
    VIV_AF_INET        = 2,
    VIV_AF_INET6       = 10,
    VIV_SOCK_STREAM    = 1,
    VIV_SOCK_DGRAM     = 2,
    VIV_SOCK_NONBLOCK  = 04000,
    VIV_SOCK_CLOEXEC   = 02000000,
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
            tab->s[i].fd         = fd;
            tab->s[i].proto      = (u8)proto;
            tab->s[i].state      = VIV_SOCK_FRESH;
            tab->s[i].n          = n;
            tab->s[i].bound_addr = 0;   // a fresh socket carries no bind
            tab->s[i].bound_port = 0;
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

void viv_socktab_drop(struct viv_socktab *tab, s32 fd) {
    struct viv_sock *e = viv_socktab_find(tab, fd);
    if (!e) return;
    // Clear the WHOLE entry, not just the state. A stale field left in a freed
    // slot would be visible to any future claim that forgets to set it -- and
    // the bind fields make that concrete: a recycled slot still carrying the
    // previous socket's port would let a listen() announce a port this socket
    // never asked for.
    e->fd         = -1;
    e->proto      = 0;
    e->state      = VIV_SOCK_FREE;
    e->n          = 0;
    e->bound_addr = 0;
    e->bound_port = 0;
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

    // The type word carries flags in its high bits. REFUSE them rather than
    // masking them off -- a guest that asked for SOCK_NONBLOCK and silently
    // received a blocking socket blocks where it expected EAGAIN.
    if (type & (u64)(VIV_SOCK_NONBLOCK | VIV_SOCK_CLOEXEC)) {
        *out_err = T_E_INVAL;
        return false;
    }

    enum viv_net_proto proto;
    switch (type) {
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
    // worth naming: its default is STOP, and Thylacine's kernel NDFLT-stop arm
    // is unbuilt (task #15, an ABI decision needing signoff). Reporting
    // "ignore" for it would be a lie in the opposite direction from the one
    // V-6b spent its time removing.
    default:
        return false;
    }
}

bool viv_sigtab_note_ignored(const struct viv_sigtab *tab,
                             enum viv_signote note) {
    if (!tab) return false;                       // no table == nothing ignored
    if ((u32)note >= VIV_SIGNOTE_COUNT) return false;
    return tab->act[(u32)note].handler == VIV_SIG_IGN;
}

bool viv_sigtab_note_handler(const struct viv_sigtab *tab,
                             enum viv_signote note,
                             struct viv_ksigaction *out) {
    if (!tab || !out) return false;
    if ((u32)note >= VIV_SIGNOTE_COUNT) return false;

    const struct viv_ksigaction *a = &tab->act[(u32)note];
    // SIG_DFL is 0 and SIG_IGN is 1 -- neither is an address to jump to. A
    // zeroed table therefore reads as all-default with no separate init.
    if (a->handler == VIV_SIG_DFL || a->handler == VIV_SIG_IGN) return false;

    *out = *a;
    return true;
}

bool viv_sigtab_set(struct viv_sigtab *tab, enum viv_signote note,
                    const struct viv_ksigaction *act) {
    if (!tab || !act) return false;
    if ((u32)note >= VIV_SIGNOTE_COUNT) return false;
    tab->act[(u32)note] = *act;
    return true;
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
