// VIVARIUM V-2 — the Linux syscall translation table (docs/VIVARIUM.md §4/§5.3).
//
// This header + kernel/vivarium.c are the in-kernel half of Option C (the
// hybrid, user-voted 2026-07-23): the Linux calls whose translation is TOTAL and
// STATELESS are decoded here by table; everything else forwards to the userspace
// supervisor or fails ENOSYS.
//
// WHY A TABLE AND NOT LOGIC. §4's whole defence of Option C is that "the
// in-kernel part is a TABLE, not logic — auditable by inspection, and it adds no
// new kernel *semantics*, only a decode." That is a real constraint on this file,
// not a description of it: `vivarium_translate` is PURE — it takes a syscall
// number and six argument words, and returns a verdict plus a rewritten call. It
// touches no Proc, no handle table, no user memory, no lock, and allocates
// nothing. It is unit-testable with zero kernel plumbing, and that is the point.
//
// THE ADMISSION RULE (§4, binding). A Linux call may occupy a TABLE row iff its
// translation is *total and stateless*: a pure renumber plus an argument-order or
// flag-bit mapping onto exactly one existing `sys_*_for_proc`, with **no new
// kernel state, no new error semantics, and no policy**. The instant a call needs
// state the kernel does not already own — socket tables, signal dispositions,
// /proc content, ioctl dispatch — it forwards.
//
// "TOTAL" IS THE WORD THAT DOES THE WORK, and it is easy to misread as "the
// arguments line up". It does not mean that. It means the translation is correct
// for EVERY input the Linux call accepts. The worked counterexample is `munmap`:
// Linux `munmap(addr, len)` and `SYS_BURROW_DETACH(vaddr, length)` take the same
// two words in the same order and look like a free row — but burrow_detach
// requires an EXACT VMA match ("no partial detach at v1.0", syscall.h:611) while
// Linux explicitly permits partial and multi-mapping unmaps. The renumber would
// therefore be silently wrong for a legal class of inputs, so `munmap` is NOT a
// table row. Arguments aligning is not semantics aligning; check the contract.
//
// WHAT IS DELIBERATELY ABSENT. Nothing here is wired into `syscall_dispatch`.
// V-1a landed `Proc.phenotype` but NOTHING can set it to PHENO_LINUX — verified:
// `exec.c` never touches the field, the only assignment in the tree is the rfork
// inherit, and `PHENO_LINUX` is referenced nowhere outside its own enum. So the
// dispatch branch would today be branching on a field that is provably always 0.
// The branch + the declaration that makes it reachable are V-1b, and the
// declaration's correct SHAPE depends on V-7's container object (§5.2: "the fused
// container+phenotype object is the right granularity"; every peer system except
// FreeBSD has the CONTAINER declare). Building the table first is the reversible
// half: a table is data and can be rewritten freely, whereas a syscall signature
// is append-only ABI.

#ifndef THYLACINE_VIVARIUM_H
#define THYLACINE_VIVARIUM_H

#include <thylacine/types.h>
// For `struct pollfd` + POLL_MAX_NFDS: the pselect6 translator's OUTPUT type is
// the native pollfd, so the ABI it converts INTO belongs in this header's view.
#include <thylacine/poll.h>
// For `spin_lock_t`: struct viv_socktab embeds the leaf lock that makes the
// per-Proc socket table thread-safe once N-3 admits peer threads.
#include <thylacine/spinlock.h>

// `struct t_stat` is forward-declared rather than pulled in from <syscall.h>.
// A pointer parameter needs no definition, and V-1b will make syscall.c the
// dispatcher that calls into here -- so keeping the dependency one-way now
// removes an include cycle before it can exist.
struct t_stat;

// A Linux aarch64 syscall carries at most 6 argument words (x0..x5).
#define VIV_NARGS 6

// The verdict of a translation attempt.
enum viv_verdict {
    // A TABLE row matched: dispatch `out->nr` with `out->args` natively.
    VIV_TRANSLATED = 0,

    // Total-and-stateless does not hold: the call needs state, policy, or
    // judgement the kernel does not already own. V-3's userspace supervisor
    // handles it. Until V-3 exists the caller's disposition is its own choice —
    // this function only classifies.
    VIV_FORWARD = 1,

    // No Thylacine counterpart exists at all (`brk`: the heap is Burrow-based,
    // there is no break pointer to move). Honest ENOSYS, not a silent lie.
    VIV_ENOSYS = 2,

    // A TIER-2 translator exists for this number: the call is admissible, but
    // the translation is not a renumber, so `vivarium_translate` cannot perform
    // it and leaves `out` untouched. The dispatcher must invoke the named
    // translator below. See "TIER 2" for why these are separate functions.
    VIV_TIER2 = 3,
};

// A translated call: the Thylacine syscall number + its argument vector.
struct viv_call {
    u64 nr;
    u64 args[VIV_NARGS];
};

// Translate one Linux aarch64 syscall.
//
// PURE: no Proc, no uaccess, no locks, no allocation, no globals touched. Safe to
// call from anywhere, including a unit test with a synthetic argument vector.
//
// `args_in` must point to VIV_NARGS words. `out` is written only when the return
// is VIV_TRANSLATED; on FORWARD/ENOSYS it is left untouched, so a caller that
// ignores the verdict cannot accidentally dispatch a garbage number.
//
// A NULL `args_in` or `out` yields VIV_ENOSYS (fail closed, never a dispatch).
enum viv_verdict vivarium_translate(u64 linux_nr, const u64 *args_in,
                                    struct viv_call *out);

// The Linux aarch64 numbers this table knows. Named so the tests assert against
// symbols rather than magic numbers, and so a future row addition is a one-line
// diff next to its number. (Linux's aarch64 table is stable ABI — these values
// are fixed for all time by the Linux kernel's own compatibility promise.)
enum {
    // THE fd-FREEING SET -- every row here can destroy a live fd, so every row
    // here owes the socktab a drop of the entry keyed on the number it frees.
    // Leaving that out is the sharpest bug this family can have: a freed index
    // whose (proto, N) survives is handed to the next fd-creating call, and a
    // later connect() then writes a dial verb to a STRANGER'S connection.
    //
    // The obligation is discharged in TWO DIFFERENT PLACES, and the difference
    // is not stylistic (#157):
    //   * `close` (57) pays it in the ENTRY HOOK in viv_linux_dispatch, before
    //     the switch. That is correct there because a close whose fd carries an
    //     entry ALWAYS proceeds -- an fd with a socktab entry is by construction
    //     a live fd, so the close cannot then be refused.
    //   * `dup3` (24) pays it INSIDE ITS SHELL, after every refusal and
    //     immediately before the install. It cannot use the hook: dup3 can be
    //     refused (bad flags, old == new, bad old) while `new` is a perfectly
    //     live socket, and an unconditional entry-time drop would destroy the
    //     guest's socket state on a call that failed.
    // A future member must pick the arm its refusal structure demands rather
    // than copying whichever is nearer; `close_range` (436) is still FORWARD and
    // still owes it. `dup` (23) is now TIER2 (git-remote-https' helper pipe): it
    // is fd-CREATING, not fd-freeing, so it owes no DROP -- but it declines a
    // socket SOURCE (ENOSYS, as dup3 does), since a dup that did not register the
    // new number would hand back an unrecognized socket fd. Its shell arm is
    // beside dup3's.
    VIV_LINUX_DUP         = 23,
    // dup3 (#157) owes the below-the-ceiling paragraph -- 24's native occupant
    // is SYS_SPAWN_WITH_CAPS(name_va, name_len, cap_mask), whose arity matches
    // exactly, so nothing about the shape refuses it and the argument has to be
    // about what is REACHED. A mis-declared native program spawning a child
    // instead reaches dup3(old = name_va, new = name_len, flags = cap_mask):
    // `name_va` is a user VA, which is enormous, so it is out of fd range and
    // the answer is EBADF before anything is touched. And even had both been
    // small integers, dup3 acts ONLY on the caller's own handle table and can
    // at most move a descriptor the Proc already holds -- it mints no object,
    // crosses no Proc boundary, and consults no capability. Its own fds, never
    // authority. (Note the direction: cap_mask lands in `flags`, where every
    // value outside {0, O_CLOEXEC} is EINVAL, so the realistic mis-declaration
    // is refused on the flags word alone.)
    VIV_LINUX_DUP3        = 24,
    VIV_LINUX_CLOSE_RANGE = 436,
    // ioctl (C2-k1b): terminal control (TC*/TIOC*). A TIER2 shell, never a
    // renumber, so its sub-ceiling number carries no native-collision risk --
    // viv_tier2 matches it in the Linux-number namespace and dispatches the
    // explicit ioctl shell; nothing runs a native handler with the caller's args.
    VIV_LINUX_IOCTL       = 29,

    VIV_LINUX_OPENAT     = 56,
    VIV_LINUX_CLOSE      = 57,
    VIV_LINUX_LSEEK      = 62,
    VIV_LINUX_READ       = 63,
    VIV_LINUX_WRITE      = 64,
    // pread64/pwrite64 (the git 6.27 clone arm): git's index-pack reads the
    // received pack via pread, so a clone FORWARDed here to ENOSYS. Same
    // (fd, buf, count, offset) shape as SYS_PREAD/SYS_PWRITE -> pure T1
    // renumbers (see g_viv_t1). Sub-ceiling, colliding with the native LOOM
    // pair (67=SYS_LOOM_REGISTER, 68=SYS_LOOM_ENTER); the collision argument
    // is the read/write renumbers' -- a renumber runs the native handler with
    // the caller's OWN args, damage-envelope-bounded (a mis-declared LOOM
    // caller's loom handle is not a RIGHT_WRITE Spoor, so SYS_PWRITE fails
    // clean; at worst it touches the caller's own file via its own fd rights).
    VIV_LINUX_PREAD64    = 67,
    VIV_LINUX_PWRITE64   = 68,
    VIV_LINUX_NEWFSTATAT = 79,
    VIV_LINUX_FSTAT      = 80,
    VIV_LINUX_BRK        = 214,
    VIV_LINUX_MUNMAP     = 215,
    VIV_LINUX_MMAP       = 222,
    VIV_LINUX_MPROTECT   = 226,
    VIV_LINUX_STATX      = 291,
    VIV_LINUX_EXIT_GROUP = 94,
    VIV_LINUX_EXIT       = 93,   // N-3: a musl THREAD exits via SYS_exit(93)
    VIV_LINUX_FUTEX      = 98,   // N-3: pthread mutex/cond/join wait+wake

    // The signal family (V-6, §6.22). Contiguous in Linux's aarch64 table.
    VIV_LINUX_RESTART_SYSCALL = 128,
    VIV_LINUX_KILL            = 129,
    VIV_LINUX_TKILL           = 130,
    VIV_LINUX_TGKILL          = 131,
    VIV_LINUX_SIGALTSTACK     = 132,
    VIV_LINUX_RT_SIGSUSPEND   = 133,
    VIV_LINUX_RT_SIGACTION    = 134,
    VIV_LINUX_RT_SIGPROCMASK  = 135,
    VIV_LINUX_RT_SIGPENDING   = 136,
    VIV_LINUX_RT_SIGTIMEDWAIT = 137,
    VIV_LINUX_RT_SIGQUEUEINFO = 138,
    VIV_LINUX_RT_SIGRETURN    = 139,

    // The socket family (V-5, section 5.5). Contiguous in Linux's aarch64
    // table; aarch64 has no socketcall multiplexer, so each is its own number.
    // All are above THE NATIVE CEILING (see below), so the collision re-check
    // the ARCH section 25.4 row mandates is discharged by construction for
    // every row here.
    VIV_LINUX_SOCKET      = 198,
    VIV_LINUX_SOCKETPAIR  = 199,
    VIV_LINUX_BIND        = 200,
    VIV_LINUX_LISTEN      = 201,
    VIV_LINUX_ACCEPT      = 202,
    VIV_LINUX_CONNECT     = 203,
    VIV_LINUX_GETSOCKNAME = 204,
    VIV_LINUX_GETPEERNAME = 205,
    VIV_LINUX_SENDTO      = 206,
    VIV_LINUX_RECVFROM    = 207,
    VIV_LINUX_SETSOCKOPT  = 208,
    VIV_LINUX_GETSOCKOPT  = 209,
    VIV_LINUX_SHUTDOWN    = 210,
    VIV_LINUX_SENDMSG     = 211,
    VIV_LINUX_RECVMSG     = 212,
    VIV_LINUX_ACCEPT4     = 242,

    // Readiness (V-5c, section 5.5.4). aarch64 has NO plain poll(2) or
    // select(2) -- the generic ABI dropped them -- so these two ARE the poll
    // family, and musl's poll()/select() are thin wrappers over them.
    //
    // THE COLLISION RE-CHECK, WHICH FINALLY HAS WORK TO DO. Every row above is
    // over the native ceiling, so the ARCH section 25.4 mandate was discharged
    // by construction. These two are NOT: 72 is SYS_GETPID and 73 is
    // SYS_GETUID. So the argument has to be made per number, and it still
    // holds -- but for a different reason:
    //
    //   * A PHENO_LINUX Proc CANNOT REACH A NATIVE NUMBER AT ALL. Every number
    //     it issues goes through vivarium_translate, and an unclassified one
    //     lands on FORWARD -> ENOSYS (viv_linux_dispatch). It never had getpid
    //     or getuid to lose, and adding these rows takes nothing away.
    //   * A NATIVE program MIS-DECLARED as PHENO_LINUX issuing native 72
    //     intending getpid now dispatches the pselect6 translator over
    //     getpid's (absent, hence garbage) arguments. The translator reads
    //     user memory the caller owns, bounds-checked, and polls fds the
    //     caller already holds -- so the worst outcome is EFAULT, a block, or
    //     (V-5d F4) a BOUNDED WRITE of up to three 32-byte fd_set results into
    //     addresses the caller's own registers named, which is still the
    //     caller's own address space and still confers nothing;
    //     never authority. A mis-declared Proc is comprehensively broken
    //     either way; I-43 is about what it can REACH, not whether it works.
    //
    // A future row below 100 owes the same paragraph. Do not reach for the
    // ceiling argument again without checking the number.
    VIV_LINUX_PSELECT6    = 72,
    VIV_LINUX_PPOLL       = 73,

    // Process creation (LINEAGE L-3d, docs/LINEAGE.md §7). Above the native
    // ceiling, so the collision re-check is discharged by construction -- and
    // this time the ceiling was CHECKED rather than quoted: it had moved from
    // 100 to 102 when SYS_EXECVE and SYS_RFORK landed at L-2a/L-3b, and four
    // separate comments still said 100 (one still said the pre-#97 "256,
    // sparsely"). That is why VIV_NATIVE_CEILING below exists as a symbol
    // rather than as a number repeated in prose.
    VIV_LINUX_CLONE       = 220,
    VIV_LINUX_EXECVE      = 221,
    VIV_LINUX_WAIT4       = 260,

    // The startup batch (#150) -- the set a real Linux binary issues between
    // _start and its first useful instruction. Measured, not guessed: this is
    // exactly the census `viv_report_unserved` printed the moment #149's loader
    // fix let Alpine's busybox execute, minus the two rows that are declined on
    // purpose (brk 214, mprotect 226).
    //
    // FOUR OF THEM ARE BELOW THE NATIVE CEILING, so each owes the per-number
    // collision paragraph the pselect6/ppoll block above demands -- and this is
    // the first time that obligation has had to be discharged for more than two
    // numbers at once. The first half of the argument is shared: a PHENO_LINUX
    // Proc cannot reach a native number AT ALL (every number it issues goes
    // through vivarium_translate; an unclassified one lands on FORWARD ->
    // ENOSYS), so it never had the native call to lose. The second half -- what
    // a NATIVE program MIS-DECLARED as PHENO_LINUX now reaches -- is per number:
    //
    //   17  vs SYS_SET_DUMPABLE(dumpable). getcwd would write the cwd into
    //       args[0], which is the dumpable flag: a VA of 0 or 1. That fails
    //       sys_validate_user_buf, so the outcome is ERANGE/EFAULT and no write.
    //   25  vs SYS_SPAWN_FULL(...). fcntl is ENOSYS below -- there is no shell
    //       to mis-dispatch INTO, so this row costs a mis-declared Proc exactly
    //       an ENOSYS.
    //   66  vs SYS_LOOM_SETUP(entries, params_va). writev would read args[1] as
    //       an iovec array and write to fd args[0]. Both are the caller's OWN --
    //       its own memory, bounds-checked, and an fd it already holds -- so the
    //       worst case is EFAULT/EBADF or a write of the caller's bytes to the
    //       caller's fd. Never authority.
    //   96  vs SYS_TTY_SET_FG(fd, pgid). set_tid_address would store args[0] --
    //       a small fd number -- as clear_child_tid. It is validated (4-byte
    //       aligned, under UACCESS_USER_VA_TOP), and the exit-time store through
    //       it is a uaccess_store_u32 with a fixup entry, so an unmapped VA 4
    //       faults into the fixup and is swallowed. The caller's own address
    //       space, and nothing else.
    //
    // The remaining seven are above the ceiling and discharge by construction.
    VIV_LINUX_GETCWD          = 17,
    VIV_LINUX_FCNTL           = 25,
    // pipe2 (#155) owes the same paragraph -- 59 is below the ceiling, and its
    // native occupant is SYS_WSTAT(fd, valid, mode, uid, gid, size). The mis-
    // declared native program is refused TWICE OVER, and the outer refusal is
    // the DOMAIN CHECK rather than a memory guard: `valid` is a bitmask of
    // T_WSTAT_{MODE,UID,GID,SIZE} == 0xF, so every legal wstat mask lands in
    // [1,15], and the pipe2 domain admits only {0, O_CLOEXEC == 0x80000}. No
    // wstat a native caller can legally make is even reachable past the decide.
    // Should a garbage `valid` of 0 slip through anyway, args[0] -- the fd index
    // wstat put there -- becomes the VA the fd pair is written to, which is page
    // zero, so the copy-out faults, both freshly-made fds are closed, and the
    // answer is EFAULT. The caller's own address space and its own two fds.
    VIV_LINUX_PIPE2           = 59,
    VIV_LINUX_READV           = 65,   // N-5: the served twin of WRITEV below (git protocol-v2 stateless-connect reads the helper response through readv)
    VIV_LINUX_WRITEV          = 66,
    VIV_LINUX_SET_TID_ADDRESS = 96,
    VIV_LINUX_SETGID          = 144,
    VIV_LINUX_SETUID          = 146,
    VIV_LINUX_UNAME           = 160,
    VIV_LINUX_GETPID          = 172,
    VIV_LINUX_GETPPID         = 173,
    VIV_LINUX_GETUID          = 174,
    VIV_LINUX_GETGID          = 176,
    VIV_LINUX_GETTID          = 178,   // N-3: per-Thread tid (getpid stays per-Proc)
    // C2-k2: session/process-group control for interactive job control (`viv sh`).
    // Delegated to the native cores 89-92 (arities match). getpgid/getsid are
    // pure renumbers; setsid/setpgid are shells that remap the native T_E_ACCES
    // "EPERM contour" to the Linux EPERM a guest's errno check expects.
    VIV_LINUX_SETPGID         = 154,
    VIV_LINUX_GETPGID         = 155,
    VIV_LINUX_GETSID          = 156,
    VIV_LINUX_SETSID          = 157,

    // The time family. Both above the native ceiling (113, 169 > the highest
    // native syscall), so collision-free by construction -- their collision
    // re-check is the ceiling argument, discharged by the static_asserts in
    // vivarium.c beside restart_syscall/socket/clone/execve/wait4, not a
    // per-number one. These are the calls a libc's timeout path issues; without
    // them curl/git/TLS cannot bound a wait, and busybox `date` reads 1970.
    VIV_LINUX_CLOCK_GETTIME   = 113,
    VIV_LINUX_GETTIMEOFDAY    = 169,

    // The path-mutation family (#50; VIVARIUM.md section 6.24). Three of the
    // four are BELOW the native ceiling, so each owes the per-number collision
    // paragraph the startup-batch block above demands. The shared first half
    // is unchanged: a PHENO_LINUX Proc cannot reach a native number at all.
    // The second half -- what a NATIVE program MIS-DECLARED as PHENO_LINUX now
    // reaches -- turns on one fact for all three: every shell's decide admits
    // ONLY dirfd == AT_FDCWD (-100 as s32), and no legal native argument at
    // these numbers has that shape.
    //
    //   34  vs SYS_WALK_OPEN(spoor_fd, ...). mkdirat reads args[0] as dirfd;
    //       a native spoor_fd is a small non-negative index and the FROM_ROOT
    //       sentinel is -1, neither -100 -> every legal native call FORWARDs
    //       to ENOSYS. Nothing is reached.
    //   35  vs SYS_CHROOT(spoor_fd). Same gate, same shapes, same ENOSYS.
    //   38  vs SYS_BURROW_DETACH(vaddr, length). renameat requires BOTH
    //       args[0] and args[2] == -100. args[0] is a mapped user VA (never
    //       0x...FFFFFF9C-shaped in the attach window) and args[2] is a stale
    //       register; if both somehow read -100 the shell resolves args[1] and
    //       args[3] as path VAs in the caller's OWN memory and renames within
    //       the caller's OWN namespace under its OWN A-2d identity -- the
    //       pipe2-row damage envelope (the caller's own things), never
    //       authority.
    //
    //   276 (renameat2) is above the ceiling: collision-free by construction,
    //       asserted beside the socket/time rows in vivarium.c.
    VIV_LINUX_MKDIRAT   = 34,
    VIV_LINUX_UNLINKAT  = 35,
    VIV_LINUX_RENAMEAT  = 38,
    VIV_LINUX_RENAMEAT2 = 276,

