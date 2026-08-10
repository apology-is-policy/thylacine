// ELF64 ARM64 loader implementation (P2-Ga).
//
// Per ARCHITECTURE.md §6.4 + §11.2 + §28 invariant I-12 (W^X). Parses
// + validates an ELF blob in memory; rejects malformed inputs and
// W^X-violating segments; produces a structured segment list.
//
// At v1.0 P2-Ga the loader is parse-only — it does NOT map segments
// into an address space. Phase 3 wires the actual mapping (BURROW-backed
// segment + page fault demand paging). Phase 5+ adds the exec()
// syscall surface that calls into this loader.
//
// W^X enforcement (ARCH §28 I-12): the loader rejects any PT_LOAD
// segment with both PF_W and PF_X set in its p_flags. This is one of
// three layers (PTE bits + mprotect + ELF loader); each layer
// independently catches a class of violation.

#include <thylacine/elf.h>
#include <thylacine/page.h>
#include <thylacine/types.h>

// Sane upper bound on phnum. Real binaries have 5-15 program headers;
// 256 is well past anything the linker would emit, and bounds the
// validation loop's total work.
#define ELF_MAX_PHNUM 256

// Wraparound-safe addition of u64 + u64 with overflow detection. Sets
// *out to the sum; returns true on overflow.
static bool u64_add_overflow(u64 a, u64 b, u64 *out) {
    if (a > ((u64)-1) - b) return true;
    *out = a + b;
    return false;
}

// Wraparound-safe multiplication of u32 * u32 to u64.
static u64 u32_mul_widen(u32 a, u32 b) {
    return (u64)a * (u64)b;
}

