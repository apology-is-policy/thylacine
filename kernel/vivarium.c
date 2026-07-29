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
//   openat  — NOT a renumber. Linux passes a NUL-terminated path; SYS_OPEN wants
//             an explicit path_len (syscall.h:1340), so translating means
//             SCANNING user memory for the terminator. Add AT_FDCWD ->
//             SYS_WALK_OPEN_FROM_ROOT and O_* -> OREAD/OWRITE/ORDWR/OEXEC and it
//             is a real translator. Total and stateless still — so it is
//             admissible as a T2 row, just not as a T1 one. (V-2b.)
//   fstat   — NOT a renumber. `struct t_stat` is 88 bytes (syscall.h:2143);
//             Linux aarch64 `struct stat` is 128 with a different field order.
//             The translation is a struct conversion written to user memory.
//             Again total + stateless => T2, not T1. (V-2b.)
//   mmap    — FORWARD. addr hints, PROT_*, MAP_FIXED/ANONYMOUS/PRIVATE and
//             fd-backed mappings are POLICY. SYS_BURROW_ATTACH_LAZY takes a
//             length and nothing else. This is the "needs judgement" case the
//             rule exists to exclude.
//   munmap  — FORWARD, and this is the instructive one. The arguments line up
//             perfectly — munmap(addr, len) vs SYS_BURROW_DETACH(vaddr, length)
//             — and it LOOKS like a free row. It is not: burrow_detach requires
//             an exact VMA match and explicitly refuses a partial detach
//             (syscall.h:611-620), while Linux permits partial and
//             multi-mapping unmaps. The renumber would be silently WRONG for a
//             legal class of inputs, which is exactly what "total" forbids.
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
    { VIV_LINUX_OPENAT, VIV_FORWARD },  // -> T2 at V-2b (path len + O_* + AT_FDCWD)
    { VIV_LINUX_FSTAT,  VIV_FORWARD },  // -> T2 at V-2b (88B t_stat -> 128B stat)
    { VIV_LINUX_MMAP,   VIV_FORWARD },  // policy: addr/prot/flags/fd-backing
    { VIV_LINUX_MUNMAP, VIV_FORWARD },  // NOT total: burrow_detach is exact-match
    { VIV_LINUX_BRK,    VIV_ENOSYS  },  // no counterpart; libc falls back to mmap
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
