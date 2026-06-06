/*
 * Thylacine kernel linker script — ARM64 / QEMU virt.
 *
 * Layout decisions (ARCHITECTURE.md §5, §6):
 *
 * - Load address (LMA): 0x40080000. QEMU virt RAM starts at 0x40000000;
 *   the first 512 KiB is conventionally reserved for early firmware
 *   (UEFI / U-Boot / shadow ROM). Linux ARM64 uses the same offset.
 *
 * - Link virtual address (VMA): 0xFFFFA00000080000. The kernel is
 *   linked into the TTBR1 high half per ARCH §6.2 ("Kernel modules +
 *   KASLR" region 0xFFFF_A000_*). PIE relocations (R_AARCH64_RELATIVE
 *   entries in .rela.dyn) let the boot stub slide the runtime VA by a
 *   randomized 2 MiB-aligned offset, applied via the slide computed
 *   in arch/arm64/kaslr.c. The boot stub runs at LMA initially (TTBR0
 *   identity-maps the low 4 GiB), applies relocations, sets up TTBR1
 *   to map the randomized high VA range to LMA, then long-branches
 *   into high VA and continues from there.
 *
 * - LMA / VMA split per section. AT(KERNEL_LOAD_PA) on .text fixes
 *   the LMA of the first byte; subsequent sections inherit the
 *   LMA-VMA delta. The flat binary (objcopy -O binary) is positioned
 *   in LMA order, so its file-offset 0 corresponds to PA 0x40080000
 *   — which is what QEMU's load_aarch64_image() expects given the
 *   Linux ARM64 image header at offset 0.
 *
 * - Section order: .text, .rodata, .data, .rela.dyn, .bss. Each
 *   starts on a 4 KiB page boundary so MMU page-protection (P1-C)
 *   gives .text RX, .rodata R, .data RW, .bss RW with no inter-
 *   section spillover. W^X invariant (I-12) starts here.
 *
 * - BSS-allocated boot stack: 16 KiB (THREAD_STACK_SIZE per ARCH §7.3
 *   future commitment). Aligned to a page; a 4 KiB guard page sits
 *   BELOW the stack at _boot_stack_guard. mmu.c zeroes its L3 PTE so
 *   stack overflow faults synchronously instead of silently corrupting
 *   BSS.
 *
 * Symbols exported for C code:
 *   _kernel_start         — first byte of the kernel image (link VA;
 *                          PC-rel runtime gives the load PA)
 *   _kernel_end           — first byte past the kernel image
 *   _text_end / _rodata_end / _data_end — section boundary symbols
 *                          consumed by mmu.c for per-section permissions
 *   _bss_start, _bss_end  — BSS bounds (start.S clears this range)
 *   _rela_start, _rela_end — .rela.dyn bounds (kaslr.c walks this)
 *   _boot_stack_guard     — start of the 4 KiB stack guard page
 *   _boot_stack_bottom    — first usable stack byte
 *   _boot_stack_top       — top of the boot stack (start.S sets SP)
 *   _image_size           — _kernel_end - _kernel_start (Linux header)
 *
 * Note: KERNEL_LINK_VA itself is hardcoded as a #define in
 * arch/arm64/mmu.h (KASLR_LINK_VA) for kaslr.c's slide math. PIE mode
 * means absolute symbols can't be read via PC-relative addressing, so
 * we keep the constant on both sides and assert their equality at
 * runtime by linker ASSERT.
 */

ENTRY(_start)

KERNEL_LOAD_PA   = 0x40080000;
KERNEL_LINK_VA   = 0xFFFFA00000080000;
PAGE_SIZE        = 0x1000;       /* 4 KiB */
BOOT_STACK_SIZE  = 0x4000;       /* 16 KiB; matches ARCH §7.3 thread stack */