    // The directory-read + durability rows (the getdents64/fsync chunk;
    // VIVARIUM.md section 6.25). All three are BELOW the native ceiling and
    // all three are FD-BASED -- no AT_FDCWD gate exists to refuse a
    // mis-declared native caller on shape, so each per-number paragraph rests
    // on the DAMAGE ENVELOPE instead (the pipe2-row standard: the caller's
    // own things, never authority). The shared first half is unchanged: a
    // PHENO_LINUX Proc cannot reach a native number at all.
    //
    //   61  vs SYS_CAP_GRANT_CLEARANCE. getdents64 reads args[0] as an fd:
    //       the shell looks it up in the CALLER'S OWN handle table
    //       (RIGHT_READ KOBJ_SPOOR) and either answers EBADF/ENOTDIR or
    //       enumerates a directory the caller already holds open, writing
    //       into the caller's OWN buffer VA. Enumeration of an already-held
    //       handle mints nothing and crosses no boundary.
    //   82  vs SYS_WEFT_MAP(data_fd, hint_va). fsync reads args[0] as an fd
    //       and flushes a file the caller already holds RIGHT_WRITE on --
    //       a durability barrier on the caller's own object; hint_va is
    //       never read (the shell passes an explicit datasync=0).
    //   83  vs SYS_BURROW_ATTACH_LAZY(length). fdatasync likewise: a length
    //       that happens to collide with a small fd index at most flushes
    //       the caller's own file; anything else is EBADF.
    VIV_LINUX_GETDENTS64 = 61,
    VIV_LINUX_FSYNC      = 82,
    VIV_LINUX_FDATASYNC  = 83,

    // faccessat (the git-under-VIVARIUM chunk; VIVARIUM.md section 6.26). The
    // RAW 3-arg syscall -- faccessat(dirfd, pathname, mode) -- NOT the 4-arg
    // faccessat2(439): musl's access() and its faccessat(...,flags=0) both
    // issue this number with no flags word, so args[3] does not exist and is
    // never read. git's git_config_system() calls access(R_OK) on
    // /etc/gitconfig and treats any errno but ENOENT as FATAL, so an
    // untranslated FORWARD->ENOSYS aborts `git version` before it prints --
    // this is the row that makes git run at all.
    //
    // 48 is BELOW the native ceiling, so it owes the per-number collision
    // paragraph. It carries the mkdirat family's gate, not the getdents64
    // family's damage-envelope one, because it HAS an AT_FDCWD gate:
    //   48  vs SYS_NOTE_MASK(new_mask, old_mask_out_va). faccessat's decide
    //       admits ONLY dirfd == AT_FDCWD (-100 as s32). new_mask is a small
    //       non-negative note bitfield -- never the -100 sentinel -- so a
    //       native NOTE_MASK caller mis-declared PHENO_LINUX FORWARDs to
    //       ENOSYS on shape, reaching nothing. Identical to 34/35's argument.
    VIV_LINUX_FACCESSAT  = 48,

    // chdir (the git chunk). `cd` in the container and git's own chdir into
    // each repo it touches -- without it `git init repo` cannot enter the tree
    // it just made. The native SYS_CHDIR reads + validates the path itself, so
    // the shell only measures the length Linux leaves implicit and delegates.
    //
    // 49 is BELOW the ceiling and FD-less (no AT_FDCWD gate to refuse a
    // mis-declared native caller on shape), so its collision argument is the
    // getdents64 family's DAMAGE ENVELOPE, not a shape gate:
    //   49  vs SYS_SPAWN_FULL_ARGV(args_va, ...). chdir reads args[0] as a
    //       path VA in the CALLER'S OWN memory and, at most, moves the
    //       CALLER'S OWN cwd (or fails) under its OWN A-2d identity. A native
    //       spawn-args pointer read as a path resolves to garbage and fails;
    //       nothing is spawned, no authority crosses -- the pipe2-row envelope
    //       (the caller's own things, never authority).
    VIV_LINUX_CHDIR      = 49,

    // fchmodat (the git chunk). git's git_config_set copies the original
    // config file's permission bits onto the lockfile before the rename, via
    // chmod -- and treats that chmod FAILING as a config-write failure, so
    // `git init` cannot write core.filemode without it. The shell opens the
    // path O_PATH (chmod requires OWNERSHIP, never read, so the perm_check-
    // exempt navigation handle is correct) and applies the mode through the
    // audited sys_wstat_for_proc, whose perm_wstat_check IS the POSIX
    // owner-or-CAP gate.
    //
    // 53 is BELOW the ceiling but HAS an AT_FDCWD gate (it shares
    // vivarium_faccessat_decide), so its collision argument is the shape one,
    // not the damage envelope:
    //   53  vs SYS_PIVOT_ROOT(new_root_fd). fchmodat's decide admits ONLY
    //       dirfd == AT_FDCWD (-100 as s32). new_root_fd is a small
    //       non-negative Spoor fd, never the -100 sentinel, so a native
    //       PIVOT_ROOT caller mis-declared PHENO_LINUX FORWARDs to ENOSYS on
    //       shape. Identical to 48/34/35's argument.
    VIV_LINUX_FCHMODAT   = 53,

    // readlinkat (the git chunk). git's path canonicalization (real_path)
    // readlinks components while resolving the repo path, and treats an
    // untranslated FORWARD->ENOSYS as fatal -- `git init` dies at it before it
    // ever reaches the config write. The shell resolves the path NOFOLLOW (the
    // link itself is the quarry) and copies its target out via the Dev's
    // .readlink slot; a non-symlink answers EINVAL, the POSIX contour that lets
    // git's resolver treat the component as a plain file.
    //
    // 78 is BELOW the ceiling but shares faccessat's AT_FDCWD gate, so its
    // collision argument is the shape one:
    //   78  vs SYS_PCI_INFO(handle, info_va). readlinkat's decide admits ONLY
    //       dirfd == AT_FDCWD (-100 as s32). A native PCI handle is a small
    //       non-negative index, never -100, so a mis-declared PCI_INFO caller
    //       FORWARDs to ENOSYS on shape. Identical to 48/53's argument.
    VIV_LINUX_READLINKAT = 78,

    // geteuid/getegid. Thylacine carries ONE principal per Proc (no real vs
    // effective split -- I-22: authority is the capability set, not a uid), so
    // each is the exact twin of its getuid/getgid sibling (174/176) and maps
    // through the SAME vivarium_map_uid/gid. ash reads geteuid to choose its
    // prompt char and git reads it for "am I root" checks; both were FORWARDing
    // to ENOSYS. 175 and 177 are ABOVE the native ceiling -> collision-free by
    // construction, asserted beside the time/socket rows in vivarium.c.
    VIV_LINUX_GETEUID    = 175,
    VIV_LINUX_GETEGID    = 177,

    // getrandom (the git chunk). git draws random bytes to name its temporary
    // object files; without it `git add` cannot create a temp file
    // ("unable to get random bytes for temporary file"). The native
    // SYS_GETRANDOM has the identical (buf, buflen, flags) shape and does its
    // own buffer validation + copy-out. It gates on CAP_CSPRNG_READ, and that
    // gate is KEPT under I-43: a phenotype confers Linux's numbering and
    // semantics, never authority -- the CSPRNG capability stays required, so a
    // container that draws entropy must be granted it (the git-probe gate does).
    // 278 is ABOVE the native ceiling -> collision-free by construction,
    // asserted beside the time/socket rows in vivarium.c.
    VIV_LINUX_GETRANDOM  = 278,
};

// The highest ASSIGNED native Thylacine syscall number. Every vivarium row
// above this is free of collision by construction; the two rows below it
// (pselect6 72, ppoll 73) carry their own per-number argument, above.
//
// THE OBLIGATION, and the reason this is a symbol: a new native syscall above
// the ceiling makes the ceiling argument stop holding for every row at or below
// the new value, SILENTLY. Bumping this constant is therefore part of adding a
// syscall, and the `_Static_assert` in vivarium.c pins it to the current top's
// identity so a renumber of that number cannot drift unnoticed. (It cannot catch
// a NEW higher number on its own -- C has no max-over-an-enum -- so the rows that
// depend on the ceiling assert against it individually there.)
//
// Stated ONCE, deliberately. It was previously written out as a literal in four
// places and was stale in all four -- and then went stale a fifth time HERE:
// pinned to SYS_RFORK (105), it missed the Warp arc's SYS_DMA_CREATE_GPU_BO (106)
// and SYS_BURROW_FROM_HOSTMEM (107) landing above it. Re-pinned to 107; #50
// then landed SYS_OPEN_CREATE (108) and moved the ceiling IN THE SAME COMMIT --
// the "add a syscall includes move the ceiling" obligation discharged the way
// the vivarium.c assert's own comment demands. Now 108, the true top.
#define VIV_NATIVE_CEILING 108

// -----------------------------------------------------------------------------
// TIER 2 — translators (V-2b).
//
// A T1 row is a renumber, so one table plus one loop serves every row. A T2 call
// is a real translation — a flag map, an argument reshape, a struct conversion —
// so each gets its own named function. They are still PURE, and that is a
// deliberate constraint, not an accident of what happened to be easy:
//
//   * `openat` needs the path's LENGTH, which lives in user memory. Rather than
//     let uaccess into this file, the measurement is HOISTED OUT to the caller:
//     `vivarium_openat_decide` makes the whole decision without touching memory,
//     and `vivarium_openat_build` assembles the call from a length the caller
//     measured. The part that needs a kernel is a strnlen; the part that needs
//     REVIEW is pure and unit-tested.
//   * `fstat` converts an 88-byte `struct t_stat` into the 128-byte Linux
//     aarch64 `struct stat`. Both structs are plain data, so the conversion is
//     pure; the shell around it (`spoor_stat_native` into a kernel t_stat, then
//     one 128-byte copy-out) touches no translation logic at all.
//
// WHY THE DECIDE/BUILD SPLIT IS NOT CEREMONY. Measuring the path is a user-memory
// read that can fault. Doing it before knowing whether the call is even
// translatable would (a) waste the read for every forwarded call and (b) let a
// call that we are going to hand to the supervisor anyway take a fault HERE,
// inside the kernel's fast path, on a buffer the supervisor would have validated
// itself. Deciding first and measuring second is the correct order, so the API
// is shaped to make the wrong order awkward.
//
// THE ARGUMENT DOMAIN — a refinement of §4, and the important part of V-2b.
// §4 admits "a pure renumber plus an argument-order/flag-bit mapping". A flag map
// is inherently PARTIAL: `openat` accepts flags (O_CREAT, O_DIRECTORY, O_APPEND)
// that SYS_OPEN has no way to honour. So a T2 row is admitted over a **stated
// argument domain**, and a call outside that domain FORWARDS. This is not a
// loosening of "total" — it is stricter in practice, because it replaces
// "openat is a table row" (which §4's illustrative list implies) with a
// per-call check. The soundness property is the one that matters:
//
//     the translator NEVER silently mistranslates; it either produces an
//     exactly-equivalent call or declines.
//
// Declining is always safe (the supervisor is strictly more capable). Accepting
// a flag we cannot honour is the failure mode this whole tier exists to avoid.
// -----------------------------------------------------------------------------

// Linux aarch64 open flags, in OCTAL so they can be diffed line-for-line against
// `third_party/musl/arch/aarch64/bits/fcntl.h` (they were taken from it, not from
// memory). O_RDONLY/WRONLY/RDWR come from musl's generic `include/fcntl.h`.
enum {
    VIV_O_RDONLY    = 00,
    VIV_O_WRONLY    = 01,
    VIV_O_RDWR      = 02,
    VIV_O_ACCMODE   = 03,

    VIV_O_CREAT     = 0100,
    VIV_O_EXCL      = 0200,
    VIV_O_NOCTTY    = 0400,
    VIV_O_TRUNC     = 01000,
    VIV_O_APPEND    = 02000,
    VIV_O_NONBLOCK  = 04000,
    VIV_O_DSYNC     = 010000,
    VIV_O_ASYNC     = 020000,
    VIV_O_DIRECTORY = 040000,
    VIV_O_NOFOLLOW  = 0100000,
    VIV_O_DIRECT    = 0200000,
    VIV_O_LARGEFILE = 0400000,
    VIV_O_NOATIME   = 01000000,
    VIV_O_CLOEXEC   = 02000000,
    VIV_O_SYNC      = 04010000,
    VIV_O_PATH      = 010000000,
    VIV_O_TMPFILE   = 020040000,
};

// Linux's "resolve relative to the process cwd" dirfd (musl `fcntl.h`: -100).
// Compared as a SIGNED 32-BIT value, because Linux passes `dirfd` as an `int`:
// a caller may leave x0 either sign-extended (0xFFFFFFFFFFFFFF9C) or merely
// zero-extended (0x00000000FFFFFF9C), and both must be recognised.
#define VIV_AT_FDCWD (-100)

// The file-TYPE field of a Linux `mode_t` (musl `sys/stat.h` S_IFMT). Distinct
// in kind from the 07000 setuid/sgid/sticky field, and the distinction decides
// the mode gates in the create decides: POSIX and Linux both define the file
// type on `openat`/`mkdirat` as determined BY THE CALL, so these bits are
// ignored on that argument -- Linux masks them and proceeds. Callers pass them
// routinely: busybox `tar` hands `file_header->mode` straight through, so a
// directory arrives as S_IFDIR|0755 and a regular file as S_IFREG|0644.
// Stripping them is therefore EXACT (it discards a field with no meaning here),
// where stripping 07000 would be a lie (it would record less authority than the
// caller asked for) -- which is why one is masked and the other declines.
#define VIV_S_IFMT 0170000u

// The `flags` word the *at() family carries (musl `include/fcntl.h`). Distinct
// from the O_* space above: these qualify the RESOLUTION, not the open.
enum {
    VIV_AT_SYMLINK_NOFOLLOW = 0x100,
    VIV_AT_REMOVEDIR        = 0x200,
    VIV_AT_SYMLINK_FOLLOW   = 0x400,
    VIV_AT_NO_AUTOMOUNT     = 0x800,
    VIV_AT_EMPTY_PATH       = 0x1000,
};

// Decide whether an `openat` is inside the translatable domain, and compute the
// two rewritten arguments. PURE — no user memory, no Proc, no locks.
//
//   `dirfd` / `flags` are the raw x0 / x2 register values; only their low 32
//   bits are significant (both are `int` in the Linux ABI).
//
// Returns VIV_TRANSLATED with *start_fd_out, *omode_out and *cloexec_out set, or
// VIV_FORWARD (outputs untouched) for anything outside the domain. Never ENOSYS:
// `openat` exists — an out-of-domain call is one the supervisor should serve,
// not one to deny.
//
// #151: `cloexec_out` reports whether O_CLOEXEC was asked for. It is a THIRD
// output rather than a bit folded into the omode because it is not part of the
// SYS_OPEN call at all -- the flag belongs to the resulting DESCRIPTOR, so the
// shell sets it after the open succeeds. Until #151 this bit was admitted and
// discarded, on a rationale ("Thylacine has nothing to opt out of") that was
// true when written and was voided by LINEAGE: execve now preserves the handle
// table and fork copies it, so an fd without the flag really does cross exec.
//
// getdents64 chunk: `dir_required_out` reports O_DIRECTORY -- the target MUST
// be a directory (Linux: ENOTDIR otherwise; one of the three flags O_PATH
// does not ignore). SYS_OPEN has no such check, so the flag is a FOURTH
// output the SHELL enforces as a postcondition on the minted handle (its
// Spoor's own qid answers QTDIR with no extra RPC; a non-directory closes the
// fd and answers ENOTDIR). It cannot ride the omode (no SYS_OPEN bit exists)
// and translating it as an ignore would turn Linux's error into a successful
// open of a regular file -- the V-2b reject this output retires. musl's
// opendir is the forcing caller: open(name, O_RDONLY|O_CLOEXEC|O_DIRECTORY)
// gates every getdents64.
enum viv_verdict vivarium_openat_decide(u64 dirfd, u64 flags,
                                        u64 *start_fd_out, u32 *omode_out,
                                        bool *cloexec_out,
                                        bool *dir_required_out);

// Assemble the SYS_OPEN call from a decision plus a caller-measured path length.
// Trivial by design — its value is that SYS_OPEN's argument ORDER is stated in
// exactly one place, and that place is covered by a test.
void vivarium_openat_build(u64 start_fd, u64 path_va, u32 path_len, u32 omode,
                           struct viv_call *out);

// #50 (VIVARIUM.md section 6.24): the O_CREAT domain of `openat`, routed by
// the shell when O_CREAT is set WITHOUT O_PATH (Linux's O_PATH ignores every
// flag but CLOEXEC/DIRECTORY/NOFOLLOW, O_CREAT included, so an O_PATH open
// stays on the plain decide regardless of a CREAT bit). PURE. The admitted
// domain is the plain decide's admitted set minus O_PATH, plus O_CREAT and
// O_EXCL; `mode` admits the low-9 permission bits only (a setuid/sgid/sticky
// bit declines to the supervisor, census-visible). Emits the SYS_OPEN_CREATE
// omode (access + OTRUNC + NOFOLLOW + OEXCL-for-O_EXCL) and perm (mode&0777);
// dirfd admits AT_FDCWD only, as the plain row. Never ENOSYS -- out-of-domain
// is the supervisor's to serve.
enum viv_verdict vivarium_openat_create_decide(u64 dirfd, u64 flags, u64 mode,
                                               u32 *omode_out, u32 *perm_out,
                                               bool *cloexec_out);

// #50: `mkdirat` -- create-by-path with DMDIR, the exclusive arm of
// SYS_OPEN_CREATE (mkdir has no open-if-present: an existing leaf is EEXIST).
// PURE. dirfd admits AT_FDCWD only; `mode` admits the low-9 bits (07000
// declines). Emits perm = (mode & 0777) | DMDIR. The shell closes the fd the
// kernel core returns -- Linux mkdirat returns 0, not a descriptor.
enum viv_verdict vivarium_mkdirat_decide(u64 dirfd, u64 mode, u32 *perm_out);

// #50: `unlinkat` -- flags 0 <-> unlink a non-directory, AT_REMOVEDIR <->
// SYS_UNLINK_REMOVEDIR (rmdir an empty directory): a 1:1 map onto the native
// unlink mechanics run on the split parent. Any other flag bit declines.
// dirfd admits AT_FDCWD only. PURE.
enum viv_verdict vivarium_unlinkat_decide(u64 dirfd, u64 flags,
                                          u32 *tflags_out);

// #50: `renameat` / `renameat2` -- Linux's replace-existing atomicity IS
// SYS_RENAME's documented contract, so the map is 1:1 with nothing computed;
// the decide is the domain gate alone. Both dirfds admit AT_FDCWD only;
// renameat2's `flags` admits exactly 0 (NOREPLACE/EXCHANGE/WHITEOUT decline,
// census-visible -- renameat passes literal 0 here). PURE.
enum viv_verdict vivarium_renameat_decide(u64 olddirfd, u64 newdirfd,
                                          u64 flags);

// The Linux aarch64 `struct stat` (128 bytes) — `include/uapi/asm-generic/stat.h`,
// which `third_party/musl/arch/aarch64/bits/stat.h` reproduces field-for-field.
// Spelled with explicit-width members rather than musl's typedefs so the layout is
// readable without chasing `alltypes.h`; the aarch64 overrides that make it work
// out are `blksize_t = int` and `nlink_t = unsigned int`.
//
// This is an ABI type: the kernel writes sizeof(*this) bytes into a Linux guest's
// buffer, so the offsets are pinned below exactly as `struct t_stat`'s are.
struct viv_linux_stat {
    u64 st_dev;         //   0
    u64 st_ino;         //   8
    u32 st_mode;        //  16
    u32 st_nlink;       //  20
    u32 st_uid;         //  24
    u32 st_gid;         //  28
    u64 st_rdev;        //  32
    u64 __pad1;         //  40
    s64 st_size;        //  48
    s32 st_blksize;     //  56
    s32 __pad2;         //  60
    s64 st_blocks;      //  64
    s64 st_atime_sec;   //  72
    u64 st_atime_nsec;  //  80
    s64 st_mtime_sec;   //  88
    u64 st_mtime_nsec;  //  96
    s64 st_ctime_sec;   // 104
    u64 st_ctime_nsec;  // 112
    u32 __unused4;      // 120
    u32 __unused5;      // 124
};

_Static_assert(sizeof(struct viv_linux_stat) == 128,
               "struct viv_linux_stat is the Linux aarch64 stat ABI -- pinned at 128 "
               "bytes. The kernel writes sizeof() bytes into a Linux guest's buffer, "
               "so a drifted layout corrupts the guest's stack.");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_dev)        ==   0, "st_dev @0");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_ino)        ==   8, "st_ino @8");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_mode)       ==  16, "st_mode @16");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_nlink)      ==  20, "st_nlink @20");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_uid)        ==  24, "st_uid @24");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_gid)        ==  28, "st_gid @28");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_rdev)       ==  32, "st_rdev @32");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_size)       ==  48, "st_size @48");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_blksize)    ==  56, "st_blksize @56");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_blocks)     ==  64, "st_blocks @64");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_atime_sec)  ==  72, "st_atim.tv_sec @72");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_atime_nsec) ==  80, "st_atim.tv_nsec @80");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_mtime_sec)  ==  88, "st_mtim.tv_sec @88");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_mtime_nsec) ==  96, "st_mtim.tv_nsec @96");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_ctime_sec)  == 104, "st_ctim.tv_sec @104");