int elf_load(const void *blob, size_t size, struct elf_image *out) {
    if (!blob || !out) return ELF_LOAD_NULL_INPUT;

    // R5-G F70 close: zero `*out` on entry. Partial-population on
    // early-exit failure paths now leaves a defined-zero state rather
    // than attacker-controlled data. The contract still says "ignore
    // *out on non-OK return," but defensive zeroing eliminates a
    // class of confused-deputy bugs in future callers.
    {
        u8 *out_bytes = (u8 *)out;
        for (size_t i = 0; i < sizeof(*out); i++) out_bytes[i] = 0;
    }

    // R5-G F61 close: alignment precondition. The cast `blob` →
    // `const struct Elf64_Ehdr *` is undefined behavior if the pointer
    // is not at least 8-byte aligned (struct's natural alignment).
    // UBSan -fsanitize=alignment traps on this; production codegen may
    // assume alignment in vector / LDP loads. Reject up front.
    if (((uintptr_t)blob) % _Alignof(struct Elf64_Ehdr) != 0)
        return ELF_LOAD_BAD_ALIGN;

    if (size < sizeof(struct Elf64_Ehdr)) return ELF_LOAD_TOO_SMALL;

    const u8 *bytes = (const u8 *)blob;
    const struct Elf64_Ehdr *eh = (const struct Elf64_Ehdr *)blob;

    // -----------------------------------------------------------------
    // Stage 1: e_ident validation.
    // -----------------------------------------------------------------
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) {
        return ELF_LOAD_BAD_MAGIC;
    }
    if (eh->e_ident[EI_CLASS]   != ELFCLASS64)    return ELF_LOAD_BAD_CLASS;
    if (eh->e_ident[EI_DATA]    != ELFDATA2LSB)   return ELF_LOAD_BAD_DATA;
    if (eh->e_ident[EI_VERSION] != EV_CURRENT)    return ELF_LOAD_BAD_VERSION;
    if (eh->e_ident[EI_OSABI] != ELFOSABI_NONE &&
        eh->e_ident[EI_OSABI] != ELFOSABI_GNU)    return ELF_LOAD_BAD_OSABI;

    // -----------------------------------------------------------------
    // Stage 2: e_type / e_machine / e_version.
    // -----------------------------------------------------------------
    // D-2: ET_DYN joins ET_EXEC. The difference is entirely positional --
    // a PIE's p_vaddr are offsets from a base the loader picks -- so it is
    // captured by one bias applied below and nothing else in this function
    // branches on the type again.
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return ELF_LOAD_BAD_TYPE;
    if (eh->e_machine != EM_AARCH64)  return ELF_LOAD_BAD_MACHINE;
    if (eh->e_version != EV_CURRENT)  return ELF_LOAD_BAD_FILE_VER;

    const bool is_pie = (eh->e_type == ET_DYN);
    const u64  bias   = is_pie ? ELF_PIE_LOAD_BIAS : 0;

    // -----------------------------------------------------------------
    // Stage 3: program-header table validation.
    // -----------------------------------------------------------------
    if (eh->e_phentsize != sizeof(struct Elf64_Phdr))
        return ELF_LOAD_BAD_PHENTSIZE;
    if (eh->e_phnum == 0)
        return ELF_LOAD_NO_PHDRS;
    if (eh->e_phnum > ELF_MAX_PHNUM)
        return ELF_LOAD_TOO_MANY_PHDRS;

    // phoff + phnum * phentsize <= size, with overflow protection.
    u64 phtab_bytes = u32_mul_widen(eh->e_phnum, eh->e_phentsize);
    u64 phtab_end;
    if (u64_add_overflow(eh->e_phoff, phtab_bytes, &phtab_end))
        return ELF_LOAD_PHTAB_OOB;
    if (phtab_end > size)
        return ELF_LOAD_PHTAB_OOB;

    // R5-G F62 close: e_phoff must be 8-byte aligned (the natural
    // alignment of struct Elf64_Phdr). An attacker-controlled odd
    // phoff would misalign the Phdr-table cast below; UBSan BRKs on
    // misaligned struct loads. Real linkers always emit phoff =
    // sizeof(Elf64_Ehdr) = 64, naturally aligned.
    if (eh->e_phoff % _Alignof(struct Elf64_Phdr) != 0)
        return ELF_LOAD_PHTAB_OOB;

    // -----------------------------------------------------------------
    // Stage 4: per-segment validation. Iterate program headers,
    // collect PT_LOAD entries, enforce W^X + bounds + interp/stack
    // policy.
    // -----------------------------------------------------------------
    // Biased below, once the segments are collected -- keep the raw value
    // here so the in-segment search compares like with like.
    out->entry      = eh->e_entry;
    out->load_bias  = bias;
    out->type       = eh->e_type;
    // Program-header table location — consumed by exec_setup to build the
    // AT_PHDR / AT_PHENT / AT_PHNUM auxv entries. All three are already
    // validated above (phentsize == sizeof(Elf64_Phdr); phoff + phnum *
    // phentsize within `size`; phoff aligned).
    out->phoff      = eh->e_phoff;
    out->phnum      = eh->e_phnum;
    out->phentsize  = eh->e_phentsize;
    out->n_segments = 0;

    const struct Elf64_Phdr *ph = (const struct Elf64_Phdr *)(bytes + eh->e_phoff);

    for (u16 i = 0; i < eh->e_phnum; i++) {
        const struct Elf64_Phdr *p = &ph[i];

        // R5-G F64 close: hoist W^X check ABOVE the switch. The
        // invariant should be type-blind — every segment with a
        // p_flags field must be flag-checked, regardless of p_type.
        // Future segment types (PT_AARCH64_*, PT_GNU_PROPERTY, etc.)
        // get the check for free; the check no longer depends on
        // remembering to validate per-case.
        //
        // Mask off OS-specific (PF_MASKOS) + proc-specific (PF_MASKPROC)
        // bits; check architectural PF_W & PF_X only. ARCH §28 I-12.
        {
            u32 wx_bits = p->p_flags & (PF_W | PF_X);
            if (wx_bits == (PF_W | PF_X)) {
                return ELF_LOAD_RWX_REJECTED;
            }
        }

        switch (p->p_type) {
        case PT_LOAD:
            if (p->p_filesz > p->p_memsz)
                return ELF_LOAD_BAD_FILESZ;

            // file_offset + filesz <= size, with overflow protection.
            {
                u64 seg_end;
                if (u64_add_overflow(p->p_offset, p->p_filesz, &seg_end))
                    return ELF_LOAD_SEG_OOB;
                if (seg_end > size)
                    return ELF_LOAD_SEG_OOB;
            }

            if (out->n_segments >= ELF_MAX_LOAD_SEGMENTS)
                return ELF_LOAD_TOO_MANY_LOADS;

            {
                struct elf_load_segment *seg = &out->segments[out->n_segments];
                // D-2: the ONE place the PIE bias enters. Everything that
                // reads seg->vaddr afterwards -- here, in exec.c's mapper, in
                // the AT_PHDR translation -- sees a final address and needed
                // no change. `bias` is 0 for ET_EXEC, so that path is byte-
                // identical to before.
                //
                // Bound the biased span inside the PIE window. Without this a
                // hostile p_vaddr could place a segment past the window and
                // into (or beyond) the burrow-attach range; vma_insert and
                // burrow_map would still refuse to break isolation, but the
                // refusal would come from a general allocator rule rather than
                // from the loader saying what it will and will not place.
                if (bias != 0) {
                    u64 seg_top;
                    if (u64_add_overflow(p->p_vaddr, p->p_memsz, &seg_top))
                        return ELF_LOAD_PIE_OOB;
                    if (u64_add_overflow(seg_top, bias, &seg_top))
                        return ELF_LOAD_PIE_OOB;
                    if (seg_top > ELF_PIE_LOAD_LIMIT)
                        return ELF_LOAD_PIE_OOB;
                }
                seg->vaddr       = p->p_vaddr + bias;
                seg->file_offset = p->p_offset;
                seg->filesz      = p->p_filesz;
                seg->memsz       = p->p_memsz;
                // Mask out OS/proc-specific bits; keep only the
                // architectural permission bits. PF_W & PF_X have
                // already been verified non-coincident.
                seg->flags       = p->p_flags & (PF_R | PF_W | PF_X);
                out->n_segments++;
            }
            break;

        case PT_INTERP:
            // The kernel loads exactly ONE image per exec and runs no
            // interpreter. DISTRO D-4's rewrite-to-ldso route reads this
            // segment at the vivarium exec chokepoint and restarts
            // resolution on the interpreter, so what reaches elf_load is
            // the interpreter itself -- which has no PT_INTERP of its own
            // (measured: stock ld-musl-aarch64.so.1 carries none). This
            // reject therefore stays correct on both sides of D-4.
            return ELF_LOAD_HAS_INTERP;

        case PT_DYNAMIC:
            // R5-G F63 rejected this outright as the stronger dynamic-binary
            // indicator (PT_INTERP is only the loader path). D-2 narrows the
            // rejection to ET_EXEC, because every PIE carries a PT_DYNAMIC --
            // a static-PIE for its self-applied RELA table, stock ldso for its
            // own. The table is still never PROCESSED here: both self-relocate
            // from their entry point before executing anything that depends on
            // it, so accepting the segment costs the loader nothing.
            //
            // An ET_EXEC with a PT_DYNAMIC is still refused, unchanged: it
            // wants an interpreter this loader does not run, and its
            // relocations have no self-applying startup path.
            if (!is_pie) return ELF_LOAD_HAS_DYNAMIC;
            break;

        case PT_GNU_STACK:
            // The stack permissions segment. Linkers emit this with
            // p_flags = PF_R | PF_W (NX stack); a binary with PF_X here
            // requests an executable stack — ARCH §24 NX-stack policy
            // rejects this.
            if (p->p_flags & PF_X)
                return ELF_LOAD_EXEC_STACK;
            break;

        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_TLS:
        case PT_GNU_RELRO:
            // Skipped -- all three describe memory some PT_LOAD already
            // covers. PT_GNU_RELRO asks for a post-relocation re-protect
            // the phenotype answers with ENOSYS (musl tolerates that
            // specifically, dynlink.c:855,1428 -- RELRO degrades, which is
            // no loss against a status quo that had none). PT_PHDR is
            // auxv-relevant only; AT_PHDR is derived from e_phoff instead,
            // so a PIE without one (stock ldso) still resolves.
            break;

        default:
            // Unknown PT_* — silently skip per System V gABI guidance
            // (loaders must ignore unknown types they don't recognize).
            // The W^X check above already filtered any PF_W|PF_X
            // combination, regardless of p_type.
            break;
        }
    }

    if (out->n_segments == 0)
        return ELF_LOAD_NO_PHDRS;

    // -----------------------------------------------------------------
    // Stage 5: e_entry within some PT_LOAD segment's vaddr range.
    // -----------------------------------------------------------------
    // D-2: bias the entry here so the comparison below is against the
    // already-biased segment vaddrs, and so the zero-check tests the
    // address control actually transfers to. For ET_EXEC bias is 0, so
    // this is the old `eh->e_entry == 0` unchanged; for a PIE, e_entry 0
    // is a legitimate "entry at the load base" and the biased value is
    // never 0, which is the correct reading of the same rule.
    out->entry = eh->e_entry + bias;
    if (out->entry == 0)
        return ELF_LOAD_BAD_ENTRY;
    {
        bool entry_in_segment = false;
        for (int i = 0; i < out->n_segments; i++) {
            const struct elf_load_segment *s = &out->segments[i];
            // Entry must be in vaddr..vaddr+memsz; an X segment is
            // required for entry to be valid (caller can re-check if
            // they care). At v1.0 we accept any segment containing it;
            // Phase 3+ exec() should additionally verify it's an
            // executable segment.
            u64 seg_end;
            if (u64_add_overflow(s->vaddr, s->memsz, &seg_end))
                continue;
            if (out->entry >= s->vaddr && out->entry < seg_end) {
                entry_in_segment = true;
                break;
            }
        }
        if (!entry_in_segment)
            return ELF_LOAD_BAD_ENTRY;
    }

    return ELF_LOAD_OK;
}

