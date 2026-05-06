// ELF64 ARM64 loader implementation (P2-Ga).
//
// Per ARCHITECTURE.md §6.4 + §11.2 + §28 invariant I-12 (W^X). Parses
// + validates an ELF blob in memory; rejects malformed inputs and
// W^X-violating segments; produces a structured segment list.
//
// At v1.0 P2-Ga the loader is parse-only — it does NOT map segments
// into an address space. Phase 3 wires the actual mapping (VMO-backed
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
    if (eh->e_ident[EI_OSABI]   != ELFOSABI_NONE) return ELF_LOAD_BAD_OSABI;

    // -----------------------------------------------------------------
    // Stage 2: e_type / e_machine / e_version.
    // -----------------------------------------------------------------
    if (eh->e_type    != ET_EXEC)     return ELF_LOAD_BAD_TYPE;
    if (eh->e_machine != EM_AARCH64)  return ELF_LOAD_BAD_MACHINE;
    if (eh->e_version != EV_CURRENT)  return ELF_LOAD_BAD_FILE_VER;

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

    // -----------------------------------------------------------------
    // Stage 4: per-segment validation. Iterate program headers,
    // collect PT_LOAD entries, enforce W^X + bounds + interp/stack
    // policy.
    // -----------------------------------------------------------------
    out->entry      = eh->e_entry;
    out->n_segments = 0;

    const struct Elf64_Phdr *ph = (const struct Elf64_Phdr *)(bytes + eh->e_phoff);

    for (u16 i = 0; i < eh->e_phnum; i++) {
        const struct Elf64_Phdr *p = &ph[i];

        switch (p->p_type) {
        case PT_LOAD:
            // W^X enforcement: PF_W and PF_X cannot both be set. ARCH
            // §28 I-12. Mask off OS/proc-specific bits; check the
            // architectural bits only.
            {
                u32 wx_bits = p->p_flags & (PF_W | PF_X);
                if (wx_bits == (PF_W | PF_X)) {
                    return ELF_LOAD_RWX_REJECTED;
                }
            }

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
                seg->vaddr       = p->p_vaddr;
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
            // Dynamic binaries / PIE deferred to Phase 5+. v1.0 only
            // accepts statically-linked ET_EXEC.
            return ELF_LOAD_HAS_INTERP;

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
        case PT_DYNAMIC:
        case PT_TLS:
        case PT_GNU_RELRO:
            // Skipped at v1.0. PT_DYNAMIC + PT_TLS + PT_GNU_RELRO need
            // dynamic-linker support. PT_PHDR is auxv-relevant only.
            break;

        default:
            // Unknown PT_* — silently skip per System V gABI guidance
            // (loaders must ignore unknown types they don't recognize).
            break;
        }
    }

    if (out->n_segments == 0)
        return ELF_LOAD_NO_PHDRS;

    // -----------------------------------------------------------------
    // Stage 5: e_entry within some PT_LOAD segment's vaddr range.
    // -----------------------------------------------------------------
    if (eh->e_entry == 0)
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
            if (eh->e_entry >= s->vaddr && eh->e_entry < seg_end) {
                entry_in_segment = true;
                break;
            }
        }
        if (!entry_in_segment)
            return ELF_LOAD_BAD_ENTRY;
    }

    return ELF_LOAD_OK;
}