_Static_assert(__builtin_offsetof(struct viv_linux_stat, st_ctime_nsec) == 112, "st_ctim.tv_nsec @112");

// Convert a Thylacine `struct t_stat` into the Linux aarch64 `struct stat`.
// PURE — plain data in, plain data out; the caller owns both buffers.
//
// `out` is fully written (every reserved/pad word zeroed), so a stale kernel
// stack frame cannot leak into a guest through the gaps. That is an I-13
// obligation, not tidiness.
void vivarium_stat_to_linux(const struct t_stat *in, struct viv_linux_stat *out);

// Decide whether a `newfstatat` is inside the translatable domain (V-2c).
// PURE, and the smallest translator here: it returns a verdict and NOTHING else.
//
// That emptiness is the finding. `openat` had to compute a rewritten start_fd
// because SYS_OPEN TAKES one; SYS_STAT does not take a base at all — it is
// hardcoded to "absolute from the Territory root, relative joined with the LS-4
// cwd" (syscall.h:1605), which IS the AT_FDCWD rule. `sys_stat_for_proc` and
// `sys_open_handler` perform that join with the same `territory_join_cwd`
// call, so the correspondence is one implementation, not two that agree.
//
// The consequence cuts both ways: AT_FDCWD is free, and a REAL dirfd is not
// merely unimplemented but INEXPRESSIBLE — there is no argument to put it in.
//
// WHY THERE IS NO `vivarium_fstatat_build`. `openat` gets one because its
// translation ends in a native SYS_OPEN the dispatcher can run. This one cannot:
// SYS_STAT copies out an 88-byte `struct t_stat`, and the guest's buffer wants
// the 128-byte Linux layout, so dispatching SYS_STAT at the guest's pointer
// would write the wrong struct into it. The shell must instead call
// `sys_stat_for_proc` into a KERNEL t_stat, run it through
// `vivarium_stat_to_linux`, and copy out 128 bytes — exactly `fstat`'s shape.
// So `newfstatat` is `openat`'s front half joined to `fstat`'s back half, and
// the missing build function is what that join looks like.
//
// D-1: `*stalk_flags_out` is 0 (follow -- POSIX stat) or nonzero (the lstat
// shape -- AT_SYMLINK_NOFOLLOW admitted since symlinks landed); the shell
// forwards it to sys_stat_for_proc, which owns the resolver vocabulary.
enum viv_verdict vivarium_fstatat_decide(u64 dirfd, u64 flags,
                                         u32 *stalk_flags_out);

// TIER 2 — `faccessat` (the git chunk; VIVARIUM.md §6.26). The whole decision is
// the AT_FDCWD gate: it is both the "only the cwd form is TRANSLATED" contract
// (a dirfd-relative access would need a real dirfd resolve this row does not do)
// AND the native-48 collision defense (SYS_NOTE_MASK's new_mask is never -100).
// PURE: no memory, no mode read -- the mode's EINVAL contour and the stat +
// perm_check live in the shell, exactly as mmap judges `len` there. The raw
// syscall carries no flags word (musl access()/faccessat(...,0) issue the 3-arg
// form; the 4-arg faccessat2 is a different number), so there is nothing to map.
enum viv_verdict vivarium_faccessat_decide(u64 dirfd);

// -----------------------------------------------------------------------------
// TIER 2 — `mmap` (V-2d). See VIVARIUM.md §6.21.
//
// V-2a classified `mmap` FORWARD as "the 'needs judgement' case the rule exists
// to exclude". §4.1 changed what FORWARD costs: V-3 is deferred, so FORWARD now
// means ENOSYS, and `mmap` is on musl's critical path twice over
// (`__init_tls.c:137` for TLS; mallocng for every heap area). A guest cannot
// reach main() without it. So it is promoted the same way `openat` was — over a
// STATED ARGUMENT DOMAIN, which is the tool V-2b already built.
//
// The target is SYS_BURROW_ATTACH_LAZY: it takes a length, picks the address,
// and produces a demand-zero RW/XN anonymous region.
//
// THE PROTECTION QUESTION, and why the strict answer loses. Thylacine anonymous
// memory is ALWAYS RW/XN and there is NO prot-mutation syscall anywhere — that
// is an I-12 design choice, not a gap. So this row cannot honour PROT_NONE or a
// read-only PROT_READ exactly; it grants read+write regardless.
//
// Declining every prot but PROT_READ|PROT_WRITE would be the letter of §6.19's
// "never silently mistranslate". But PROT_NONE is the DOMINANT anonymous shape
// in musl -- the thread guard page (pthread_create.c:295) and mallocng's meta
// areas (malloc.c:82) -- so declining it means malloc never initialises and
// nothing runs at all. Admitting it is a STATED FIDELITY DEGRADATION, and musl
// itself is the evidence that it is the sanctioned one:
//
//     mallocng/malloc.c:92 -- if (mprotect(p, pagesize, PROT_READ|PROT_WRITE)
//                                 && errno != ENOSYS) return 0;
//
// The libc ANTICIPATES a system with no mprotect and proceeds on the assumption
// that the PROT_NONE mapping is already usable, which is exactly what Thylacine
// produces. The consequence is named in VIVARIUM.md §9's DEGRADED tier rather
// than buried here: guard pages are NOT protective under the Linux phenotype,
// and a PROT_READ anonymous mapping is writable. It costs FIDELITY, never
// AUTHORITY -- the pages are the guest's own, every gate is unchanged, and
// nothing crosses a Proc boundary, so I-43 is untouched.
//
// PROT_EXEC is the hard line and is REFUSED, not degraded. An executable
// anonymous mapping is what CAP_JIT / I-42 governs (JIT-ON-WX-DESIGN.md), and
// W^X (I-12) forbids the RW-and-X region the naive translation would produce.
// The admission is therefore an ALLOW-LIST of two bits rather than "everything
// except PROT_EXEC" -- measured, aarch64 musl also defines PROT_BTI/PROT_MTE and
// generic musl PROT_GROWSDOWN/PROT_GROWSUP, none of which we can honour either.
// -----------------------------------------------------------------------------

// Linux `prot` bits. Generic musl `include/sys/mman.h`, plus the two aarch64
// additions from `arch/aarch64/bits/mman.h` -- read from the tree, not recalled.
enum {
    VIV_PROT_NONE       = 0,
    VIV_PROT_READ       = 1,
    VIV_PROT_WRITE      = 2,
    VIV_PROT_EXEC       = 4,
    VIV_PROT_BTI        = 0x10,        // aarch64
    VIV_PROT_MTE        = 0x20,        // aarch64
    VIV_PROT_GROWSDOWN  = 0x01000000,
    VIV_PROT_GROWSUP    = 0x02000000,
};

// Linux `flags` bits (generic musl `include/sys/mman.h`).
enum {
    VIV_MAP_SHARED          = 0x01,
    VIV_MAP_PRIVATE         = 0x02,
    VIV_MAP_FIXED           = 0x10,
    VIV_MAP_ANONYMOUS       = 0x20,
    VIV_MAP_NORESERVE       = 0x4000,
    VIV_MAP_STACK           = 0x20000,
    VIV_MAP_FIXED_NOREPLACE = 0x100000,
};

// The only admitted `flags` word -- an EXACT match, not a mask test.
//
// MAP_STACK and MAP_NORESERVE are absent DELIBERATELY. Both are arguably
// honourable (MAP_STACK is a no-op on Linux; Thylacine's lazy anon already IS
// the no-reserve behaviour), but MEASURED, musl passes neither: both
// pthread_create sites and all four mallocng sites pass exactly
// MAP_PRIVATE|MAP_ANON. Admitting a flag no caller sends would be speculation
// dressed as generosity, and this file's standard is that each admission is a
// claim about behaviour that has to be justified individually.
#define VIV_MMAP_FLAGS_ADMITTED ((u32)(VIV_MAP_PRIVATE | VIV_MAP_ANONYMOUS))

// Decide whether an `mmap` is inside the translatable domain. PURE -- no user
// memory, no Proc, no locks.
//
// `addr` is accepted at ANY value and deliberately ignored: without MAP_FIXED,
// Linux specifies `addr` as a HINT the kernel may disregard, and the caller
// learns the real address from the return value. Ignoring it is conforming, not
// a compromise. MAP_FIXED / MAP_FIXED_NOREPLACE -- where the address is a
// REQUIREMENT -- are outside the admitted flags word and therefore decline.
//
// `len` is NOT judged here. It is a semantic question, not a domain one: Linux
// answers EINVAL for 0 and ENOMEM for too-large, and the shell produces both
// exactly (0 up front; the target's own refusal for the rest). Forwarding on
// length would answer ENOSYS for a call Linux gives a specific errno.
//
// Returns VIV_TRANSLATED (the shell may call SYS_BURROW_ATTACH_LAZY with `len`)
// or VIV_FORWARD. Never ENOSYS: `mmap` exists.
enum viv_verdict vivarium_mmap_decide(u64 addr, u64 prot, u64 flags,
                                      u64 fd, u64 offset);

// -----------------------------------------------------------------------------
// TIER 2 — the FILE mmap arm (DISTRO D-3). See docs/DISTRO.md §6.
//
// A SECOND decider rather than a widened first one, and the split is deliberate.
// The two domains are disjoint by construction (the anon arm demands
// MAP_ANONYMOUS and fd == -1; this one demands its absence and fd >= 0), so
// nothing needs to arbitrate between them -- but keeping them apart means the
// anon arm's 18 domain tests still exercise BYTE-IDENTICAL code after D-3, which
// is what makes them a regression net for this change rather than a casualty of
// it. `vivarium_mmap_arms_disjoint` pins the disjointness itself.
//
// The domain is MEASURED off stock ldso, not derived from Linux. musl's
// map_library (third_party/musl/ldso/dynlink.c:809) opens a library with exactly
// one call of this shape -- the whole-span reservation:
//
//     mmap(addr_min, map_len, prot, MAP_PRIVATE, fd, off_start)
//
// where `prot` is the LOWEST PT_LOAD's prot and `addr_min` is a bare hint (no
// MAP_FIXED). Read against the shipped Alpine libc that is
// `mmap(0, 0xc3000, PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 0)`.
//
// THE ADMITTED prot IS R, OR R|X -- NEVER WRITABLE, and that is the I-36 line
// rather than a convenience. A writable file mapping would have to write back,
// which is the one thing REVENANT's file-backed Burrow does not do; D-3's answer
// to a writable request is a private eager COPY (a separate arm), so no
// userspace writable file mapping exists on any path. PROT_WRITE therefore
// declines HERE and is served elsewhere -- it does not ride this arm.
//
// `offset` MUST be page-aligned. Linux gives EINVAL for a misaligned offset, but
// this is stricter than fidelity: map_file_backed's #149 note records that the
// FILE fault arm derives each page's file position as
// `v->file_offset + (page-floored burrow offset)`, which is only the mapping's
// own bytes when the Burrow's offset 0 IS the mapping's start. The alignment
// requirement is what keeps that identity true by construction.
//
// `len` is NOT judged here, for the same reason the anon arm does not judge it.
#define VIV_MMAP_FILE_FLAGS_ADMITTED ((u32)VIV_MAP_PRIVATE)
#define VIV_MMAP_FILE_PROT_ADMITTED  ((u32)(VIV_PROT_READ | VIV_PROT_EXEC))

enum viv_verdict vivarium_mmap_file_decide(u64 prot, u64 flags,
                                           u64 fd, u64 offset);

// -----------------------------------------------------------------------------
// TIER 2 — the two MAP_FIXED arms (DISTRO D-3b). See docs/DISTRO.md §6.
//
// map_library reserves a whole span (the D-3a arm above) and then OVERLAYS each
// remaining PT_LOAD onto it. Two more shapes, both MEASURED off dynlink.c:
//
//   arm 2, :842 -- every PT_LOAD past the lowest, at its OWN prot:
//     mmap_fixed(base+this_min, this_max-this_min, prot,
//                MAP_PRIVATE|MAP_FIXED, fd, off_start)
//
//   arm 3, :851 -- the whole-page bss tail, gated on
//   `p_memsz > p_filesz && (p_flags & PF_W)`:
//     mmap_fixed(pgbrk, base+this_max-pgbrk, prot,
//                MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0)
//
// NOTE that arm 2's `this_max` derives from p_memsz, NOT p_filesz -- so arm 2
// maps file-backed PAST the file's data and arm 3 then overlays the whole bss
// pages. Between them musl memsets the partial tail page (:849), which is a
// WRITE into arm 2's mapping.
//
// THAT WRITE IS WHY A WRITABLE arm 2 IS NOT A FILE MAPPING. A writable private
// file mapping would need copy-on-write over the shared Image-cache pages, and a
// bug there would leak one container's writes into another's view of the same
// library. So a PROT_WRITE arm-2 request is served by an EAGER PRIVATE COPY into
// an anonymous Burrow -- which is a conforming MAP_PRIVATE (POSIX and Linux both
// leave post-mmap file changes unspecified for private mappings), is the
// CONSERVATIVE reading, and keeps "no userspace writable file mapping exists"
// true by construction rather than by care. I-36 is untouched.
//
// MEASURED cost of that copy, over every ELF in the stock Alpine rootfs: 888 KiB
// if every library were mapped at once; 372 KiB for the largest single one
// (libcrypto.so.3); 16 KiB for the ld-musl a typical dynamic process maps.
//
// ALSO MEASURED, and it bounds what the in-guest gate can prove: all 18 ELFs in
// that rootfs carry exactly two PT_LOADs, `R-X` then `RW-`. So arm 2 is ALWAYS
// writable there -- the non-writable arm-2 path (which rides the shared Image
// cache exactly as D-3a does) has NO producer on this rootfs and is exercised
// only by the unit suite. It is built because the ELF format permits it and a
// `-z separate-code` toolchain (binutils >= 2.31, so Debian/Fedora) emits the
// four-segment `R / R-X / R / RW-` layout that produces it; it is NOT claimed as
// gate-covered.
//
// PROT_NONE DECLINES on both arms, and on arm 3 that deliberately diverges from
// the non-fixed anon arm, which degrades PROT_NONE to writable. The difference
// is that a FIXED PROT_NONE request over an existing mapping is a GUARD -- and
// silently handing back a writable page where a guard was asked for is not a
// degradation anyone would sanction. Measured, it costs nothing: arm 3 fires
// only under PF_W, so its prot always carries R|W.
//
// `addr` is a REQUIREMENT here, not the hint it is on the non-fixed arms, so it
// must be page-aligned and non-zero (a fixed map at NULL is refused).
#define VIV_MMAP_FIXED_FILE_FLAGS_ADMITTED \
    ((u32)(VIV_MAP_PRIVATE | VIV_MAP_FIXED))
#define VIV_MMAP_FIXED_FILE_PROT_ADMITTED \
    ((u32)(VIV_PROT_READ | VIV_PROT_WRITE | VIV_PROT_EXEC))

#define VIV_MMAP_FIXED_ANON_FLAGS_ADMITTED \
    ((u32)(VIV_MAP_PRIVATE | VIV_MAP_FIXED | VIV_MAP_ANONYMOUS))
#define VIV_MMAP_FIXED_ANON_PROT_ADMITTED \
    ((u32)(VIV_PROT_READ | VIV_PROT_WRITE))

enum viv_verdict vivarium_mmap_fixed_file_decide(u64 addr, u64 prot, u64 flags,
                                                 u64 fd, u64 offset);
enum viv_verdict vivarium_mmap_fixed_anon_decide(u64 addr, u64 prot, u64 flags,
                                                 u64 fd, u64 offset);

// True iff no argument tuple is admitted by more than ONE mmap arm. Pure; exists
// so the disjointness the arms' comments CLAIM is a checked fact rather than a
// set of assertions that agree with each other. Covers all four arms pairwise --
// they separate on the flags word alone (each arm demands EXACT equality against
// a distinct value), so this also fails loudly if a future edit relaxes any of
// those equalities into a mask test.
bool vivarium_mmap_arms_disjoint(u64 addr, u64 prot, u64 flags,
                                 u64 fd, u64 offset);

// -----------------------------------------------------------------------------
// TIER 0/2 — signals (V-6). See VIVARIUM.md §6.22.
//
// Thylacine has no POSIX signals; it has Plan 9 NOTES (I-19). Every row below is
// a decode onto a note that ALREADY EXISTS, which is what makes Tier 0 a
// translation rather than new machinery.
//
// THE PIECE THAT IS NEW: the per-Proc disposition table. Pouch keeps its sigtab
// in USERSPACE (`__pouch_sigtab`, patch 0007) because the pouch libc is ours to
// patch -- it registers ONE bootstrap via SYS_NOTIFY and dispatches per-signal
// itself. A Vivarium guest's libc is not ours, so that architecture is simply
// unavailable and the kernel has to hold the table. It is the reason `rt_sigaction`
// is a shell with state rather than a pure row.
// -----------------------------------------------------------------------------

// Linux aarch64 signal numbers (musl `arch/aarch64/bits/signal.h`, read from the
// tree). Only the ones a row actually names are listed; the rest are handled by
// range checks, so an unlisted number is not a silent gap.
enum {
    VIV_SIGHUP  = 1,  VIV_SIGINT  = 2,  VIV_SIGQUIT = 3,  VIV_SIGILL  = 4,
    VIV_SIGABRT = 6,  VIV_SIGBUS  = 7,  VIV_SIGFPE  = 8,  VIV_SIGKILL = 9,
    VIV_SIGSEGV = 11, VIV_SIGPIPE = 13, VIV_SIGTERM = 15, VIV_SIGCHLD = 17,
    VIV_SIGCONT = 18, VIV_SIGSTOP = 19, VIV_SIGTSTP = 20, VIV_SIGWINCH = 28,
};

// Linux's `_NSIG` is 65 and signals are 1..64 inclusive.
#define VIV_NSIG 64

// `sa_handler` sentinels (generic musl `include/signal.h`).
#define VIV_SIG_DFL 0UL
#define VIV_SIG_IGN 1UL
#define VIV_SIG_ERR ((u64)-1)

// `sa_flags` bits (musl `arch/aarch64/bits/signal.h`).
enum {
    VIV_SA_NOCLDSTOP = 1,
    VIV_SA_NOCLDWAIT = 2,
    VIV_SA_SIGINFO   = 4,
    VIV_SA_ONSTACK   = 0x08000000,
    VIV_SA_RESTART   = 0x10000000,
    VIV_SA_NODEFER   = 0x40000000,
    VIV_SA_RESETHAND = 0x80000000,
    VIV_SA_RESTORER  = 0x04000000,
};

// `rt_sigprocmask` how-values (generic musl `include/signal.h`).
enum { VIV_SIG_BLOCK = 0, VIV_SIG_UNBLOCK = 1, VIV_SIG_SETMASK = 2 };

// The kernel-ABI sigaction struct the GUEST sends -- musl's `struct k_sigaction`
// with SA_RESTORER present, which is the shape aarch64 musl actually emits.
//
// MEASURED, not assumed: musl compiles with `-D_XOPEN_SOURCE=700` (its Makefile
// line 50), which satisfies the guard exposing SA_RESTORER in
// `arch/aarch64/bits/signal.h:114`, so `src/internal/ksigaction.h` includes the
// `restorer` member and `sigaction.c` always fills it with `__restore_rt`
// (`mov x8,#139; svc 0`).
//
// V-6b CORRECTION. V-6a shipped this as a RUNTIME discrimination -- helpers that
// returned 24 or 32 depending on whether the caller set SA_RESTORER -- on the
// theory that a libc omitting the flag sends the shorter shape. Reading musl
// showed the layout is fixed by the ARCH, not chosen per call:
//
//   * `src/internal/ksigaction.h` gates `restorer` on `#ifdef SA_RESTORER`, a
//     COMPILE-time arch property. aarch64 has no override in `arch/aarch64/`,
//     and its `bits/signal.h` defines SA_RESTORER, so the member is always there.
//   * `sigaction.c` sets `ksa.flags |= SA_RESTORER` UNCONDITIONALLY for every
//     install -- `signal(SIGPIPE, SIG_IGN)` included. There is no musl call that
//     arrives without it.
//   * Linux copies `sizeof(struct sigaction)` from `act` regardless of flags. If
//     the kernel wanted 24 on aarch64, musl's 32-byte struct would land `mask`
//     8 bytes past where the kernel reads it and every install would carry the
//     wrong mask -- so musl working at all is the proof.
//
// So the size is a CONSTANT here, matching what Linux itself reads, and a guest
// that sends a short buffer gets the same fault it would get on Linux rather
// than a shape we invented for it.
struct viv_ksigaction {
    u64 handler;
    u64 flags;
    u64 restorer;
    u64 mask;
};
#define VIV_KSIGACTION_SIZE         32u
#define VIV_KSIGACTION_OFF_FLAGS     8u
#define VIV_KSIGACTION_OFF_RESTORER 16u
#define VIV_KSIGACTION_OFF_MASK     24u
_Static_assert(sizeof(struct viv_ksigaction) == VIV_KSIGACTION_SIZE,
               "the kernel-side mirror must match the size the guest sends");