// ---------------------------------------------------------------------
// VIVARIUM V-1: the ADVISORY brand hint (docs/VIVARIUM.md section 12.1).
// ---------------------------------------------------------------------
//
// PURE + read-only + bounds-safe. It NEVER decides a phenotype -- the Q3
// resolution is binding: a Proc is never INFERRED into a non-default ABI,
// only DECLARED into one by its vivarium. This exists so an obvious
// mismatch (a Linux-interp binary exec'd OUTSIDE a vivarium) earns a
// diagnostic + a clean failure instead of a silent mis-decode.
//
// PT_INTERP naming a Linux loader is the ONLY signal trusted here.
// EI_OSABI is DELIBERATELY NOT CONSULTED, and that omission is the whole
// point of Q3: ELFOSABI_GNU(3) == ELFOSABI_LINUX(3) is emitted by Clade
// for NATIVE Thylacine output, so keying on it would mis-brand our own
// toolchain's binaries; and ELFOSABI_NONE(0) is carried by native AND by
// musl-static Linux binaries alike. The byte identifies nothing in either
// direction -- do NOT "improve" this function by consulting it.
//
// Consequence, and it is correct: the v1.0 target (a musl-STATIC Linux
// binary, which has no PT_INTERP at all) hints UNKNOWN. That is the
// designed answer -- its vivarium declares it.