SECTIONS
{
    . = KERNEL_LINK_VA;
    _kernel_start = .;

    /* .text — kernel code. MMU mapping: RX. AT() fixes the LMA. */
    .text : AT(KERNEL_LOAD_PA) ALIGN(PAGE_SIZE) {
        KEEP(*(.text._start))      /* _start MUST be the first instruction */
        *(.text .text.*)
    }
    . = ALIGN(PAGE_SIZE);
    _text_end = .;

    /* .rodata — read-only data. MMU mapping: R. */
    .rodata : ALIGN(PAGE_SIZE) {
        *(.rodata .rodata.*)

        /* R12-uaccess: kernel-mode user-VA fault-fixup table. Each
         * entry is two s32 PC-relative offsets (op_rel, fixup_rel);
         * arch/arm64/uaccess.c walks the
         * [_uaccess_fixup_start, _uaccess_fixup_end) range on every
         * kernel-mode sync fault whose FAR is in the user half.
         * Lives in .rodata for R-only protection — the table is
         * read at exception-time but never mutated. PC-relative
         * encoding (cf. Linux __ex_table) keeps the table PIE-clean
         * — no R_AARCH64_ABS64 relocs that the PIE linker rejects,
         * and no R_AARCH64_RELATIVE entries needing a runtime
         * fixup pass. KEEP() is defensive: even with --gc-sections
         * off today, the section carries no symbol references from
         * C and would otherwise be eligible for stripping by future
         * toolchain configurations. */
        . = ALIGN(4);
        _uaccess_fixup_start = .;
        KEEP(*(.uaccess_fixup))
        _uaccess_fixup_end = .;

        /* W1.5 boot-time alternatives (LSE-patching). Both sections are
         * READ-ONLY metadata the patcher consumes: .altinstructions is the
         * (PC-relative-offset) patch table, .altinstr_replacement holds the
         * single-instruction LSE replacements (copied OUT, never executed in
         * place). RO+NX here keeps them W^X-clean; PC-relative .word offsets
         * keep them reloc-free / KASLR-independent (cf. .uaccess_fixup, the
         * Halls symtab). apply_alternatives walks [start,end). */
        . = ALIGN(8);
        _alt_instructions_start = .;
        KEEP(*(.altinstructions))
        _alt_instructions_end = .;
        . = ALIGN(4);
        KEEP(*(.altinstr_replacement))
    }
    . = ALIGN(PAGE_SIZE);
    _rodata_end = .;

    /* .data — initialized writable data. MMU mapping: RW. */
    .data : ALIGN(PAGE_SIZE) {
        *(.data .data.*)
    }
    . = ALIGN(PAGE_SIZE);
    _data_end = .;

    /* .rela.dyn — PIE relocations. The boot stub (kaslr.c) walks this
     * region and applies each R_AARCH64_RELATIVE entry by adding the
     * KASLR slide to the addend. The section must be in the loaded
     * image, not in BSS, because the entries describe what to patch.
     *
     * On our minimal kernel the section is typically empty (all
     * references are PC-relative) but the infrastructure is in place
     * for future code that introduces absolute pointer references. */
    .rela.dyn : ALIGN(8) {
        _rela_start = .;
        *(.rela.dyn)
        *(.rela.text .rela.text.*)
        *(.rela.rodata .rela.rodata.*)
        *(.rela.data .rela.data.*)
        _rela_end = .;
    }

    /* PIE infrastructure that lld emits but we don't actively use. We
     * place these adjacent to .rela.dyn (BEFORE .bss) so that the flat
     * binary doesn't carry 56 KiB of zero-padding over the BSS gap;
     * /DISCARD/'ing them crashes lld's PIE pipeline even with
     * -z norelro -z nopack-relative-relocs, because lld still wants
     * the dynamic-section family resident. They're tiny (a few hundred
     * bytes total) and live in the loaded image; mmu.c's L3 mapping
     * for the kernel's 2 MiB block covers them transparently. */
    .dynamic   : ALIGN(8) { *(.dynamic) }
    .dynsym    : ALIGN(8) { *(.dynsym) }
    .dynstr    : ALIGN(8) { *(.dynstr) }
    .gnu.hash  : ALIGN(8) { *(.gnu.hash) }
    .hash      : ALIGN(8) { *(.hash) }
    .got       : ALIGN(8) { *(.got) *(.got.plt) }
    . = ALIGN(PAGE_SIZE);

    /* .bss — zero-initialized data. start.S clears [_bss_start, _bss_end). */
    .bss : ALIGN(PAGE_SIZE) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)

        /* Stack guard page. mmu.c zeroes the L3 PTE for [_boot_stack_guard,
         * _boot_stack_bottom) so a stack overflow faults synchronously
         * (translation fault, FAR_EL1 inside the guard region) instead of
         * silently corrupting prior BSS. The page is BSS-allocated and
         * zero-cleared at boot; once mmu_enable() runs, it's unreachable
         * via TTBR0/TTBR1. P1-F's fault handler will recognise FAR_EL1
         * inside this region as a stack-overflow extinction. */
        . = ALIGN(PAGE_SIZE);
        _boot_stack_guard = .;
        . += PAGE_SIZE;

        /* Boot stack lives in BSS so it's zeroed at startup. SP grows down,
         * so the "top" pointer in start.S is _boot_stack_top, which is the
         * highest address — i.e. the END of the buffer. */
        _boot_stack_bottom = .;
        . += BOOT_STACK_SIZE;
        _boot_stack_top = .;

        _bss_end = .;
    }

    . = ALIGN(PAGE_SIZE);
    _kernel_end = .;

    /* Discard sections we don't want in the final image. */
    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.ARM.attributes)
        *(.interp)
        *(.plt)
        *(.plt.got)
    }
}