_Static_assert(__builtin_offsetof(struct viv_ksigaction, flags)    == VIV_KSIGACTION_OFF_FLAGS,    "flags @8");
_Static_assert(__builtin_offsetof(struct viv_ksigaction, restorer) == VIV_KSIGACTION_OFF_RESTORER, "restorer @16");
_Static_assert(__builtin_offsetof(struct viv_ksigaction, mask)     == VIV_KSIGACTION_OFF_MASK,     "mask @24");
// The lock-free per-field access discipline (vivarium.c) is single-copy-atomic
// only on naturally aligned u64s: pin the alignment the argument stands on.
_Static_assert(_Alignof(struct viv_ksigaction) == 8 && sizeof(u64) == 8,
               "sigtab fields must be naturally aligned 8-byte words");

// Which note carries a given Linux signal. PURE.
//
// VIV_SIGNOTE_NONE means "no note in the tree carries this signal" -- the row
// declines rather than inventing a delivery. That is the honest answer for
// SIGALRM (no timer note), SIGUSR1/2 (no general-purpose note), and the rest of
// the realtime range.
enum viv_signote {
    VIV_SIGNOTE_NONE = 0,
    VIV_SIGNOTE_INTERRUPT,       // SIGINT only -- see viv_signal_note for why
                                 // V-6b evicted SIGTERM from this row
    VIV_SIGNOTE_KILL,            // SIGKILL          (non-catchable both sides)
    VIV_SIGNOTE_PIPE,            // SIGPIPE
    VIV_SIGNOTE_CHILD_EXIT,      // SIGCHLD
    VIV_SIGNOTE_SNARE_SEGV,      // SIGSEGV
    VIV_SIGNOTE_SNARE_BUS,       // SIGBUS
    VIV_SIGNOTE_SNARE_ILL,       // SIGILL
    VIV_SIGNOTE_SNARE_FPE,       // SIGFPE
    VIV_SIGNOTE_TTY_HUP,         // SIGHUP
    VIV_SIGNOTE_TTY_QUIT,        // SIGQUIT
    VIV_SIGNOTE_TTY_WINCH,       // SIGWINCH
    VIV_SIGNOTE_TTY_SUSP,        // SIGTSTP
    VIV_SIGNOTE_TTY_CONT,        // SIGCONT
};

enum viv_signote viv_signal_note(u64 signum);

// One past the last real note kind -- the array bound for a per-note table.
#define VIV_SIGNOTE_COUNT ((u32)VIV_SIGNOTE_TTY_CONT + 1u)

// Does `signum` EXCLUSIVELY own its note -- is it the ONLY Linux signal this
// map routes there? PURE.
//
// V-6b found this the hard way. A DISPOSITION is per-signal, but the note
// substrate carries no signal identity: the poster posts the NAME "interrupt",
// and SIGINT and SIGTERM both land on it. So `sigaction(SIGINT, SIG_IGN)` with
// SIGTERM left at SIG_DFL is NOT REPRESENTABLE -- honouring it silences SIGTERM
// too, and not honouring it kills a Proc that asked to ignore Ctrl-C. Both
// directions are wrong, which means the call is outside the domain, not merely
// approximated. It declines.
//
// Computed by SCANNING the map rather than listing exclusive signals, so it can
// never drift: adding a second signal to any note automatically narrows the
// domain instead of silently making one of the two wrong. The exclusivity rule
// is also what makes the REVERSE direction (note -> signal) well-defined, which
// is what delivery needs to answer "is this note ignored?".
bool viv_signal_owns_note_exclusively(u64 signum);

// Can a note of this kind actually REACH a Proc's queue? PURE.
//
// MEASURED, not assumed: `g_known_notes` (notes.c) holds interrupt / kill /
// pipe / child_exit / tty:{winch,susp,cont,quit,hup} -- and NOT the snare:*
// family. `proc_fault_terminate` calls `exits(name)` DIRECTLY without going
// through `notes_post`, so an EL0 fault terminates its Proc before any queue
// or mask is consulted (notes.h says the same of NOTE_BIT_SNARE: "this bit has
// no consumer today").
//
// The consequence for the phenotype is sharp and permanent-until-v1.x: SIGSEGV,
// SIGBUS, SIGILL and SIGFPE can be given SIG_DFL (terminate -- which is what
// already happens) but can NEVER be caught or ignored. A guest that installs a
// SIGSEGV handler is told so, rather than discovering it at the first fault.
bool viv_signote_is_deliverable(enum viv_signote note);

// Which Linux signal owns this note -- the inverse of `viv_signal_note`. PURE.
//
// Well-defined ONLY because `viv_signal_owns_note_exclusively` gates every
// disposition write, so a note with a live entry has exactly one signal behind
// it. Computed by SCANNING for the same reason the exclusivity check does: a
// second table would be a second thing to keep in step.
//
// Returns 0 for a note no signal maps to. Delivery needs this to fill
// `si_signo` and x0, which is the whole reason the exclusivity rule is load-
// bearing rather than merely tidy.
u64 viv_signote_to_signal(enum viv_signote note);

// Is this note's LINUX default action "ignore"? PURE.
//
// SIGCHLD, SIGWINCH and SIGCONT are the three whose default is to do nothing.
// It matters because Thylacine's queue holds an undelivered note until someone
// reads it, and a Linux guest has no notes fd to read one with (there is no
// translation row for SYS_NOTE_OPEN, and a native number reaches the table
// first) -- so without this the queue would fill with notes nothing will ever
// consume. Dropping them at delivery is what Linux does and what keeps the
// queue bounded.
//
// SIGTSTP is deliberately NOT here: its default is STOP, not ignore, so
// claiming "ignore" for it would be a stored lie in the other direction. The
// STOP is applied, not dropped -- at POST time by job_stop_cb when any thread
// leaves the tty family unmasked, and at DELIVERY time by the NOTE_DFL_STOP
// arm of notes_deliver_at_el0_return when they were all masked and the note
// had to wait. So the "queue would fill with notes nothing consumes" hazard
// above does not apply to it either: both arms consume.
//
// (This used to defer on "the kernel NDFLT-stop arm is an unbuilt ABI decision
// (task #15)". 434c3fd9 built that arm; the premise is discharged.)
bool viv_signote_default_is_ignore(enum viv_signote note);

// Is this note's LINUX default action "terminate"? PURE. SIGPIPE, SIGINT,
// SIGHUP, SIGQUIT -- the catchable terminate-default signals a note carries.
//
// The phenotype branch of the EL0 tail acts on this for a SIG_DFL candidate
// instead of falling through to the native uncaught arm, because that arm is
// keyed on the NATIVE terminate latch, and the native `pipe` note has none
// (task #237, a Plan 9 ABI question). A Linux guest has no notes fd, so a
// SIG_DFL SIGPIPE nothing acts on becomes the queue head forever: every later
// caught signal is stranded behind it and every later default-ignore note is
// never dropped. Linux's answer is not in doubt -- SIG_DFL SIGPIPE terminates
// -- so the phenotype answers it for its own Procs and leaves the native
// question where it is.
//
// SIGKILL terminates too but is answered by the dispatcher's kill branch before
// any disposition is read (non-catchable, and vivarium_sigaction_decide refuses
// it a sigtab row); SIGTSTP stops (the STOP arm consumes it); the three
// `default_is_ignore` rows are dropped; the snare:* rows never reach a queue.
bool viv_signote_default_is_terminate(enum viv_signote note);

// Per-Proc signal disposition. Lazily allocated on a Proc's first translatable
// `rt_sigaction`; freed ONLY at proc_free (reset in place at exec -- #254).
// Across process creation the rule is POSIX's, phenotype-conditional (ARCH 7.6,
// operator-voted 2026-08-17): fork COPIES the table into the child's OWN
// allocation (viv_sigtab_clone_into), exec resets the CAUGHT rows and KEEPS
// SIG_IGN (viv_sigtab_reset_caught); a native Proc clears everything. (This
// paragraph said "NOT inherited across rfork" and called SIG_IGN's survival a
// "stated fidelity gap (section 9)" -- both false since d3a11c8e.)
//
// Indexed by `enum viv_signote`, NOT by signal number -- legitimate ONLY
// because `viv_signal_owns_note_exclusively` gates every write, so each live
// entry has exactly one signal behind it.
//
// V-6c widened this from `u8 ignored[]` to the whole `k_sigaction`. V-6b stored
// only what it could honour, which was one bit; delivery needs the handler
// address, SA_RESTORER (the guest's return trampoline) and the flags. A zeroed
// table reads as all-SIG_DFL, which is the correct initial state, so the lazy
// allocation needs no separate initialiser.
struct viv_sigtab {
    struct viv_ksigaction act[VIV_SIGNOTE_COUNT];
};

// -----------------------------------------------------------------------------
// SOCKETS (V-5) -- the per-Proc socket table.
//
// Linux fuses into one fd what /net splits across three files, so a translated
// socket carries state no fd and no path can hold. docs/VIVARIUM.md section 5.5
// has the design and the measurements; the short version:
//
//   socket()  open /net/<proto>/clone -> the fid BECOMES ctl; read it for N
//   connect() write the verb to ctl; open .../N/data; SWAP the fd onto data
//   read/write/close  untranslated -- T1 rows on an ordinary Spoor fd
//
// `N` is re-readable from ctl, but only while the fd still IS ctl. `proto` is
// knowable ONLY at socket(), where the guest passes SOCK_STREAM/SOCK_DGRAM and
// never mentions it again. Recovering it later would mean decoding netd's qid
// layout -- REFUSED, because /net is a mount point that need not be netd, and
// decoding a foreign server's qid as netd's is exactly the silent
// mistranslation the argument-domain rule exists to forbid. So proto is
// REMEMBERED. That is the whole reason this table exists.
// -----------------------------------------------------------------------------

// The /net protocol directories. Values are internal (never crossing to EL0),
// so they are ordinals, not an ABI.
enum viv_net_proto {
    VIV_NET_TCP = 0,
    VIV_NET_UDP = 1,
};

// A socket's lifecycle position, which is also WHICH FILE THE FD DENOTES:
//
//   FRESH      -> the fd is the connection's `ctl`  (fresh, or bound, or both)
//   LISTENING  -> the fd is STILL `ctl` (announce is a ctl write, not a swap)
//   CONNECTED  -> the fd has been swapped onto `data`
//
// So the ctl/data split is FRESH|LISTENING vs CONNECTED, not FRESH vs the rest:
// a listening socket keeps its ctl fd forever, because that is the fd `accept`
// re-walks from and the fd whose reference keeps the listener alive. Anything
// that writes a ctl verb (connect, announce) requires a non-CONNECTED fd, and
// anything that moves bytes requires CONNECTED.
enum viv_sock_state {
    VIV_SOCK_FREE      = 0,   // the entry is not in use (zeroed table = all free)
    VIV_SOCK_FRESH     = 1,
    VIV_SOCK_CONNECTED = 2,
    VIV_SOCK_LISTENING = 3,
};

// Bounded: a guest must not be able to grow kernel memory without bound (the
// I-32 posture). 64 is generous against PROC_HANDLE_MAX (256) while keeping the
// table under a page; a guest that exhausts it gets EMFILE, which is a POSIX
// answer rather than an extinction.
#define VIV_SOCK_MAX 64u

// The SOCK_NONBLOCK / SOCK_CLOEXEC flag bits carried in socket()'s type word
// (Linux aarch64 octal values). In the header because both the decide (which
// masks them off before the base-type switch) and the socket shell (which
// applies them to the ctl fd) need them.
enum {
    VIV_SOCK_NONBLOCK = 04000,
    VIV_SOCK_CLOEXEC  = 02000000,
};

struct viv_sock {
    u64 epoch;       // the identity key: a monotonic per-TABLE stamp, unique to
                     // THIS claim of the slot (0 = never claimed). See the keyed-
                     // writer contract -- `n` alone cannot serve, because netd
                     // recycles the connection number (lowest-free slot index).
    s32 fd;          // the guest's fd; < 0 when the entry is free
    u32 n;           // the /net connection number
    u32 bound_addr;  // bind(): the requested local address, host order (0 = any)
    u32 remote_addr; // N-2a: the last unconnected sendto() destination, host order
    u16 bound_port;  // bind(): the requested local port,    host order (0 = any)
    u16 remote_port; // N-2a: the last sendto() dest port (0 = never sent to)
    u8  proto;       // enum viv_net_proto
    u8  state;       // enum viv_sock_state
};

// THERE IS DELIBERATELY NO `bound` FLAG. An unbound socket and one bound to
// 0.0.0.0:0 are indistinguishable in EVERY path this table feeds:
//   * listen()  needs a concrete non-zero port either way (netd's announce
//               parser rejects port 0), so both are refused identically;
//   * connect() is unconstrained by both, so both proceed identically.
// A flag would therefore be state that no reader could ever branch on. If a
// future row makes the two differ -- getsockname() on a bound-but-idle socket
// is the candidate -- add it THEN, with the reader that needs it.
_Static_assert(sizeof(struct viv_sock) == 32, "viv_sock pinned at 32 bytes "
               "(the u64 epoch identity key added 8; still 64*32 = 2 KiB, under a page)");

// The per-Proc table (Proc.socktab). Lazily allocated on the first translated
// socket(), CAS-installed, freed at proc_free, NOT rfork-inherited -- the
// viv_sigtab shape exactly.
//
// LOCKED SINCE N-3 (phenotype threads). Until clone(CLONE_THREAD) landed this
// table was lock-free, sound because a PHENO_LINUX Proc could not obtain a peer
// thread -- a property of the clone row's argument domain, which N-3 dissolved
// by admitting the thread set. Peer threads now share this table, so the flat
// free-list array races on three axes: slot ALLOCATION (two claim() scans
// picking the same FREE slot), field TEARING (one thread writing remote_*/state
// while another reads them), and slot REUSE (a blocked op holding a pointer into
// a slot that a peer close()+socket() recycled for a different socket). The last
// is the sharpest -- it is [[bug-254]] (a read/write correct when written
// becomes a race as the reader set grows) and [[bug-spoor-transport-lock-across-sleep]]
// (a pointer held across a sleep names a stranger's object on the far side).
//
// THE DISCIPLINE that closes all three without ever holding the lock across
// I/O (which spinlock.h forbids: sleep-under-spinlock is a whole-guest wedge):
//   * `lock` is a LEAF spinlock, held ONLY over pure array ops -- claim, drop,
//     a field read snapshotted into the caller's stack, a keyed field write.
//     Never across a walk/open/read/write. So a caller that must block does its
//     I/O with the lock DROPPED.
//   * READERS take a SNAPSHOT (viv_socktab_get copies the whole entry out under
//     the lock); they never dereference a table pointer after the lock drops.
//   * WRITERS are KEYED + IDENTITY-GUARDED: a set_* re-finds the fd under the
//     lock and writes only if the slot still names the SAME socket (fd present
//     AND epoch == the expect_epoch the caller snapshotted). A slot that a peer
//     closed and recycled carries a STRICTLY GREATER epoch, so a stale write
//     from a blocked op lands nowhere -- it cannot corrupt the socket that
//     reused the slot.
// The identity key is `epoch`, a MONOTONIC per-table stamp (`next_epoch++` on
// each claim), NOT `n`. `n` -- the /net connection number -- cannot serve,
// because netd mints it as the LOWEST-FREE slot index (usr/netd server.rs,
// MAX_CONNS=8) with no generation folded in: a close()+socket() on one fd draws
// the SAME n by default, so an n-keyed guard would silently pass a stale write
// onto the recycled socket (the holotype F1 finding). A monotonic epoch is
// unique across every recycle (u64: it does not wrap in any real runtime), which
// is exactly the "different lifetime" the guard needs -- immutability-for-life
// was the wrong property. `epoch` is captured by the same snapshot that reads
// the other fields, so keying on it costs nothing extra.
//
// sigtab, the sibling per-Proc table, needs NO such lock: it is FIXED-INDEX (by
// signote enum, never a free-list scan, so no allocation race) and publishes
// each row with an atomic release store on the handler gate (viv_sigtab_set), a
// discipline that already tolerates concurrent delivery-read vs sigaction-write.
// socktab cannot borrow that trick: its entries are scanned, allocated/freed,
// and multi-field with an identity that must be seen together -- hence the lock.
struct viv_socktab {
    spin_lock_t     lock;       // leaf; held only over array ops, never across I/O
    u64             next_epoch; // monotonic; stamps each claim's viv_sock.epoch
    struct viv_sock s[VIV_SOCK_MAX];
};

// Snapshot the entry for `fd` into `*out` (which may be NULL for an existence
// test). Returns true iff `fd` has a live entry. Takes the lock; the copy makes
// the result safe to read after the lock drops. Replaces the old pointer-
// returning find() -- a table pointer must never outlive the lock now.
bool viv_socktab_get(struct viv_socktab *tab, s32 fd, struct viv_sock *out);

// Claim a free entry for `fd` in state `born`. Returns true on success, false if
// the table is full (-> EMFILE) or `tab` is NULL. Takes the lock; the scan +
// write are one critical section, so two peer claims cannot pick one slot. Does
// NOT check for a duplicate fd: the caller has just been handed that fd by
// handle_alloc, so it cannot already be in the table -- an entry left behind by
// a closed fd would be the close-hook bug this table's drop path prevents, and a
// duplicate here would be its symptom rather than its cause. `born` is FRESH for
// socket() and CONNECTED for accept() (an accepted fd IS data, born connected).
bool viv_socktab_claim(struct viv_socktab *tab, s32 fd,
                       enum viv_net_proto proto, u32 n, enum viv_sock_state born);

// Release the entry for `fd`, if any. Idempotent -- an fd with no entry (a
// plain file, or a socket already dropped) is a no-op, which is what lets the
// close hook run unconditionally for a phenotyped Proc. Takes the lock.
void viv_socktab_drop(struct viv_socktab *tab, s32 fd);

// Drop the socktab entry of every close-on-exec socket in `p`. execve calls this
// AFTER the commit and BEFORE handle_close_on_exec frees the fds, so a freed fd
// number cannot carry a stale (proto, n) row into the new image. Reads cloexec
// live from the handle table; runs in execve's sole-live-thread window
// (proc_exec_alone), so it takes NO socktab lock -- there is no peer thread to
// race, and taking the lock would nest it under the handle table's own lock
// (handle_get_cloexec requires t->lock) for no benefit. NULL-safe.
struct Proc;
void viv_socktab_drop_cloexec(struct Proc *p);

// True when a claim would succeed. Takes the lock for a coherent scan, but the
// answer is ADVISORY: accept() asks BEFORE it blocks (making a real inbound
// connection exist and discovering a full table afterwards means hanging up on a
// peer), then a real peer claim() may consume the room before accept's own claim
// -- which still returns false and yields EMFILE. This is the courtesy check,
// not the safety one.
bool viv_socktab_has_room(struct viv_socktab *tab);

// Keyed, identity-guarded writers. Each re-finds `fd` UNDER THE LOCK and writes
// only if the slot still names the same socket (present AND epoch == expect_epoch).
// Returns true if written, false if the slot was closed or recycled while the
// caller blocked -- a stale write from a blocked op is dropped, never applied to
// the socket that reused the slot. `expect_epoch` is the `epoch` the caller
// snapshotted (a monotonic per-claim stamp; see the struct comment for why NOT n).
//   set_state    -- listen(): FRESH -> LISTENING.
//   set_bound    -- bind(): record the requested local endpoint.
//   record_remote-- connect()/sendto(): record the peer; also_connect
//                   additionally transitions FRESH -> CONNECTED in the same
//                   lock hold (so a peer recvmsg never sees CONNECTED with an
//                   unset remote), which connect() passes true and the
//                   connectionless datagram sendto passes false.
bool viv_socktab_set_state(struct viv_socktab *tab, s32 fd, u64 expect_epoch,
                           enum viv_sock_state st);
bool viv_socktab_set_bound(struct viv_socktab *tab, s32 fd, u64 expect_epoch,
                           u32 addr, u16 port);
bool viv_socktab_record_remote(struct viv_socktab *tab, s32 fd, u64 expect_epoch,
                               u32 addr, u16 port, bool also_connect);

// Decide whether a `socket(domain, type, protocol)` is inside the translatable
// domain, and if so which /net protocol directory it names. PURE.
//
// THE ARGUMENT DOMAIN. AF_INET only: AF_INET6 has no /net representation at
// v1.0 and is refused honestly (EAFNOSUPPORT) rather than silently served as
// v4. SOCK_STREAM -> tcp, SOCK_DGRAM -> udp; SOCK_SEQPACKET/SOCK_RAW have no
// /net analogue. `protocol` must be 0 or the family default (IPPROTO_TCP/UDP);
// anything else names a protocol netd does not speak.
//
// SOCK_NONBLOCK/SOCK_CLOEXEC in the type word are ADMITTED (N-1a): the decide
// masks them off before the base-type switch and the shell applies them --
// NONBLOCK as the ctl open-file's CNONBLOCK (which the recv shells read to turn
// netd's non-blocking empty-read into -EAGAIN), CLOEXEC as the fd's cloexec bit.
// Any OTHER high bit in the type word is refused (T_E_INVAL): an unknown flag is
// a request no honest translation exists for.
//
// Returns true + writes *out_proto when translatable; false leaves *out_proto
// untouched and the caller answers the errno in *out_err.
bool vivarium_socket_decide(u64 domain, u64 type, u64 protocol,
                            enum viv_net_proto *out_proto, s32 *out_err);

// The /net protocol directory name for a proto ("tcp" / "udp"). PURE; never
// NULL for a value the decide function produced.
const char *vivarium_net_proto_dir(enum viv_net_proto proto);