// A tiny bounded substring test (no libc in the kernel; `hay` is already
// proven NUL-terminated within its own extent by the caller).
static bool brand_contains(const char *hay, size_t hay_len, const char *needle) {
    size_t n = 0;
    while (needle[n] != '\0') n++;
    if (n == 0 || n > hay_len) return false;
    for (size_t i = 0; i + n <= hay_len; i++) {
        size_t j = 0;
        while (j < n && hay[i + j] == needle[j]) j++;
        if (j == n) return true;
    }
    return false;
}

// DISTRO D-4: the bounded PT_INTERP walk, now the SHARED one -- elf_brand_hint
// below reads its answer out of this rather than repeating the bounds logic.
// Splitting it was not a tidiness move: the D-4 rewrite ACTS on the extracted
// path (it resolves and execs it), so a second copy of "is this offset inside
// the prefix, is this string terminated" would be a second place for that
// judgement to be subtly weaker. #140 is the standing demonstration of what
// two copies of one walk cost.
size_t elf_read_interp(const void *blob, size_t size, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';
    if (!blob || size < sizeof(struct Elf64_Ehdr)) return 0;

    const u8 *bytes = (const u8 *)blob;
    const struct Elf64_Ehdr *eh = (const struct Elf64_Ehdr *)blob;

    // Only inspect something that is plausibly an aarch64 ELF64 at all; a
    // non-ELF blob has no interpreter to report (never a verdict from garbage).
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (eh->e_ident[EI_DATA]  != ELFDATA2LSB) return 0;

    if (eh->e_phentsize != sizeof(struct Elf64_Phdr)) return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHNUM) return 0;

    // The phdr table must lie wholly inside the buffer we were handed. The
    // buffer may be a bounded PREFIX of the file (REVENANT's header read), so
    // "not present" is a legitimate, common answer -- never an error.
    u64 ph_off   = eh->e_phoff;
    u64 ph_bytes = (u64)eh->e_phnum * (u64)sizeof(struct Elf64_Phdr);
    if (ph_off > size || ph_bytes > (u64)size - ph_off) return 0;

    const struct Elf64_Phdr *ph = (const struct Elf64_Phdr *)(bytes + ph_off);
    for (u16 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_INTERP) continue;

        u64 off = ph[i].p_offset;
        u64 fsz = ph[i].p_filesz;
        if (fsz == 0 || off > size || fsz > (u64)size - off)
            return 0;                   // interp not in our prefix

        const char *interp = (const char *)(bytes + off);
        size_t n = 0;
        while (n < (size_t)fsz && interp[n] != '\0') n++;
        if (n == (size_t)fsz) return 0; // unterminated: untrusted

        // An EMPTY interp ("" -- a zero-length but terminated string) names
        // nothing; treat it as absent rather than resolving the empty path.
        if (n == 0) return 0;
        // Longer than we will carry: absent, not truncated. A truncated
        // interpreter path resolves to a DIFFERENT file, which is the one
        // outcome worse than refusing.
        if (n > ELF_INTERP_MAX || n + 1 > out_cap) return 0;

        for (size_t j = 0; j < n; j++) out[j] = interp[j];
        out[n] = '\0';
        return n;
    }

    return 0;                           // static: the v1.0 target, by design
}

enum elf_brand elf_brand_hint(const void *blob, size_t size) {
    char interp[ELF_INTERP_MAX + 1];
    size_t n = elf_read_interp(blob, size, interp, sizeof(interp));
    if (n == 0) return ELF_BRAND_UNKNOWN;

    // Both the glibc and musl aarch64 loaders, by substring so a distro's
    // /lib64 or multiarch path still matches.
    if (brand_contains(interp, n, "ld-linux") ||
        brand_contains(interp, n, "ld-musl"))
        return ELF_BRAND_LINUX_LIKELY;
    return ELF_BRAND_UNKNOWN;           // some other interpreter: no verdict
}
