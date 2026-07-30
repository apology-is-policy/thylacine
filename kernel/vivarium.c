// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// The contract, the admission rule, and why this file is PURE are all in
// <thylacine/vivarium.h>. Read that first; this file is the data plus a lookup.

#include <thylacine/vivarium.h>

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
    //
    // NOT YET LISTED, deliberately: rt_sigaction (134), rt_sigprocmask (135),
    // kill (129), tkill (130), tgkill (131). Their PURE translators exist below
    // and are unit-tested, but their shells do not, and a VIV_TIER2 row whose
    // shell is missing would be a table that DECLARES a capability the code does
    // not have -- `viv_tier2`'s default arm calls exactly that a "table/shell
    // disagreement" and fails closed. So the rows land with the shells, in the
    // same commit, and until then these numbers FORWARD like any unclassified
    // call. This mirrors V-2: the translation tables landed before V-1b gave
    // them a caller, by design (§6.19/§6.20).
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
    // SIGTERM SHARES `interrupt` with SIGINT at v1.0. Inherited from the pouch
    // mapping (POUCH-DESIGN §6.4) and a stated imprecision, not an oversight:
    // both default-terminate, so the observable difference is only which one a
    // handler sees -- and Tier 1 resolves that by recording the signum
    // alongside, so the handler is called with the number the guest sent.
    case VIV_SIGINT:   case VIV_SIGTERM:  return VIV_SIGNOTE_INTERRUPT;
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