// getsockopt's admitted domain: exactly (SOL_SOCKET, SO_ERROR). Linux uapi
// values (asm-generic; aarch64 uses the generic numbering).
enum {
    VIV_SOL_SOCKET = 1,
    VIV_SO_ERROR   = 4,
};

// Decide whether a `getsockopt(fd, level, optname, ...)` is inside the
// translatable domain. PURE. Narrowed to 32 bits: both arguments are C `int`s
// in the Linux ABI, so the narrowing IS the ABI (the openat precedent, not the
// clone one).
//
// THE ARGUMENT DOMAIN is one point: (SOL_SOCKET, SO_ERROR) -- the connect
// verification every libcurl consumer runs (`verifyconnect`: a getsockopt
// failure is read as CONNECT failure, so a refused row here turns a
// SUCCEEDED connect into "(7) Could not connect"). Everything else still
// declines to ENOSYS so a guest's own fallback runs exactly as it did when
// the whole number was refused -- widening this domain is a per-option
// honesty argument, not a default.
//
// WHAT the shell answers, and the EXACT boundary of its honesty (holotype F2):
// SO_ERROR is a socket's pending error, cleared by the read. The shell answers
// the constant 0, and that is TRUE for every SYNCHRONOUSLY-delivered error --
// which is the entire class a blocking-only phenotype socket produces on the
// GUEST's own syscalls (SOCK_NONBLOCK is refused at socket(), F_SETFL is not
// served), so a failure is always that op's own return value, never pending.
// This is exactly what the row exists for: connect verification (curl's
// verifyconnect branches on err==0 -> connected), where a failed connect
// already returned its error and a successful one has no pending error.
//
// THE ONE GAP, deliberately shipped narrowed (operator-ratified 2026-08-25):
// netd ALSO latches errors ASYNCHRONOUSLY -- a connected-UDP/ICMP send that
// fails locally (server.rs data_send) or the connect-timeout reaper -- and
// surfaces them as POLLERR via check_ready. The shell does not consult that
// latch, so a guest that observes POLLERR on such a socket and then reads
// SO_ERROR gets 0, contradicting the POLLERR. Consulting the latch honestly
// needs a netd-errno-exposure protocol path + a blocking read-and-clear (its
// own arc; filed). The gap is LATENT at v1.0 (no shipping guest reaches it --
// UDP DNS is not yet live) and does not touch the connect-verification path
// the row serves. REVISIT also pins the NONBLOCK case: a NONBLOCK row would
// make a FRESH-state socket carry its in-flight connect outcome here.
//
// Returns true when admitted; false leaves the decline errno in *out_err
// (T_E_NOSYS -- the T2 "declined these arguments" path).
bool vivarium_getsockopt_decide(u64 level, u64 optname, s32 *out_err);

// The one send flag the sendto row admits. Linux asm-generic value; a
// TRUTHFUL no-op here (see vivarium_sendto_decide).
enum { VIV_MSG_NOSIGNAL = 0x4000 };

// Decide whether a `sendto(fd, buf, len, flags, addr, addrlen)` /
// `recvfrom(fd, buf, len, flags, addr, addrlen)` is inside the translatable
// domain. PURE (the caller resolves fd -> socktab state first).
//
// WHY THESE ROWS EXIST: aarch64 has no plain send/recv syscall -- musl's
// send() IS sendto(fd, buf, len, flags, NULL, 0) and recv() IS
// recvfrom(..., NULL, NULL) -- so any Linux binary that sends on a socket
// through send() (curl does) reaches these numbers, and the FORWARD refusal
// made every such send die ENOSYS mid-connection. The served shape is
// EXACTLY the connected-socket send()/recv(): NULL address, flags 0 (plus
// MSG_NOSIGNAL for send -- truthfully a no-op, because the phenotype socket
// data path is a 9P Spoor write and the pipe EPIPE-note machinery never runs
// there, so no SIGPIPE exists to suppress). The shells then delegate the
// data movement to the NATIVE write/read handlers -- the same path a
// T1-renumbered write()/read() on the socket fd takes -- so short
// writes/reads, weft fast-paths, and the #844 fd lifecycle behave
// identically to the native call.
//
// SERVED (N-2a): the with-address UDP datagram shape -- sendto(fd, q, ql,
// MSG_NOSIGNAL, dest, sl) on a FRESH udp socket. The shell dials `dest` on the
// ctl fd (netd re-points conn N per call) and moves the bytes on a transient
// data fid; recvmsg (212) reads the reply and synthesizes msg_name from the
// recorded destination. This is the DNS path -- res_msend.c sends a query per
// nameserver this way. (recvfrom with a non-NULL addr stays declined -- musl's
// resolver receives via recvmsg, not recvfrom; the recvfrom addr writeback is
// still unbuilt and honestly ENOSYS.)
//
// DECLINED, each to ENOSYS (the census-visible "unbuilt" answer), each
// honestly: a with-address TCP send (no connectionless TCP), MSG_PEEK (no
// non-consuming 9P read), MSG_DONTWAIT (would lie -- nonblock is the socket's
// own CNONBLOCK, not a per-call flag), MSG_WAITALL (changes the return
// contract), a non-NULL recvfrom source-address out-pointer (peer-address state
// recvfrom does not carry -- recvmsg does), AND an unconnected send with NO
// destination -- genuinely unbuilt (no dial without an addr), so it declines to
// ENOSYS rather than a fabricated ENOTCONN (R2-F1: Linux gives EPIPE/
// EDESTADDRREQ, we serve none, and an errno would hide the gap from the census).
bool vivarium_sendto_decide(enum viv_net_proto proto, u8 state, u64 flags,
                            u64 addr_va, u64 addrlen, s32 *out_err);
bool vivarium_recvfrom_decide(u8 state, u64 flags, u64 addr_va, s32 *out_err);

// recvmsg(212) decide (N-2b). flags must be 0 -- MSG_PEEK (no non-consuming 9P
// read), MSG_WAITALL (changes the return contract), MSG_DONTWAIT (nonblock is
// the socket's own CNONBLOCK, not a per-call flag) all decline to ENOSYS. The
// socket must be able to produce a datagram: CONNECTED (its fd IS `data`) or a
// FRESH socket with a recorded remote (has_remote = a prior unconnected sendto).
// A never-sent FRESH socket or a LISTENING one has nothing to receive -> ENOSYS.
// PURE (the caller resolves fd -> socktab state + remote first).
bool vivarium_recvmsg_decide(u8 state, u64 flags, bool has_remote, s32 *out_err);

// Parse a Linux `struct sockaddr_in` (already copied into kernel memory) into
// its four address octets + host-order port. PURE.
//
// Returns false when the family is not AF_INET, the length is short, or the
// port is 0 (netd's dial parser rejects it, and a connect to port 0 is
// meaningless) -- so a malformed address can never reach the ctl writer.
bool vivarium_sockaddr_in_parse(const u8 *sa, u64 salen,
                                u8 out_ip4[4], u16 *out_port);

// The same parse WITHOUT the port-0 rejection, for bind(), where 0.0.0.0:0 is
// the ordinary "any address, any port" request rather than a malformed one.
// PURE. vivarium_sockaddr_in_parse is this plus `port != 0`.
bool vivarium_sockaddr_in_parse_any(const u8 *sa, u64 salen,
                                    u8 out_ip4[4], u16 *out_port);

// Build a Linux `struct sockaddr_in` (the accept() peer-address out-parameter)
// into `buf`. PURE. Returns 16 -- the fixed sockaddr_in size -- or 0 if `buflen`
// is short. The bytes are laid out exactly as Linux expects: family LE, port and
// address BOTH network order.
u32 vivarium_sockaddr_in_build(u8 *buf, u32 buflen, const u8 ip4[4], u16 port);

// Build a netd ctl command line: "<verb> a.b.c.d!port". PURE. Returns the
// length written, or 0 if it would not fit (which the caller must treat as a
// refusal, never as a zero-length write).
u32 vivarium_net_cmd_ipport(char *buf, u32 buflen, const char *verb,
                            const u8 ip4[4], u16 port);

// Build the `announce` ctl verb. PURE. An address of 0.0.0.0 becomes Plan 9's
// wildcard `announce *!port`; a concrete address becomes `announce a.b.c.d!port`.
//
// The distinction is not cosmetic. netd migrates a listener announced on an
// EXPLICIT 127.x address onto its loopback stack, while a `*` listener stays on
// the NIC and does NOT span loopback -- so a guest binding 127.0.0.1 and a guest
// binding INADDR_ANY reach genuinely different listeners, and rendering one as
// the other would silently move the server.
u32 vivarium_net_cmd_announce(char *buf, u32 buflen, const u8 ip4[4], u16 port);

// Parse netd's `a.b.c.d!port` endpoint rendering (the `remote` / `local` files)
// back into octets + host-order port. PURE -- the inverse of the ipport builder.
// Returns false on any malformation, on a >255 octet, or on a >65535 port, so a
// garbled endpoint file can never become a plausible-looking peer address.
bool vivarium_parse_ipport(const char *buf, u32 len, u8 out_ip4[4], u16 *out_port);

// Decide whether listen() may proceed on a socket in this state. PURE.
//
// Writes the POSIX errno to *out_err and returns false when it may not:
//   * a UDP socket cannot listen at all         -> EOPNOTSUPP
//   * a CONNECTED socket cannot listen          -> EINVAL
//   * an unbound (or port-0) socket cannot      -> EOPNOTSUPP
// The last is a DEGRADED answer, not an equivalent one: Linux would auto-bind an
// ephemeral port, and netd's announce parser rejects port 0 outright. Declining
// is the honest half of the argument domain -- and harmless in practice, because
// discovering an auto-bound port needs getsockname(), which is not a row yet.
bool vivarium_listen_decide(enum viv_net_proto proto, enum viv_sock_state state,
                            u16 bound_port, s32 *out_err);

// Parse the decimal connection number netd returns when a `clone`/`ctl` fid is
// read. PURE. Returns false on empty, non-decimal, or out-of-range input --
// which the caller must treat as a protocol failure rather than connection 0.
bool vivarium_parse_conn_n(const char *buf, u32 len, u32 *out_n);

// -----------------------------------------------------------------------------
// Readiness (V-5c, section 5.5.4).
// -----------------------------------------------------------------------------

// Convert a Linux `struct timespec` to the native SYS_POLL millisecond timeout.
// PURE.
//
// ROUNDS UP. A sub-millisecond timeout must not become 0: 0 means "return
// immediately" to SYS_POLL, so truncating would turn a caller's 100us wait into
// a busy loop -- the one conversion error a poll loop would never notice and
// never stop paying for. Linux rounds a ppoll timeout up to the next tick for
// the same reason.
//
// SATURATES at INT32_MAX ms (~24.8 days) rather than overflowing. A caller
// asking for longer gets a shorter wait than it asked for and then loops --
// which is what a poll caller does anyway.
//
// Returns false with *out_err = EINVAL for a malformed timespec (a negative
// field, or tv_nsec >= 1e9), matching Linux's own validation.
bool vivarium_timespec_to_ms(s64 sec, s64 nsec, s32 *out_ms, s32 *out_err);

// Decide whether a ppoll() is inside the translatable domain. PURE.
//
// THE ARGUMENT DOMAIN, and note that both declines below are shapes rather than
// bad values -- ENOSYS, not EINVAL, because the arguments are perfectly valid
// Linux and it is this kernel that cannot serve them:
//
//   * sigmask != NULL -- ppoll's WHOLE REASON TO EXIST over poll() is that it
//     swaps the signal mask ATOMICALLY with the wait. Thylacine has no way to
//     do that, and doing it non-atomically re-opens exactly the race the caller
//     chose ppoll to close. Honouring it approximately would be the silent
//     mistranslation this tier exists to prevent. (musl's poll() passes NULL,
//     so the common path is unaffected.)
// nfds > POLL_MAX_NFDS is EINVAL rather than ENOSYS: that IS a bad value, and
// the native cap is the real bound a caller must respect.
//
// nfds == 0 IS SERVED, and it did not used to be. Linux reads it as "sleep for
// the timeout", and V-5c-1 declined it because native SYS_POLL rejects nfds == 0
// and there is no native sleep SYSCALL to route it to. That reasoning looked one
// layer too high: there is no sleep syscall, but there has always been a sleep
// PRIMITIVE -- `tsleep` with a deadline and a cond that is never true, which is
// what poll's own slow path parks on. V-5c-2 needed it anyway for
// `select(0, NULL, NULL, NULL, &tv)`, the classic portable sleep, so both
// zero-fd forms now route to `viv_timed_sleep` and neither declines.
bool vivarium_ppoll_decide(u64 nfds, u64 sigmask_va, s32 *out_err);

// -----------------------------------------------------------------------------
// pselect6 (V-5c-2) -- the fd_set reshape.
//
// The one T2 row whose translation is a genuine CHANGE OF REPRESENTATION rather
// than a renumber or a field copy: three 1024-bit bitmaps in, one pollfd array
// out, and three bitmaps back. It is also the row with the most ways to be
// subtly wrong, so the whole conversion is PURE and unit-driven, and the shell
// in syscall.c does nothing but uaccess and the poll call.
//
// THE PRIOR ART IS IN THIS TREE, AND IT IS WRONG IN FOUR WAYS. pouch's
// userspace select() (`usr/lib/pouch/patches/0005-pouch-poll.patch`) performs
// this same translation over native SYS_POLL. Reading it before writing this
// was worth more than the writing: every one of its four defects is a decision
// point here (task #99).
//
//   * IT CAPS THE WRONG AXIS. It rejects any fd >= 64 with EBADF, on the stated
//     grounds that "the handle would be unreachable through any Thylacine
//     syscall". That was true when PROC_HANDLE_MAX was 64; commit ffcc64b7 split
//     PROC_HANDLE_MAX (256, the fd-VALUE ceiling) from POLL_MAX_NFDS (64, the
//     pollfd-ARRAY ceiling) and made it false. The cap belongs on the COUNT of
//     contributing fds -- a select over fds 200 and 201 is two pollfds and is
//     fine; a select over 65 low fds is not.
//   * IT TRANSLATES exceptfds TO POLLPRI, which native poll has no bit for, so
//     the request is silently dropped and the exceptfds bit can never be set. A
//     select waiting ONLY on exceptfds therefore blocks forever instead of
//     failing. Silently-never-true is the mistranslation this tier exists to
//     prevent; see the exceptfds note on the scan function below.
//   * IT FORWARDS POLLHUP INTO THE WRITE SET, commented "(Linux semantics)".
//     Linux's POLLOUT_SET is POLLOUT|POLLERR; POLLHUP is in POLLIN_SET only.
//   * IT COUNTS FDS, NOT BITS. See the return-value note below.
//
// None of this makes pouch's select unusable -- it makes it a boundary-line that
// drifted from the kernel underneath it. This one is written against the kernel
// as it is today, with the constants named rather than copied.
// -----------------------------------------------------------------------------

// An fd_set is a fixed 1024-bit bitmap (musl `include/sys/select.h`:
// FD_SETSIZE 1024). The ABI is the caller's `fd_set *`, so this is the size of
// the object, not a tunable.
#define VIV_FD_SETSIZE   1024
#define VIV_FD_SET_BYTES (VIV_FD_SETSIZE / 8)

// Decide whether a pselect6() is inside the translatable domain, and clamp nfds.
// PURE.
//
// `nfds` arrives as a signed Linux `int` widened to 64 bits, so the negative
// case is real and must be checked as signed.
//
//   * sigmask != NULL -- declines for EXACTLY ppoll's reason (the atomic mask
//     swap has no counterpart), and note that pselect6 packs it indirectly: the
//     6th argument is a POINTER to {const sigset_t *ss; size_t ss_len;} because
//     aarch64 caps a syscall at 6 registers. So a NULL 6th argument and a
//     non-NULL pointer to a NULL ss are different things; only the former is
//     unambiguously "no mask", and the latter is declined rather than peeked at.
//   * nfds < 0 -- EINVAL, Linux's own check.
//
// CLAMPING IS LINUX'S BEHAVIOUR, NOT A SHORTCUT. `core_sys_select` does
// `if (n > max_fds) n = max_fds;` -- bits above the fd table are simply not
// scanned. Our handle table is a FIXED PROC_HANDLE_MAX entries (never grown), so
// "max_fds" here is a constant, and clamping to it is exactly what Linux does on
// a process whose table happens to be that size. A bit set above the clamp names
// an fd that cannot exist, so ignoring it loses nothing; a bit set BELOW it that
// names no open handle still becomes POLLNVAL -> EBADF, which is also Linux.
bool vivarium_pselect6_decide(u64 nfds_raw, u64 sigmask_va, u32 *out_nfds,
                              s32 *out_err);

// How many bytes of an fd_set cover the low `nfds` bits, rounded up to the
// 8-byte granule Linux uses (FDS_BYTES: FDS_LONGS(nr) * sizeof(long)). PURE.
//
// Copying only these bytes rather than the full 128 is fidelity, not thrift:
// Linux's get_fd_set/set_fd_set touch exactly this much, so a caller that sized
// its allocation to its nfds -- legal, and done by code that keeps fd_sets in
// packed structs -- must not take a fault here that it would not take there.
u32 vivarium_fdset_bytes(u32 nfds);

// Scan three fd_sets into a pollfd array. PURE.
//
// A NULL set pointer means "all zero" (Linux passes NULL for an unwanted set).
// `out_cap` is the caller's array length; the count of CONTRIBUTING fds is what
// the native ceiling applies to.
//
// EXCEPTFDS DECLINES RATHER THAN NEVER-FIRES. Native poll has no POLLPRI: the
// requestable set is (POLLIN|POLLOUT), full stop. So there is no honest way to
// represent "wake me on out-of-band data", and the two dishonest ways are both
// worse than declining -- dropping the bit silently (pouch) turns a pure
// exceptfds wait into an infinite block, and treating it as POLLIN would report
// ordinary data as an exception. A NULL or all-zero exceptfds is not a request
// and passes through untouched; only a SET bit inside [0, nfds) declines.
//
// Returns false with ENOSYS for a set exceptfds bit, EINVAL if more than
// `out_cap` fds contribute.
bool vivarium_fdset_to_pollfds(const u8 *rd, const u8 *wr, const u8 *ex,
                               u32 nfds, struct pollfd *out, u32 out_cap,
                               u32 *out_count, s32 *out_err);

// Map poll results back into three fd_sets and count the ready BITS. PURE.
//
// The three output buffers are ZEROED FIRST and then have only ready bits set --
// select reports its result by overwriting the caller's sets, so a bit the
// caller asked about and did not get back must come home clear.
//
// THE REVERSE MAPPING IS DELIBERATELY ASYMMETRIC. Linux's do_select tests
//     POLLIN_SET  = POLLIN  | POLLHUP | POLLERR
//     POLLOUT_SET = POLLOUT | POLLERR
// POLLHUP is in the read set and NOT the write set, which is not an oversight:
// a peer that hung up leaves data readable (then EOF, which read() must be
// allowed to observe), while writing to it is an error rather than a completion.
// Each test is additionally gated on the fd having been REQUESTED in that
// direction, so an errored fd the caller only listed in readfds comes back in
// readfds only.
//
// THE RETURN IS A COUNT OF BITS, NOT OF FDS. Linux increments retval once per
// bit set, so an fd ready for both reading and writing counts TWICE and the
// return can exceed the number of fds passed in. Callers written against that
// contract loop "while (n-- > 0) find the next set bit" and stop one short if
// the count is per-fd -- which is pouch's fourth defect.
//
// POLLNVAL FAILS THE WHOLE CALL. poll reports a bad fd per-entry; select has no
// per-fd error channel, so POSIX makes an invalid fd anywhere EBADF for the
// entire call. Returns false with EBADF, leaving the output buffers untouched --
// on an error Linux does not write the sets back at all.
bool vivarium_pollfds_to_fdset(const struct pollfd *pfds, u32 count,
                               u8 *rd, u8 *wr, u8 *ex, u32 *out_bits,
                               s32 *out_err);

// The budget a caller-requested timeout of 0 gets when the array holds a /net
// socket, so netd's async readiness probe has a chance to land (task #98; the
// reasoning is at the use site in viv_ppoll). Small enough that a zero-timeout
// poll is still "immediate", generous enough for a loopback round trip.
#define VIV_PPOLL_PROBE_MS 10

// The reverse step: which note kind is this note NAME? PURE.
//
// Takes the name rather than a bit because NOTE_BIT_TTY is one bit for five
// distinct names, and a disposition has to tell tty:winch from tty:hup. Returns
// VIV_SIGNOTE_NONE for anything unrecognised, so an unknown name is never
// treated as a signal.
enum viv_signote viv_signote_from_note_name(const char *name);

// The inverse: the canonical notes.h name literal (program-lifetime .rodata)
// for a decodable note, NULL for VIV_SIGNOTE_NONE. PURE. The rt_sigaction
// shell hands it to notes_discard_name when a new disposition ignores.
const char *viv_signote_note_name(enum viv_signote note);

// Is the note carrying `note` currently ignored by this Proc? PURE + NULL-safe
// (a Proc that never called rt_sigaction has no table and ignores nothing).
bool viv_sigtab_note_ignored(const struct viv_sigtab *tab, enum viv_signote note);

// Does this Proc have a REAL handler for `note` -- neither SIG_DFL nor SIG_IGN?
// PURE + NULL-safe. On true, `*out` receives the whole recorded k_sigaction
// (handler + flags + restorer + mask); on false `*out` is untouched, so a
// caller that ignores the return cannot deliver to a stale address.
bool viv_sigtab_note_handler(const struct viv_sigtab *tab, enum viv_signote note,
                             struct viv_ksigaction *out);