/*
 * Linker-computed size symbol. The Linux ARM64 image header in start.S
 * needs `image_size = _kernel_end - _kernel_start` at offset 0x10 of the
 * binary, but the assembler cannot evaluate subtraction of external
 * symbols. Defining the difference here as a linker-resolved absolute
 * symbol lets start.S do `.quad _image_size` directly.
 *
 * Per Linux ARM64 booting protocol §1: image_size includes BSS so that
 * a bootloader allocating image_size bytes from the load address has
 * space for the kernel's full memory footprint.
 */
_image_size = _kernel_end - _kernel_start;


/* Sanity assertions: the linker errors out if these are violated. */
ASSERT(_kernel_start == KERNEL_LINK_VA,
       "Kernel must link at KERNEL_LINK_VA (0xFFFFA00000080000)")
ASSERT((_boot_stack_top - _boot_stack_bottom) == BOOT_STACK_SIZE,
       "Boot stack size mismatch")
ASSERT((_boot_stack_bottom - _boot_stack_guard) == PAGE_SIZE,
       "Boot stack guard page must be exactly 4 KiB")

/* The kernel image is mapped by two contiguous page-grain L3 tables
 * covering exactly the 4 MiB region starting at the 2 MiB-aligned PA
 * below the load PA — i.e. [KERNEL_LOAD_PA & ~(2 MiB - 1), that + 4 MiB)
 * — per arch/arm64/mmu.c::build_page_tables (P5-kernel-l3-4mib; was a
 * single 2 MiB L3 table). The firmware reservation [kernel_2mib_pa,
 * KERNEL_LOAD_PA) sits in front of the image inside that same 4 MiB
 * region (typically 512 KiB on QEMU virt, since KERNEL_LOAD_PA =
 * 0x40080000 and the region starts at 0x40000000). The L3 tables have
 * no entries past + 4 MiB; any access to a higher PA from the kernel
 * high-VA or direct-map alias takes an unhandled translation fault. The
 * image-only assert is therefore conservative-low: the real constraint
 * is image_size + firmware offset within the 4 MiB region. */
ASSERT((_kernel_end - _kernel_start)
       + (KERNEL_LOAD_PA & (0x400000 - 1)) < 0x400000,
       "Kernel image + firmware reservation overflow the 4 MiB L3 mapping at kernel_2mib_pa (= KERNEL_LOAD_PA & ~(2 MiB - 1)). Either shrink the image, move KERNEL_LOAD_PA to a 4 MiB boundary, or widen the L3 mapping in arch/arm64/mmu.c::build_page_tables.")

/* The 4 MiB L3 region must lie within a single 1 GiB L2 table.
 * build_page_tables wires its two 2 MiB halves as L2 entries `idx` and
 * `idx + 1` of one per-GiB table (l2_ttbr0[gib] for TTBR0; the single
 * l2_directmap_kernel for the direct-map alias). If kernel_2mib_pa
 * (= KERNEL_LOAD_PA & ~(2 MiB - 1)) were the last 2 MiB block of a 1 GiB
 * block, `idx` would be 511 and `idx + 1` would index 512 — one entry
 * past the 512-entry table (an out-of-bounds write) — and the second
 * 2 MiB would land in the next GiB. The TTBR1 high-half path is guarded
 * at runtime instead (its index derives from the KASLR slide, a runtime
 * value); this ASSERT pins the fixed-PA TTBR0 + direct-map paths at link
 * time. The current KERNEL_LOAD_PA = 0x40080000 puts the region at
 * [0x40000000, 0x40400000) — comfortably inside GiB 1. */
ASSERT(((KERNEL_LOAD_PA & ~(0x200000 - 1)) & (0x40000000 - 1)) + 0x400000 <= 0x40000000,
       "The kernel image's 4 MiB L3 region straddles a 1 GiB boundary: kernel_2mib_pa (= KERNEL_LOAD_PA & ~(2 MiB - 1)) is in the last 2 MiB of a 1 GiB block, so build_page_tables' `idx + 1` L2 wiring overruns l2_ttbr0 / l2_directmap_kernel. Move KERNEL_LOAD_PA so kernel_2mib_pa + 4 MiB stays within one 1 GiB block.")