// Record a disposition. Returns false (and changes nothing) if the note is out
// of range or `act` is NULL -- the caller has already run the domain check, so
// this is defence-in-depth on the array index.
bool viv_sigtab_set(struct viv_sigtab *tab, enum viv_signote note,
                    const struct viv_ksigaction *act);

// exec's POSIX disposition reset, applied IN PLACE. NULL-safe (nothing to
// reset). The table object survives and the pointer never changes -- cross-Proc
// readers (notes_post's SIG_IGN hook, notes_proc_has_live_handler, the ^Z fan)
// hold it with a bare acquire and no lock of exec's, so freeing it here was a
// UAF (#254 / #243). The only free is proc_free, where the Proc itself is gone.
// Stores are per 8-byte FIELD (the granule those readers load), never per byte.
void viv_sigtab_reset(struct viv_sigtab *tab);

// The phenotype's fork/exec signal-state rule (ARCH 7.6, POSIX; 2026-08-17).
// execve: reset CAUGHT rows to SIG_DFL, keep SIG_IGN rows (the mask is kept by
// the caller). NULL-safe.
void viv_sigtab_reset_caught(struct viv_sigtab *tab);
// fork: give `child` its OWN copy of `parent`'s table (NULL parent table ->
// child NULL). 0, or -1 on OOM (child->sigtab left NULL). The child must be
// unpublished (a plain store, no CAS).
struct Proc;
int viv_sigtab_clone_into(struct Proc *child, const struct Proc *parent);

// Decide whether an `rt_sigaction` is inside the translatable domain. PURE.
//
// `signum` must be 1..64, must not be SIGKILL/SIGSTOP (POSIX: uncatchable, and
// Linux itself answers EINVAL), and must map to a note -- a disposition we can
// record but could never act on would be a stored lie.
//
// `handler` must not be SIG_ERR (POSIX-invalid; the pouch layer's F11 audit
// close found the same thing -- without the check a bootstrap calls h(sig) at
// address -1).
//
// THE ARGUMENT DOMAIN: installing a REAL handler requires SA_RESTORER, because
// the guest's own trampoline is how the handler returns. Thylacine will not
// synthesise one: the alternative is a vDSO sigreturn page, and Thylacine's vDSO
// is deliberately RO+XN (I-12/I-13) -- making it executable to serve a
// compatibility row would be a real weakening of an audited surface. SIG_DFL and
// SIG_IGN need no trampoline and are admitted without it.
//
// AT V-6b THE DOMAIN IS NARROWER STILL, and deliberately so:
//
//   * The signal must EXCLUSIVELY own its note (above) -- a shared note cannot
//     carry two independent dispositions.
//   * SIG_IGN on SIGCHLD declines. On Linux that is not "ignore", it is
//     AUTO-REAP -- the child never becomes a zombie. Thylacine has no such
//     mode, so honouring the surface meaning while dropping the real one would
//     leave a guest leaking zombies it believes were reaped.
//   * The note must be DELIVERABLE (above), so a SIGSEGV handler or an ignored
//     SIGBUS is refused rather than stored where nothing will ever read it.
//     SIG_DFL is still admitted for those -- terminate is what already happens.
//
// `sigsetsize` must be 8 (Linux checks `sizeof(sigset_t)` and musl passes
// `_NSIG/8` == 8).
enum viv_verdict vivarium_sigaction_decide(u64 signum, u64 handler, u64 flags,
                                           u64 sigsetsize);

// Decide whether an `rt_sigprocmask` is inside the translatable domain. PURE.
//
// The target is the per-Thread `note_mask`, which exists for exactly this reason
// ("so multi-thread Procs can have different threads accept different signals --
// POSIX pthread_sigmask semantics", notes.h:107).
//
// `how` must be one of BLOCK/UNBLOCK/SETMASK; `sigsetsize` must be 8. The MASK
// VALUE is not judged here -- bits naming signals with no note are dropped by
// `viv_sigset_to_notemask` rather than declining the whole call, because musl
// blocks wide masks (`__block_all_sigs` sets every bit) on paths where declining
// would break an otherwise-translatable program.
enum viv_verdict vivarium_sigprocmask_decide(u64 how, u64 sigsetsize);

// Convert a Linux `sigset_t` word into the kernel's NOTE_BIT_* mask. PURE.
//
// Bits naming signals with no note are DROPPED, deliberately: the alternative is
// to refuse a mask musl routinely sends. The consequence -- blocking SIGALRM has
// no effect because nothing can deliver SIGALRM either -- is consistent rather
// than lossy.
//
// `out_bits` receives a mask of (1u << NOTE_BIT_*) values; the NOTE_BIT_*
// numbering is notes.h's and is passed in by the caller so this file stays free
// of kernel headers.
struct viv_notebit_map {
    u8 interrupt, kill, pipe, child_exit, snare, tty;
};
u64 viv_sigset_to_notemask(u64 sigset, const struct viv_notebit_map *m);

// THE canonical instance, filled from notes.h's NOTE_BIT_* in vivarium.c.
//
// The map is still a PARAMETER rather than a global read inside the
// translators, because that is what keeps them unit-testable against a
// synthetic numbering. But there is exactly ONE kernel instance, because two
// consumers (the syscall shells and the delivery path) each hand-rolling a
// copy is precisely the mirror-drift trap: a per-file `static` verifies only
// that FILE's numbering, never that it agrees with the other's.
extern const struct viv_notebit_map g_viv_notebits;

// The inverse: report a NOTE_BIT_* mask back as a Linux `sigset_t` word. PURE.
//
// Deliberately reports EVERY signal a set bit actually blocks, which makes the
// coarseness VISIBLE instead of hidden. NOTE_BIT_TTY is one bit for five signals
// (notes.h: "per-kind masking is a v1.x extension"), so blocking SIGWINCH really
// does block SIGHUP as well -- and a guest that reads its mask back is told so,
// rather than being shown the tidy answer it asked for while the system does
// something wider. Over-blocking DEFERS a signal, it does not lose one; the
// honest report is what keeps that a stated cost rather than a surprise.
//
// Signals with no note are never reported blocked: nothing can deliver them, so
// "blocked" would describe a state that does not exist.
u64 viv_notemask_to_sigset(u64 notemask, const struct viv_notebit_map *m);

// The mask a handler RUNS under (Linux signal_delivered): the interrupted
// thread's mask, plus the action's sa_mask, plus the delivered signal itself
// unless SA_NODEFER. `sa_mask` is the raw sigset word the guest sent
// (viv_ksigaction.mask); `signum` the Linux number being delivered. PURE --
// the delivery path stores the result in note_mask for the handler's duration
// and puts the pre-handler mask back at rt_sigreturn. Through the same
// coarse translation as rt_sigprocmask, so a tty-family sa_mask entry blocks
// the family and SIGKILL in sa_mask is dropped, exactly as the mask row does.
u64 vivarium_handler_mask(u64 note_mask, u64 sa_mask, u64 sa_flags, u64 signum,
                          const struct viv_notebit_map *m);

// -----------------------------------------------------------------------------
// TIER 1 -- the signal frame (V-6c). See VIVARIUM.md §6.22.
//
// THE SHAPE, and why it deletes a hazard rather than guarding one. Delivery
// writes a real `siginfo_t` + `ucontext_t` to the guest's stack so a handler
// that READS them works, but `rt_sigreturn` restores from the kernel-side
// `Thread` snapshot and ignores the frame entirely. §8's audit-trigger row
// originally warned that Tier 1 "restores pstate/pc from user memory -- a
// classic privilege-escalation shape; must reject any frame that would
// elevate". Under this design NO field of the user frame ever reaches pstate,
// pc or sp, so there is no frame to reject and no validator to get wrong.
//
// The stated cost, in §9's DEGRADED tier: writing to `uc_mcontext` does not
// change where execution resumes. That breaks signal-driven control transfer
// (Go's sigpanic, JIT deoptimisation); neither reaches this path at v1.0.
//
// THE LAYOUT IS THE TARGET'S, NOT THE HOST'S -- and that distinction cost a
// measurement. `struct sigcontext` ends in musl's `long double __reserved[256]`,
// which is 16 bytes per element on aarch64 (4096 bytes, 16-ALIGNED, so it lands
// at 288 rather than 280) but only 8 on an arm64 Mac. Compiling the layout probe
// with the host cc gave sizeof == 2328; the same probe under
// `--target=aarch64-linux-gnu` gives 4384, and that is the number the guest
// uses. The offsets below were confirmed by _Static_assert against the real
// target compiler before being written down.
//
// WHAT IS WRITTEN vs WHAT IS RESERVED. `sigcontext.__reserved` is a 4096-byte
// area holding a chain of `struct _aarch64_ctx { u32 magic; u32 size; }` records
// (FPSIMD, ESR, SVE...). We write only the 8-byte TERMINATOR -- magic 0, size 0
// -- so a guest that walks the chain stops immediately and correctly concludes
// "no extension records". The remaining 4088 bytes are left as they were, which
// is the guest's OWN stack memory below its own sp: it could read those bytes
// before the signal and can read them after, so nothing crosses a boundary and
// I-13 is untouched. Zeroing them would cost 4 KiB of copy per delivery to hide
// data from the process that owns it.
//
// The absent FPSIMD record is HONEST rather than lazy: note delivery does not
// save or restore Q0-Q31 at all (task #96 -- a pre-existing property of the
// native note path, which V-6c makes more reachable), so a record claiming to
// hold them would be a lie. An empty chain says exactly what is true.
// -----------------------------------------------------------------------------

// Sizes of the FULL Linux structures -- what the stack pointer must skip, as
// distinct from the prefix this file actually writes.
#define VIV_SIGINFO_SIZE      128u    // siginfo_t
#define VIV_UCONTEXT_SIZE    4560u    // ucontext_t (176 header + 4384 mcontext)
#define VIV_SIGFRAME_SIZE    (VIV_SIGINFO_SIZE + VIV_UCONTEXT_SIZE)   // 4688
#define VIV_FRAME_RECORD_SIZE 16u     // { fp, lr } -- the walkable frame chain
#define VIV_SIGFRAME_TOTAL   (VIV_SIGFRAME_SIZE + VIV_FRAME_RECORD_SIZE)

// `si_code` for a kernel-generated signal (Linux `SI_KERNEL`). Chosen over
// SI_USER because it is the one value that claims NOTHING about the union: a
// note carries a 16-byte name and one u32 arg, so si_pid / si_uid / si_status /
// si_addr have no source. SI_USER would invite a guest to read si_pid and get a
// confident zero.
#define VIV_SI_KERNEL 0x80

// The Linux aarch64 `siginfo_t` (128 bytes). Only the three leading ints have a
// source here; the union is zeroed rather than guessed at.
struct viv_linux_siginfo {
    s32 si_signo;       //   0
    s32 si_errno;       //   4
    s32 si_code;        //   8
    s32 __pad0;         //  12
    u8  __pad1[112];    //  16 -- the _sifields union, deliberately all zero
};
_Static_assert(sizeof(struct viv_linux_siginfo) == VIV_SIGINFO_SIZE,
               "siginfo_t is 128 bytes on Linux aarch64");
_Static_assert(__builtin_offsetof(struct viv_linux_siginfo, si_signo) == 0, "si_signo @0");
_Static_assert(__builtin_offsetof(struct viv_linux_siginfo, si_errno) == 4, "si_errno @4");
_Static_assert(__builtin_offsetof(struct viv_linux_siginfo, si_code)  == 8, "si_code @8");

// The WRITTEN prefix of `struct sigcontext` -- through the first 8 bytes of
// `__reserved`, which carry the record-chain terminator. The full struct is 4384
// bytes; the 4088 past this are the guest's own untouched stack (see above).
struct viv_linux_mcontext_head {
    u64 fault_address;  //   0 -- 0: no deliverable note is fault-generated
    u64 regs[31];       //   8 -- x0..x30
    u64 sp;             // 256
    u64 pc;             // 264
    u64 pstate;         // 272
    u8  __pad_res[8];   // 280 -- __reserved is 16-aligned, so it starts at 288
    u32 end_magic;      // 288 -- _aarch64_ctx.magic == 0 (end of chain)
    u32 end_size;       // 292 -- _aarch64_ctx.size  == 0
};
_Static_assert(sizeof(struct viv_linux_mcontext_head) == 296, "mcontext head 296");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, regs)      ==   8, "regs @8");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, sp)        == 256, "sp @256");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, pc)        == 264, "pc @264");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, pstate)    == 272, "pstate @272");
_Static_assert(__builtin_offsetof(struct viv_linux_mcontext_head, end_magic) == 288,
               "sigcontext.__reserved is __aligned__(16), so it begins at 288 -- "
               "NOT 280. Getting this wrong puts every mcontext field the guest "
               "reads 8 bytes out of place.");

// The WRITTEN prefix of `ucontext_t`, which is everything up to and including
// the mcontext head.
struct viv_linux_ucontext_head {
    u64 uc_flags;       //   0
    u64 uc_link;        //   8 -- 0: no linked context
    u64 ss_sp;          //  16 -- stack_t uc_stack; SS_DISABLE, we have no altstack
    s32 ss_flags;       //  24
    u32 __pad_ss;       //  28
    u64 ss_size;        //  32
    u64 uc_sigmask[16]; //  40 -- sigset_t is 128 bytes in musl userspace
    u8  __pad_mctx[8];  // 168 -- uc_mcontext is 16-aligned, so it starts at 176
    struct viv_linux_mcontext_head uc_mcontext;   // 176
};
_Static_assert(sizeof(struct viv_linux_ucontext_head) == 472, "ucontext head 472");
_Static_assert(__builtin_offsetof(struct viv_linux_ucontext_head, ss_sp)       ==  16, "uc_stack @16");
_Static_assert(__builtin_offsetof(struct viv_linux_ucontext_head, uc_sigmask)  ==  40, "uc_sigmask @40");
_Static_assert(__builtin_offsetof(struct viv_linux_ucontext_head, uc_mcontext) == 176, "uc_mcontext @176");

// The contiguous head of Linux's `struct rt_sigframe { siginfo info; ucontext uc; }`.
// One kernel-side buffer, one copy-out.
struct viv_sigframe_head {
    struct viv_linux_siginfo       info;   //   0
    struct viv_linux_ucontext_head uc;     // 128
};
_Static_assert(sizeof(struct viv_sigframe_head) == 600, "written frame head 600");
_Static_assert(sizeof(struct viv_sigframe_head) <= VIV_SIGFRAME_SIZE,
               "the written prefix must fit inside the frame the sp reserves");

// Build the frame head. PURE -- plain data in, plain data out; the caller owns
// the buffer and performs the copy-out, exactly the vivarium_stat_to_linux
// split.
//
// `out` is fully written including every pad, so a stale kernel stack frame
// cannot reach a guest through the gaps (I-13).
//
// `regs31` must point to 31 words (x0..x30) -- the INTERRUPTED context, which
// the caller has already snapshotted. `sigmask` is the Linux sigset word the
// guest should see as blocked at delivery.
void vivarium_build_sigframe(struct viv_sigframe_head *out, u64 signum,
                             u64 sigmask, const u64 *regs31,
                             u64 sp, u64 pc, u64 pstate);

// -----------------------------------------------------------------------------
// TIER 2 — `clone` (LINEAGE L-3d). See docs/LINEAGE.md §5.3 and §8.
//
// The row that gives a Linux guest a second process. Its target is SYS_RFORK
// (LINEAGE L-3b), and the mapping is a CONSTANT rather than a computation:
//
//     clone(CLONE_VM|CLONE_VFORK|SIGCHLD, stack, ptid, tls, ctid)
//         ->  SYS_RFORK(RFPROC|RFMEM, stack, 0)
//
// so what this decide function actually decides is the DOMAIN, which is where
// every interesting question lives.
//
// THE ARGUMENT ORDER, and why it is not the one most people remember. arm64
// selects CONFIG_CLONE_BACKWARDS, so `tls` comes BEFORE `child_tid`:
//
//     x0 flags   x1 stack   x2 parent_tid   x3 tls   x4 child_tid
//
// musl's own aarch64 clone.s states it in a comment at the top of the file
// ("syscall(SYS_clone, flags, stack, ptid, tls, ctid)"), which is where this was
// read from. The x86-64 order (ptid, ctid, tls) would silently swap two words.
//
// THE HAZARD THAT MAKES THIS ROW DIFFERENT FROM EVERY OTHER ONE: x2, x3 and x4
// ARE GARBAGE ON THE ONLY CALL THAT MATTERS. `posix_spawn` invokes
// `__clone(child, stack, flags, arg)` with FOUR arguments (posix_spawn.c:198),
// and clone.s then does `mov x2,x4 / mov x3,x5 / mov x4,x6` -- moving three
// registers the caller never set. On Linux that is harmless, because
// CLONE_PARENT_SETTID / CLONE_SETTLS / CLONE_CHILD_SETTID are all clear and the
// kernel therefore never reads them.
//
// A translator that reached for `args[3]` as the child's TLS would be reading
// one of those uninitialised registers and handing it to the child as
// TPIDR_EL0 -- and the child would then fault or corrupt on its first
// thread-local access, at a site with no visible connection to the clone. So
// this decide takes flags and stack ONLY, the shell passes a LITERAL 0 for
// child_tls (SYS_RFORK's "inherit the caller's" sentinel, which is what a vfork
// child needs), and the three garbage words are never named at all. The
// admitted domain's exclusion of CLONE_SETTLS is what makes that correct rather
// than merely safe.
//
// (This is the inverse of the arity property the ARCH §25.4 row states for T1
// rows. There the risk is a native target reading MORE argument words than the
// Linux call supplies; here the words are supplied and MEANINGLESS. Both come
// down to the same rule: read a register only when the call's own contract says
// it holds something.)
// -----------------------------------------------------------------------------

// Linux `clone` flag bits (musl `include/sched.h`, read from the tree). Only the
// ones the domain reasoning below names are listed.
enum {
    VIV_CSIGNAL              = 0x000000ff,   // the low byte IS the exit signal
    VIV_CLONE_VM             = 0x00000100,
    VIV_CLONE_FS             = 0x00000200,
    VIV_CLONE_FILES          = 0x00000400,
    VIV_CLONE_SIGHAND        = 0x00000800,
    VIV_CLONE_VFORK          = 0x00004000,
    VIV_CLONE_THREAD         = 0x00010000,
    VIV_CLONE_SYSVSEM        = 0x00040000,
    VIV_CLONE_SETTLS         = 0x00080000,
    VIV_CLONE_PARENT_SETTID  = 0x00100000,
    VIV_CLONE_CHILD_CLEARTID = 0x00200000,
    VIV_CLONE_DETACHED       = 0x00400000,
    VIV_CLONE_CHILD_SETTID   = 0x01000000,
};

// SIGCHLD, from `arch/aarch64/bits/signal.h` -- the exit signal posix_spawn asks
// for, and the only one Thylacine can deliver: `exits()` posts the `child_exit`
// note unconditionally (I-19), so SIGCHLD is what the target already does.
#define VIV_CLONE_SIGCHLD 17u

// The only admitted `flags` word -- an EXACT match, exactly as
// VIV_MMAP_FLAGS_ADMITTED is, and for the same reason: a bit we have not
// reasoned about must decline rather than ride along.
//
// Compared at FULL 64-BIT WIDTH, unlike every other decide in this file. Those
// narrow to 32 bits because their Linux parameters ARE `int`; clone's `flags`
// is an `unsigned long`, so narrowing here would be an assumption about Linux's
// own source rather than about its ABI -- and this tree cannot check that. The
// stricter reading is therefore the right one under uncertainty, and it costs
// nothing: musl's clone.s zero-extends (`uxtw x0,w2`), so the high half is
// always 0 from the real consumer.
#define VIV_CLONE_FLAGS_ADMITTED \
    ((u32)(VIV_CLONE_VM | VIV_CLONE_VFORK | VIV_CLONE_SIGCHLD))

// The SECOND admitted word (LINEAGE L-6a): a plain `fork()`. musl's fork() ->
// _Fork() emits exactly `clone(SIGCHLD, 0)` -- no CLONE_VM, so the child gets a
// PRIVATE copy-on-write address space, which is what L-4/L-5 built.
//
// It is a separate exact word rather than a mask relaxation for the reason the
// vfork word is exact: every bit outside these two is one nobody has reasoned
// about. And the two shapes DIFFER in more than a bit -- they take opposite
// `stack` rules (below) and map onto different rfork flag words -- so a single
// mask could not express either correctly.
#define VIV_CLONE_FLAGS_FORK ((u32)VIV_CLONE_SIGCHLD)

// The THIRD admitted word (N-3): a pthread `clone(CLONE_THREAD)`. musl's
// pthread_create emits EXACTLY this word (`__clone(func, stack, 0x007D0F00,
// ...)`, pthread_create.c) -- the full CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|
// SETTLS|PARENT_SETTID|CHILD_CLEARTID|DETACHED set. Unlike the fork/vfork words
// this one is genuinely CONCURRENT: the child is a schedulable Thread in the
// caller's OWN Proc, and every sharing bit (VM/FS/FILES/SIGHAND) is satisfied
// BY CONSTRUCTION -- a Thread linked into that Proc inherits its AddrSpace /
// Territory / HandleTable / sigtab. SYSVSEM/DETACHED are no-ops at v1.0 (no
// SysV sems; detach state is a userspace word). Exact, for the FLAGS_ADMITTED
// reason: a bit outside this set is one nobody has reasoned about.
#define VIV_CLONE_FLAGS_THREAD                                                 \
    ((u32)(VIV_CLONE_VM | VIV_CLONE_FS | VIV_CLONE_FILES | VIV_CLONE_SIGHAND | \
           VIV_CLONE_THREAD | VIV_CLONE_SYSVSEM | VIV_CLONE_SETTLS |           \
           VIV_CLONE_PARENT_SETTID | VIV_CLONE_CHILD_CLEARTID |               \
           VIV_CLONE_DETACHED))

// The cheapest possible guard against a vocabulary typo, and it pins the exact
// ABI word being matched: if any bit constant above drifts, the build stops.
_Static_assert(VIV_CLONE_FLAGS_THREAD == 0x007D0F00u,
               "VIV_CLONE_FLAGS_THREAD must equal musl pthread_create's clone "
               "flag word 0x007D0F00 -- a mismatch means a CLONE_* bit is wrong");

// The clone shape the decide selects. FORK = a private copy-on-write child (a
// fork, or a null-stack vfork served as one); VFORK = an RFMEM child + parent
// suspend (posix_spawn's non-zero-stack shape); THREAD = a Thread in the
// caller's OWN Proc (N-3). An enum rather than the older `bool share_mem` so a
// third outcome is nameable -- the pure layer says WHICH shape without importing
// proc.h's RFPROC/RFMEM, exactly as the bool did.
enum viv_clone_mode {
    VIV_CLONE_MODE_FORK,
    VIV_CLONE_MODE_VFORK,
    VIV_CLONE_MODE_THREAD,
};

// Decide whether a `clone` is inside the translatable domain. PURE -- no user
// memory, no Proc, no locks.
//
// WHY `CLONE_VM` WITHOUT `CLONE_VFORK` IS REFUSED RATHER THAN SERVED. This is
// the one place where L-3c-2's "the fail-safe direction is one-sided" argument
// does NOT carry over, and the difference is worth stating because the two
// chunks reach opposite conclusions from the same shape.
//
// At L-3c-2 the suspend was keyed on RFMEM rather than on a flag of its own,
// and the justification was that an unwanted suspend blocks visibly while an
// unwanted concurrency corrupts silently. That reasoning holds for a NATIVE
// caller, who reaches SYS_RFORK through a Thylacine ABI whose only shape is the
// vfork one. It does not hold here. A stock Linux binary that sets CLONE_VM and
// clears CLONE_VFORK has said, in the only vocabulary it has, "do not suspend
// me" -- and serving it with a suspend converts a working program into a
// DEADLOCK whenever the child neither execs nor exits promptly (a worker thread
// signalling through shared memory is the ordinary case). That is not
// conservative; it is a hang with our name on it.
//
// So the CLONE_VM-without-VFORK domain is exact, and the caller gets an honest
// decline it can act on. The genuinely concurrent shape -- the full pthread word
// VIV_CLONE_FLAGS_THREAD -- is NOW served (N-3), as VIV_CLONE_MODE_THREAD: a
// Thread in the caller's OWN Proc, NOT the old "route onto SYS_THREAD_SPAWN"
// plan. The reason it is a Thread-in-this-Proc and not a spawn is that a Linux
// clone hands the kernel no entry function -- the child resumes at the parent's
// trap frame -- so the forked-frame core (thread_create_forked) is the correct
// target, and SYS_THREAD_SPAWN's entry-va shape is the wrong one.
//
// A ZERO `stack` under the vfork shape is `vfork()` proper, and it is SERVED AS
// A FORK (option B, 2026-08-31; LINEAGE.md 3.1). Linux reads stack==0 under
// CLONE_VM as "share the parent's stack", safe there ONLY because CLONE_VFORK
// suspends the parent so the two never push concurrently. Plan 9 has no such
// shape -- rfork always gives the child its own stack, which is why it has no
// stack argument -- and SYS_RFORK's RFMEM child_sp rule is that invariant. Two
// Procs on one stack is the one memory-sharing shape the lineage refused, so
// rather than weaken that gate (option A, rejected as anti-lineage) the null
// stack maps to a private copy-on-write child: POSIX makes anything but
// _exit/exec after vfork undefined, so a copy is conformant, and the only
// observable differences (no suspend, pre-exec writes not shared) are the
// undefined ones. The result is share_mem=false -- the SAME translation the
// fork arm produces -- so it is one path reached by two flags words, not a new
// one. A NON-zero stack stays a true RFMEM vfork (posix_spawn's shape).
//
// Returns VIV_TRANSLATED or VIV_FORWARD. Never ENOSYS: `clone` exists, and the
// shapes outside the domain are ones a later chunk may serve rather than ones
// to deny forever.
//
// On VIV_TRANSLATED, *mode_out says WHICH shape the shell must build:
//   VIV_CLONE_MODE_VFORK  = RFMEM (child shares the address space and the parent
//                           suspends -- a non-zero-stack vfork);
//   VIV_CLONE_MODE_FORK   = a private copy-on-write child (a fork, OR a
//                           null-stack vfork served as one per above);
//   VIV_CLONE_MODE_THREAD = a Thread in the CALLER's OWN Proc (the pthread word).
// The shell turns FORK/VFORK into an rfork flag word and THREAD into a
// thread_create_forked into the caller's Proc; the pure layer names the shape
// without importing proc.h's RFPROC/RFMEM, which do not belong on this side of
// the boundary.
//
// It is an OUT-PARAM rather than something the shell re-derives from `flags`
// (the vivarium_openat_decide shape) because re-deriving would put the same
// decision in two places, and the three shapes differ in exactly the way a
// second reader would get wrong.
enum viv_verdict vivarium_clone_decide(u64 flags, u64 stack,
                                       enum viv_clone_mode *mode_out);

// -----------------------------------------------------------------------------
// N-3: futex -- the pthread wait/wake substrate. musl's DEFAULT (private,
// non-robust, non-PI) mutex + cond + join emit exactly three ops, verified
// against third_party/musl 1.2.5:
//   FUTEX_WAIT (0)     the block primitive (__timedwait -> pthread_mutex/cond,
//                      pthread_join on detach_state, __tl_sync). RELATIVE
//                      timespec (musl converts absolute deadlines itself; it
//                      never uses FUTEX_WAIT_BITSET/absolute on this path).
//   FUTEX_WAKE (1)     the wake, count 1 or INT_MAX (broadcast).
//   FUTEX_REQUEUE (3)  the cond wake-chain hand-off. For a DEFAULT mutex
//                      (_m_type==0) pthread_cond's unlock_requeue takes the
//                      plain-REQUEUE branch, so a broadcast with >=2 waiters
//                      DEADLOCKS if this op is unserved -- it is not optional.
// PI (LOCK_PI/UNLOCK_PI), WAKE_OP, CMP_REQUEUE and WAIT_BITSET are the opt-in
// robust/PI paths only; they FORWARD (a clean ENOSYS), not translate.
enum viv_futex_op {
    VIV_FUTEX_OP_WAIT,      // FUTEX_WAIT (base op 0)
    VIV_FUTEX_OP_WAKE,      // FUTEX_WAKE (base op 1)
    VIV_FUTEX_OP_REQUEUE,   // FUTEX_REQUEUE (base op 3)
};

// The flag bits stripped before the op switch. PRIVATE is irrelevant (all
// CLONE_THREAD peers share one AddrSpace, so torpor's (proc, addr) key already
// scopes the wait); CLOCK_REALTIME is stripped because the served WAIT treats
// its timeout as relative, which is what musl sends.
enum {
    VIV_FUTEX_PRIVATE_FLAG   = 0x80,
    VIV_FUTEX_CLOCK_REALTIME = 0x100,
};

// PURE. Strips the flag bits, classifies the base op. VIV_TRANSLATED with
// *op_out set for WAIT/WAKE/REQUEUE; VIV_FORWARD (-> the shell answers ENOSYS)
// for every other op. Fail-closed on a NULL out-param.
enum viv_verdict vivarium_futex_decide(u32 op, enum viv_futex_op *op_out);

// -----------------------------------------------------------------------------
// TIER 2 — `wait4` (LINEAGE L-6b). See docs/LINEAGE.md §5.5.
//
// The row that lets a Linux guest REAP what L-6a let it create. Its target is
// `wait_pid_for` (PTY-1e), which is already a POSIX `waitpid`: it has the pid /
// pgrp selectors, the non-blocking flag, and the stop/continue reports. So this
// is a MAP, not machinery — and the map is the whole point, because the option
// words look interchangeable and are not.
//
// THE COLLISION. Measured from `third_party/musl/include/sys/wait.h`:
//
//     Linux  WNOHANG 1   WUNTRACED 2   WEXITED 4   WCONTINUED 8
//     Thyla  WNOHANG 1   UNTRACED  2   CONTINUED 4
//
// The first two are the identity BY COINCIDENCE. The third is not, and the gap
// is occupied: Linux's `WEXITED` is 4, which is Thylacine's `WAIT_CONTINUED`.
// So a passthrough would be silently wrong in BOTH directions at once — a guest
// asking for WCONTINUED (8) sets a bit the native handler rejects as unknown,
// and a guest passing WEXITED (4) silently opts into continue-reports AND into
// the packed status encoding. Neither is a decline; both are answers that look
// plausible. That is why this row exists as a translator rather than a T1
// renumber, and why the map below is written out bit by bit.
//
// (WEXITED/WSTOPPED/WNOWAIT belong to `waitid`, not `wait4`. That makes the
// collision unlikely to fire from a correct program — but "unlikely" is not the
// standard this tier holds itself to, and the bit is REAL: musl defines it in
// the same header a guest includes, so a confused caller reaches it.)
//
// THE STATUS ENCODING IS ALREADY LINUX'S, which is the happy half. PTY-1e built
// `WAIT_STATUS_*` as "the Linux wait(2) layout so the Pouch boundary-line maps
// 1:1" (proc.h), and it checks out against musl's accessors:
//
//     WAIT_STATUS_EXITED(c) = (c & 0xff) << 8   <->  WEXITSTATUS(s) = (s & 0xff00) >> 8
//     WAIT_STATUS_STOPPED   = 0x7f | (20 << 8)  <->  WIFSTOPPED(0x147f) is true,
//                                                    WSTOPSIG -> 20 (SIGTSTP)
//     WAIT_STATUS_CONTINUED = 0xffff            <->  WIFCONTINUED(s) = (s == 0xffff)
//
// So no reshape is needed — EXCEPT that the kernel applies that encoding
// CONDITIONALLY. `wait_pid_for` packs only when the caller passed a PTY-1e flag,
// and returns the RAW exit status otherwise, "full compatibility for every
// pre-PTY caller" (proc.h). Linux always wants packed. So the translator packs
// exactly when the kernel did not — and it cannot decide that by inspecting the
// value, because a raw exit status of 5247 and a packed WAIT_STATUS_STOPPED are
// both 0x147f. It has to know what it ASKED for.
//
// WHY THIS DECIDE HANDS BACK A DESCRIPTION RATHER THAN A FLAG WORD. The obvious
// shape would be to return the native `WAIT_*` word directly. That would drag
// proc.h into this file, which the clone row already refused for RFPROC/RFMEM:
// the pure layer says WHAT WAS ASKED, and the shell — the one place that sees
// both ABIs — turns it into the native vocabulary.
//
// The split lands the risk in the right half. The dangerous direction is Linux
// bit 4 silently becoming WAIT_CONTINUED, and that is decided HERE, by an
// allow-list a unit test pins with no kernel plumbing at all. What is left for
// the shell is `.continued -> WAIT_CONTINUED`, a one-line assignment sitting
// directly above the `kernel_packs` derivation it must agree with, which is a
// tighter coupling than an out-param arriving from another file.
// -----------------------------------------------------------------------------

// Linux `wait4` option bits — musl `include/sys/wait.h`, read from the tree.
// WEXITED is listed precisely BECAUSE it is the collision; the four Linux-only
// bits are listed so that "declines" is a statement about named values rather
// than about whatever fell through.
enum {
    VIV_WNOHANG     = 1,
    VIV_WUNTRACED   = 2,          // == WSTOPPED
    VIV_WEXITED     = 4,          // waitid's; collides with WAIT_CONTINUED
    VIV_WCONTINUED  = 8,
    VIV_WNOWAIT     = 0x1000000,  // waitid's
    VIV_WNOTHREAD   = 0x20000000, // __WNOTHREAD
    VIV_WALL        = 0x40000000, // __WALL
    VIV_WCLONE      = 0x80000000, // __WCLONE
};

// The admitted option bits — an ALLOW-LIST, as VIV_MMAP_FLAGS_ADMITTED is, and
// for the same reason. Unlike the mmap and clone words this is a MASK rather
// than an exact value, because options genuinely compose: WNOHANG|WUNTRACED is
// an ordinary shell wait, whereas MAP_PRIVATE|MAP_ANON was the only mmap shape
// musl emits. A mask is the right shape when every subset is meaningful.
//
// __WALL / __WCLONE / __WNOTHREAD are excluded as a DOMAIN matter, not an
// oversight: all three discriminate thread-children from process-children, a
// distinction Thylacine's process table does not draw (a Thread is not a child).
// There is nothing to approximate them with.
#define VIV_WAIT_OPTS_ADMITTED \
    ((u32)(VIV_WNOHANG | VIV_WUNTRACED | VIV_WCONTINUED))

// Decide whether a `wait4` is inside the translatable domain, and compute the
// native flag word. PURE — no user memory, no Proc, no locks.
//
// `pid` is NOT judged here and is passed straight through, because
// `wait_pid_for`'s selectors ARE Linux's: -1 any, >0 that child, 0 the caller's
// group, <-1 the group -pid (proc.h). That correspondence is exact, so
// inspecting it would add a second opinion where there is only one rule.
//
// `rusage` must be 0. Filling it would mean inventing resource figures we do
// not collect per-child, and zeroing it would be a stored lie (a guest reading
// ru_utime would see a child that used no CPU). musl's `waitpid` and `wait`
// pass a literal 0 (`src/process/waitpid.c`), so the shell path and every
// ordinary reap are unaffected; only a deliberate `wait4(..., &ru)` declines.
// The prowl arc's per-Proc `run_ns` is the substrate a future row would use.
//
// On VIV_TRANSLATED, *out describes the request in Linux's own terms; the shell
// composes the native flag word from it.
//
// Returns VIV_TRANSLATED or VIV_FORWARD. Never ENOSYS: `wait4` exists.
struct viv_wait_opts {
    bool nohang;      // WNOHANG
    bool untraced;    // WUNTRACED / WSTOPPED
    bool continued;   // WCONTINUED  -- Linux bit 8, NOT Thylacine's bit 4
};

enum viv_verdict vivarium_wait4_decide(u64 options, u64 rusage,
                                       struct viv_wait_opts *out);

// -----------------------------------------------------------------------------
// writev (#150). The row the L-6c gate was actually blocked on: busybox's `echo`
// writes through writev, so with no translator the shell ran perfectly and
// printed nothing.
//
// IT MUST BE TIER 2, and this is the sharpest case yet of the ARITY RULE
// (section 4 / the ARCH section 25.4 row). A renumber onto SYS_WRITE lines up
// register for register -- writev(fd, iov, iovcnt) vs SYS_WRITE(fd, buf, len),
// three arguments each -- and would be catastrophically wrong: arg 1 is a
// POINTER TO AN ARRAY OF POINTERS, not a buffer, and arg 2 is an ENTRY COUNT,
// not a byte length. The kernel would write `iovcnt` bytes of the iovec array
// itself -- the guest's own pointers -- to the fd. The rule exists for exactly
// this: registers lining up is not arguments meaning the same thing.
#define VIV_UIO_MAXIOV 1024

// Linux `struct iovec` on aarch64: two 64-bit words, and the LP64 layout is
// fixed ABI. Named so the shell reads user memory in the shape it actually has
// rather than by open-coded offsets.
struct viv_linux_iovec {
    u64 base;
    u64 len;
};

_Static_assert(sizeof(struct viv_linux_iovec) == 16,
               "Linux aarch64 struct iovec is two 64-bit words");

// Linux `struct msghdr` on aarch64 (LP64 fixed ABI). recvmsg(212) reads it to
// find the scatter iovecs + the msg_name out-buffer, and writes back the three
// value-result fields (namelen, controllen, flags). The two pad words are the
// LP64 tail padding after socklen_t/int -- present in the on-wire struct, so the
// 56-byte size is exact and MUST match what the guest's libc lays out.
struct viv_linux_msghdr {
    u64 msg_name;         // @0   void *          -- sockaddr out-buffer (may be 0)
    u32 msg_namelen;      // @8   socklen_t       -- in: buf size; out: addr size
    u32 _pad0;            // @12
    u64 msg_iov;          // @16  struct iovec *
    u64 msg_iovlen;       // @24  size_t
    u64 msg_control;      // @32  void *          -- ancillary; ignored here
    u64 msg_controllen;   // @40  size_t          -- out: 0 (no cmsg served)
    s32 msg_flags;        // @48  int             -- out: 0 / MSG_TRUNC
    u32 _pad1;            // @52
};
_Static_assert(sizeof(struct viv_linux_msghdr) == 56,
               "Linux aarch64 struct msghdr is 56 bytes");

// Linux MSG_TRUNC (the recvmsg out-flag: the datagram was longer than the
// supplied buffer and the tail was discarded). arch-independent value.
enum { VIV_MSG_TRUNC = 0x20 };

// The largest datagram recvmsg reads in one call -- netd's per-connection UDP rx
// buffer (UDP_RX_BUF, server.rs). A DNS reply (<= 512, or <= 4096 with EDNS0)
// fits; a datagram past this is truncated + MSG_TRUNC. Bounds the recv bounce so
// a guest cannot ask the kernel to stage an unbounded buffer.
enum { VIV_RECV_DGRAM_MAX = 4096 };

// Linux `struct timeval` on aarch64: {s64 tv_sec; s64 tv_usec} (suseconds_t is
// `long`, hence 64-bit). It has NO native `struct t_*` twin -- Thylacine has no
// gettimeofday syscall -- so this is the only place its 16-byte size is pinned;
// gettimeofday's shell validates its user buffer against `sizeof` this.
struct viv_linux_timeval {
    s64 tv_sec;
    s64 tv_usec;
};

_Static_assert(sizeof(struct viv_linux_timeval) == 16,
               "Linux aarch64 struct timeval is two 64-bit words");

// Linux `struct timezone`: two `int`s, obsolete. gettimeofday zero-fills it when
// the caller passes a non-NULL pointer; pinned so the bound moves with the size.
struct viv_linux_timezone {
    s32 tz_minuteswest;
    s32 tz_dsttime;
};

_Static_assert(sizeof(struct viv_linux_timezone) == 8,
               "Linux struct timezone is two 32-bit ints");

// Bound the entry count. Split out as a pure decide because it carries the one
// real judgement in the row -- everything else writev does is uaccess and a loop
// over the existing byte-I/O core. Linux answers EINVAL for a count over
// UIO_MAXIOV and for a negative one (the argument is an `int`, so a huge u64
// with bit 31 set is negative on the Linux side; comparing as u64 catches both).
//
// Returns VIV_TRANSLATED with *out_count set, or VIV_FORWARD for out-of-domain.
// A count of 0 is IN domain and legal: Linux still validates the fd, so the
// shell issues a zero-length write rather than short-circuiting to 0.
enum viv_verdict vivarium_writev_decide(u64 iovcnt, u32 *out_count);

// Accumulate a total byte count with Linux's overflow rule. Linux rejects a
// writev whose lengths sum past SSIZE_MAX (it would make the return value
// indistinguishable from an error), and it checks this BEFORE writing anything.
// Returns false when the addition would break that bound.
bool vivarium_writev_accumulate(u64 *total, u64 add);

// -----------------------------------------------------------------------------
// fcntl (#151). A MULTIPLEXER, which is exactly the shape Thylacine's native ABI
// refuses -- and exactly the shape a Linux phenotype has to speak, which is why
// it lives here and has no native counterpart. Only the close-on-exec family is
// served; the rest decline.
//
// The served set is MEASURED, not guessed. Instrumenting the row and running
// Alpine busybox's /bin/sh showed it issues fcntl exactly twice at startup:
//
//     cmd 0x2   = F_SETFD,          arg 1  = FD_CLOEXEC
//     cmd 0x406 = F_DUPFD_CLOEXEC,  arg 10 = ash's savefd(), moving the script
//                                            fd above 10
//
// F_GETFD and F_DUPFD join them because each is the exact inverse/sibling of one
// of those, differing by a line; serving one of a pair and declining the other
// would be an arbitrary edge for a guest to discover at runtime.
//
// EVERY OTHER cmd declines with ENOSYS rather than Linux's EINVAL. EINVAL claims
// the cmd is not a valid fcntl operation, which for F_GETFL or F_SETLK is simply
// false; ENOSYS says the surface is absent, which is true and is what the row as
// a whole returned before any of it was served.
enum viv_fcntl_op {
    VIV_FCNTL_UNSERVED = 0,
    VIV_FCNTL_GETFD,       // read the descriptor's close-on-exec flag
    VIV_FCNTL_SETFD,       // write it; *cloexec_out carries the new value
    VIV_FCNTL_DUPFD,       // dup >= *min_fd_out; *cloexec_out is the new fd's flag
    VIV_FCNTL_GETFL,       // read the open-file status flags (access mode | O_NONBLOCK)
    VIV_FCNTL_SETFL,       // write them; the shell decodes O_NONBLOCK from arg itself
};

enum {
    VIV_F_DUPFD         = 0,
    VIV_F_GETFD         = 1,
    VIV_F_SETFD         = 2,
    VIV_F_GETFL         = 3,
    VIV_F_SETFL         = 4,
    VIV_F_DUPFD_CLOEXEC = 1030,
    VIV_FD_CLOEXEC      = 1,
};

// Classify an fcntl and extract its parameters. PURE -- no Proc, no table, no
// memory. Returns VIV_TRANSLATED with *op_out set (plus whichever of
// *cloexec_out / *min_fd_out that op uses), or VIV_FORWARD for an unserved cmd.
//
// RANGE VALIDATION OF min_fd IS NOT DONE HERE, deliberately: "is this fd index
// too large" is a question about PROC_HANDLE_MAX, i.e. about the handle table,
// and this layer does not know about tables. The shell answers it, the same way
// openat's shell owns the path measurement its decide refuses to do.
enum viv_verdict vivarium_fcntl_decide(u64 cmd, u64 arg,
                                       enum viv_fcntl_op *op_out,
                                       bool *cloexec_out, u64 *min_fd_out);

// -----------------------------------------------------------------------------
// ioctl terminal control (C2-k1, interactive git). A phenotype binary running
// an interactive git (commit / rebase -i / add -p) or any Linux TUI reaches the
// terminal ONLY through ioctl -- there is no separate termios syscall. The
// gate is isatty(), which musl implements as ioctl(fd, TIOCGWINSZ, &wsz) and
// reads as "true iff that succeeds" (third_party/musl/src/unistd/isatty.c); a
// flat ENOSYS makes isatty() false on EVERY fd, even a real terminal, so git
// silently drops to non-interactive defaults. This surface serves the TC*/TIOC*
// terminal family; the fd's tty-ness and the arg copy are the SHELL's job.
//
// The request codes are the asm-generic / aarch64 ioctl numbers (the whole
// Linux world shares these constants for the TC/TIOC family).
enum {
    VIV_TCGETS      = 0x5401,   // tcgetattr: read termios
    VIV_TCSETS      = 0x5402,   // tcsetattr TCSANOW: write termios now
    VIV_TCSETSW     = 0x5403,   // tcsetattr TCSADRAIN: write after output drains
    VIV_TCSETSF     = 0x5404,   // tcsetattr TCSAFLUSH: write after drain + input flush
    VIV_TIOCGWINSZ  = 0x5413,   // read struct winsize (isatty's actual probe)
    VIV_TIOCSWINSZ  = 0x5414,   // write struct winsize
};

// The classified op. The three TCSETS* forms COLLAPSE to VIV_IOCTL_TCSETS: the
// cons/pts line discipline applies a mode change immediately and has no separate
// termios output queue to drain or input queue to flush, so TCSANOW/DRAIN/FLUSH
// are indistinguishable at this layer (a documented, faithful-enough divergence).
enum viv_ioctl_op {
    VIV_IOCTL_UNSERVED = 0,
    VIV_IOCTL_TCGETS,       // fill *termios from the fd's line-discipline mode
    VIV_IOCTL_TCSETS,       // apply *termios to the fd's line discipline (all 3 TCSETS* forms)
    VIV_IOCTL_TIOCGWINSZ,   // fill *winsize from the fd's size
    VIV_IOCTL_TIOCSWINSZ,   // apply *winsize to the fd
};

// Classify a terminal ioctl request. PURE -- request code only; no fd, no Proc,
// no memory (the fd's tty-ness and the user-struct copy are the shell's job,
// exactly as openat's decide refuses the path measurement). Returns
// VIV_TRANSLATED with *op_out set, or VIV_FORWARD (op_out untouched) for a
// request this surface does not serve.
enum viv_verdict vivarium_ioctl_decide(u64 request, enum viv_ioctl_op *op_out);

// C2-k1b: the ioctl EXECUTION ABI. `struct viv_linux_termios` is EXACTLY what a
// Linux kernel writes for TCGETS and reads for TCSETS -- the asm-generic uapi
// layout (NCCS=19, no embedded speeds; termios2 is a separate request we do not
// serve). The guest's own libc struct termios is a >=36-byte superset whose
// first 36 bytes match this, so writing exactly 36 is what real Linux does and
// what the guest expects. c_iflag/c_oflag/c_lflag are u32; c_line is the line
// discipline byte (always N_TTY=0 here); c_cc[19] carries the control chars.
struct viv_linux_termios {
    u32 c_iflag;
    u32 c_oflag;
    u32 c_cflag;
    u32 c_lflag;
    u8  c_line;
    u8  c_cc[19];
};
_Static_assert(sizeof(struct viv_linux_termios) == 36,
               "viv_linux_termios == Linux asm-generic struct termios (36 bytes)");

// The Linux `struct winsize` (TIOCGWINSZ/TIOCSWINSZ). isatty() succeeds iff
// TIOCGWINSZ does, so serving this is what makes a phenotype fd a terminal.
struct viv_linux_winsize {
    u16 ws_row;
    u16 ws_col;
    u16 ws_xpixel;
    u16 ws_ypixel;
};
_Static_assert(sizeof(struct viv_linux_winsize) == 8,
               "viv_linux_winsize == Linux struct winsize (8 bytes)");

// The asm-generic termbits bit values -- the ABI constants the guest's termios
// carries. Only the five the cons/pts line discipline models are honored; every
// other bit is accept-and-ignore on TCSETS and reported 0 on TCGETS (the pouch
// PTY-3 "termios subset honesty", faithful for the flags we implement and stable
// under a guest's tcgetattr/modify/tcsetattr round-trip).
enum {
    VIV_LINUX_ICRNL  = 0x00000100u,  // c_iflag
    VIV_LINUX_OPOST  = 0x00000001u,  // c_oflag
    VIV_LINUX_ONLCR  = 0x00000004u,  // c_oflag (acts only under OPOST on Linux)
    VIV_LINUX_ISIG   = 0x00000001u,  // c_lflag
    VIV_LINUX_ICANON = 0x00000002u,  // c_lflag
    VIV_LINUX_ECHO   = 0x00000008u,  // c_lflag
    // A cosmetic-but-sane c_cflag baseline for TCGETS (B38400 | CS8 | CREAD);
    // git and the coreutils do not inspect the baud/char-size bits, but a zero
    // c_cflag reads as B0 (hang-up), which a stricter program could reject.
    VIV_LINUX_B38400 = 0x0000000fu,
    VIV_LINUX_CS8    = 0x00000030u,
    VIV_LINUX_CREAD  = 0x00000080u,
};

// C2-k1b PURE translation helpers (non-static for the unit tests, the
// getdents64-transform precedent): the error-prone flag/grammar logic, isolated
// from the fd + uaccess glue. viv_cons_to_linux_termios maps the cons 5-flag
// word to a Linux termios (TCGETS content); viv_linux_termios_to_grammar builds
// the deterministic consctl grammar from a Linux termios (TCSETS content),
// returning the byte count (g needs >= 64 bytes).
void viv_cons_to_linux_termios(u32 cons_flags, struct viv_linux_termios *out);
int  viv_linux_termios_to_grammar(const struct viv_linux_termios *tio, char *g);

// C2-k1b test shim: drive the ioctl shell without a phenotype dispatch. Mirrors
// viv_readv_for_test -- the non-static shell entry the test suite calls.
s64 viv_ioctl_for_test(struct Proc *p, u64 fd, u64 request, u64 argp);

// C2-k2 test shim: drive a session/pgrp TIER2 shell (SETSID/SETPGID) through the
// real viv_tier2 arm, so the ACCES->PERM errno remap is exercised on an explicit
// Proc. linux_num is VIV_LINUX_SETSID or VIV_LINUX_SETPGID; a0/a1 are pid/pgid.
s64 viv_session_for_test(struct Proc *p, u64 linux_num, u64 a0, u64 a1);

// -----------------------------------------------------------------------------
// uname (#150). A fabrication -- there is no underlying Thylacine call -- so the
// question is not HOW to translate but WHAT to claim, and a wrong answer here is
// the mistranslation the argument-domain rule exists to prevent: a guest that
// believes it is on a kernel it is not will take a code path we cannot serve.
//
// THE DECISION, field by field:
//
//   sysname    "Linux". The truthful answer WITHIN the phenotype -- the ABI the
//              guest sees IS Linux's, which is what a vivarium means. Claiming
//              "Thylacine" would send every `uname -s` check down an unknown-OS
//              path with no Thylacine support behind it, which is strictly worse
//              than the path it has.
//
//   release    "4.4.0", and this is the field the task flagged. No number is
//              honest: there is no Linux whose syscall surface matches ours,
//              because ours is a small subset of every version's. So the choice
//              is which direction to be wrong in, and LOW is safer -- a guest
//              that assumes little uses the oldest code paths, which are exactly
//              the ones translated here. 4.4 is picked as THE NEWEST KERNEL THAT
//              PROMISES NOTHING WE LACK: it predates statx (4.11, which we
//              FORWARD), io_uring (5.1), clone3 (5.3), openat2 (5.6),
//              faccessat2 (5.8) and close_range (5.9, also FORWARD) -- every
//              modern number this table declines. It also clears glibc's
//              minimum (3.2), below which a glibc binary aborts outright with
//              "FATAL: kernel too old" before main().
//
//   version    Carries "Thylacine" ON PURPOSE. Programs essentially never parse
//              this field -- it is the free-form build banner -- so it is where
//              `uname -a` can tell the truth without any version check tripping
//              over it. That split is the section 9 DEGRADED tier applied inside
//              a single struct: be compatible where a field is load-bearing, be
//              truthful where it is observable.
//
//   machine    "aarch64". Simply true.
//   nodename   "thylacine". There is no hostname concept to read.
//   domainname "(none)". Linux's own default.
//
// Linux's `struct new_utsname`: six fixed 65-byte NUL-terminated fields
// (__NEW_UTS_LEN 64, plus the terminator). 390 bytes, no padding, no alignment
// beyond a byte -- so the guest's `struct utsname` and this one are the same
// object.
#define VIV_UTS_FIELD_LEN 65

struct viv_linux_utsname {
    char sysname[VIV_UTS_FIELD_LEN];
    char nodename[VIV_UTS_FIELD_LEN];
    char release[VIV_UTS_FIELD_LEN];
    char version[VIV_UTS_FIELD_LEN];
    char machine[VIV_UTS_FIELD_LEN];
    char domainname[VIV_UTS_FIELD_LEN];
};

_Static_assert(sizeof(struct viv_linux_utsname) == 390,
               "Linux struct new_utsname is six 65-byte fields, densely packed");

// Fill a kernel-side utsname. Pure -- the shell copies it out. Zeroes the whole
// struct first, so every byte past each string is a defined 0 rather than
// whatever the caller's stack held (I-13: this struct is copied to EL0).
void vivarium_uname_fill(struct viv_linux_utsname *out);

// -----------------------------------------------------------------------------
// The identity reads (#150). getpid/getuid/getgid have exact native twins with
// matching arity and non-negative returns, so they are T1 rows -- with ONE
// translation that cannot live in a renumber, which is why getuid/getgid are
// shells instead:
//
// THE SENTINEL MAPPING. Thylacine's TCB identity is PRINCIPAL_SYSTEM ==
// 0xFFFFFFFE, and the container's shell runs as exactly that. Passed through
// raw, a Linux guest reads `(uid_t)-2` -- which in Linux practice is the
// historic "nobody"/nfsnobody value, i.e. the number that means the LEAST
// privileged identity. So the raw pass-through is not neutral: it inverts the
// fact being asked about, telling a Proc that holds the system identity that it
// is nobody. Mapping it to 0 (Unix root, the identity PRINCIPAL_SYSTEM
// corresponds to) is the faithful answer.
//
// SAFE BY CONSTRUCTION, not by luck: PRINCIPAL_INVALID and GID_INVALID are both
// 0, so 0 is not assignable to any real principal or group -- the mapping cannot
// collide with a genuine identity, and every other value passes through
// unchanged.
//
// CONFERS NOTHING. Every authority decision in Thylacine reads the real
// `principal_id` through perm_check or a CAP_* gate; the number a guest sees is
// informational to the guest alone. A container shell that believes it is root
// will ATTEMPT privileged operations and be refused at the real gates exactly as
// before -- the mapping changes what it is told, never what it may do (I-22).
u32 vivarium_map_uid(u32 principal_id);
u32 vivarium_map_gid(u32 gid);

// -----------------------------------------------------------------------------
// setuid / setgid (#150). Thylacine identity is set ONCE at spawn (via
// CAP_SET_IDENTITY) and is immutable on a running Proc, so there is nothing to
// translate a real change to. The disposition is therefore an errno choice, and
// the two candidates say different things:
//
//   ENOSYS -- "this system has no such concept". FALSE. Thylacine has a full
//             identity model; it just is not mutable in place.
//   EPERM  -- "you may not do this". TRUE, and it is also what Linux itself
//             answers an unprivileged process, so a guest's existing
//             drop-privilege fallback runs unchanged.
//
// With one exception that must not be collapsed into the refusal: setuid(getuid())
// SUCCEEDS on Linux -- it is the idempotent no-op every "drop to my own uid"
// path performs -- and refusing it would break callers that are asking for
// nothing. So the identity-preserving call succeeds and every other one is EPERM.
// The comparison is made in the GUEST's number space (after vivarium_map_uid),
// because that is the only value the guest has ever been shown.
//
// Returns true when the call is the no-op (the shell answers 0).
bool vivarium_setid_is_noop(u32 requested, u32 current_mapped);

// clock_gettime(clk_id, tp): the PURE clk_id map. Maps a Linux clockid_t onto a
// native T_CLOCK_* and nothing else -- the timespec is byte-identical, so the
// number map is the whole translation. Returns false for a clk_id with no
// Thylacine clock (the shell answers -EINVAL). See the impl for the per-id claims.
bool vivarium_clock_gettime_map(u64 linux_clk_id, u64 *thyla_clk_id_out);

// -----------------------------------------------------------------------------
// pipe2 (#155, LINEAGE L-6c). A shell cannot build a pipeline without it, and on
// aarch64 there is no second way to ask: Linux's generic syscall table has no
// legacy `pipe`, so musl's pipe() IS `syscall(SYS_pipe2, fd, 0)` -- confirmed in
// the arch table (only __NR_pipe2 59; x86_64 by contrast carries both 22 and
// 293, which is why the architecture had to be checked rather than inherited).
//
// THE ARGUMENT DOMAIN, measured off the gate's own busybox rather than reasoned
// from what Linux permits. Six call sites reach the number:
//
//   four through musl's pipe(), which hardcodes `mov x1, #0`     -> flags 0
//   two through musl's pipe2(), both `mov w1, #0x80000`          -> O_CLOEXEC
//
// So {0, O_CLOEXEC} is not a conservative subset of the domain, it IS the
// domain, and both members are reproducible EXACTLY: 0 is SYS_PIPE unchanged,
// and O_CLOEXEC is the descriptor flag #151 built -- the same handle_set_cloexec
// openat's shell already calls, applied to both new descriptors.
//
// AN ALLOW-LIST, not a deny-list, for the reason V-2d recorded when mmap made
// the same choice: aarch64 defines flags a deny-list silently admits. Linux's
// pipe2 also takes O_DIRECT (packet mode) and O_NONBLOCK, and NEITHER is
// representable here -- devpipe has no packet framing and no non-blocking read
// -- so admitting them by omission would be the mistranslation the rule exists
// to prevent: a guest told its pipe is non-blocking, whose next read blocks.
//
// DECLINING O_CLOEXEC WOULD ALSO HAVE WORKED, and it is worth recording why it
// is not what happens, because the reasoning inverted once during the work.
// musl's pipe2 has its own ENOSYS fallback -- pipe() then fcntl(F_SETFD) -- and
// since #151 made fcntl a served row, that fallback now runs correctly rather
// than silently dropping the flag as it would have a chunk ago. So declining is
// not unsafe. It is merely worse: it costs three syscalls instead of one, and it
// leans on a compat shim that belongs to one libc. A statically-linked Go binary
// calling pipe2 directly gets no such fallback. Serving what we can exactly
// serve is the answer that does not depend on who is asking.
//
// PURE, like every other decide: no Proc, no table, no memory. The int[2]
// copy-out is the shell's business. Returns VIV_TRANSLATED with *cloexec_out
// set, or VIV_FORWARD for any flag outside the domain (leaving *cloexec_out
// false, so a caller that ignores the verdict cannot act on a stale true).
enum viv_verdict vivarium_pipe2_decide(u64 flags, bool *cloexec_out);

// -----------------------------------------------------------------------------
// dup3 (#157, LINEAGE L-6c). The pipeline's second blocker, and reached the same
// way pipe2 was: aarch64 HAS NO `dup2` NUMBER (the arch table defines only
// __NR_dup3 24), so musl's dup2.c compiles `dup2(old, new)` into
// `__syscall(SYS_dup3, old, new, 0)`. A shell's redirection plumbing is dup2, so
// there is no route to a pipeline that does not pass through this number.
//
// THE ARGUMENT DOMAIN, measured off the gate's own busybox. Four call sites:
//
//   0x4d3ca0  musl dup2()       `mov x2, #0`     -> flags 0
//   0x4d3d0c  musl __dup3()     `sxtw x2, w2`    -> the caller's flags
//   0x4db7d8  busybox internal  `mov x2, #0`     -> flags 0
//   0x4db8cc  busybox internal  `mov x2, #0`     -> flags 0
//
// AND THIS ROW'S DOMAIN IS COMPLETE, WHICH PIPE2'S WAS NOT. Linux's own dup3
// rejects everything outside {0, O_CLOEXEC} with EINVAL, so our allow-list is
// not a subset of what Linux serves -- it is EQUAL to it. pipe2 had to decline
// O_DIRECT and O_NONBLOCK, which Linux genuinely serves and devpipe cannot
// represent; here there is nothing of the kind, and the row's degraded tier is
// entirely about STATE (the socket case below), never about arguments.
//
// That equality is why an out-of-domain flags word gets **EINVAL, not ENOSYS**.
// The distinction is munmap's (V-2d): Linux's own argument errors are
// reproduced, because a decline would replace a specific errno the guest can
// act on with "this surface is absent", which here would be false -- we serve
// this call, and we serve `dup3(a, b, O_NONBLOCK)` exactly as Linux does, by
// refusing it. So the decide is a predicate over the SERVED PAIR and the shell
// supplies EINVAL for the rest.
//
// TWO THINGS MEASURED IN musl's dup2.c THAT CONSTRAIN THE IMPLEMENTATION:
//
//   * `old == new` never reaches the kernel from dup2 -- musl checks it first
//     and returns via fcntl(old, F_GETFD). (dup3 itself must still answer
//     EINVAL, which is the documented dup2/dup3 difference, and that path is
//     reachable from a direct dup3 call.) It is also why #151's fcntl row is
//     load-bearing for `dup2(x, x)`, which nothing else would have shown.
//   * musl RETRIES ON -EBUSY, in a bare `while` loop, on BOTH paths. EBUSY is a
//     Linux-internal race return; this row must never produce it, because a
//     guest would spin forever rather than see an error. Nothing here does --
//     but a future refusal must not reach for that errno.
//
// THE SOCKET CASE, and why it DECLINES rather than half-serving. On Linux both
// descriptors name one `struct file`, so both stay fully usable sockets.
// Thylacine's socktab keys `(proto, N, state)` on the FD NUMBER, one entry per
// fd, unrefcounted, so there are exactly three things dup3 could do when `old`
// carries an entry, and two of them are wrong:
//
//   COPY the entry     -> two INDEPENDENT state machines over one connection. A
//                         connect() on fd A advances A to CONNECTED and swaps
//                         its handle ctl->data, while B still names `ctl` and
//                         still believes it is FRESH. Not "the same
//                         description" -- a divergence, actively wrong.
//   OMIT the entry     -> B is a plain fd on whatever Spoor A held. If A was
//                         connected this is nearly right (read/write work, since
//                         B names `data`), but connect/bind/listen/getsockname
//                         on B fail. "Reads fine, getpeername says EBADF" is the
//                         silent half-service the argument-domain rule exists to
//                         forbid.
//   DECLINE            -> ENOSYS, reported, reversible, and visible to whoever
//                         next needs it.
//
// Declining is what happens. Reproducing Linux exactly needs a REFCOUNTED
// socktab entry -- a real change to a table V-5 audited -- and that decision
// belongs in a chunk that is about it. Note the cost is bounded and known: a
// shell's dup2 is for files and pipes, and the idiom this turns away is the
// inetd shape (`dup2(connfd, 0); dup2(connfd, 1)`), recorded in VIVARIUM.md
// section 9's DEGRADED tier.
//
// THE DESTINATION IS A DIFFERENT QUESTION AND IS SERVED. dup3 CLOSES `new`, so
// if `new` carries a socktab entry that entry must be dropped -- the fd-freeing
// obligation at the top of this header. It is paid inside the shell rather than
// the dispatch hook, because dup3 can be refused while `new` is a live socket
// and an entry-time drop would destroy socket state on a call that failed.
//
// PURE, like every other decide: no Proc, no table, no memory. Returns
// VIV_TRANSLATED with *cloexec_out set for {0, O_CLOEXEC}, or VIV_FORWARD for
// any other flags word (leaving *cloexec_out false, so a caller that ignores
// the verdict cannot act on a stale true). The fd arguments are STATE, not
// arguments in this sense, and are the shell's business.
enum viv_verdict vivarium_dup3_decide(u64 flags, bool *cloexec_out);

#endif // THYLACINE_VIVARIUM_H
