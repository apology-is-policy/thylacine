/*
 * usr/scripts/aarch64-userspace.ld
 *
 * Linker script for Thylacine native userspace ELFs.
 *
 * Layout: two PT_LOAD segments — text+rodata RX, data+bss RW. Page-aligned
 * boundary between them so the kernel ELF loader (kernel/elf.c) can map
 * each at its own permission tier. W^X is enforced both by the loader
 * (RWX rejection) and by this script (no segment carries both W and X).
 *
 * Base address 0x400000 (4 MiB) matches the Linux static-executable
 * convention and sits comfortably below EXEC_USER_STACK_BASE in the
 * kernel exec.c layout. Userspace ASLR (Phase 5+) will randomize the
 * base via PT_DYN + R_AARCH64_RELATIVE relocations against this image;
 * at v1.0 P4-Ia1 the address is fixed.
 *
 * Discarded sections: cleanup of unused metadata that bloats the binary
 * without contributing at runtime (eh_frame, dynamic linking tables,
 * notes, build-id).
 */

OUTPUT_FORMAT(elf64-littleaarch64)
OUTPUT_ARCH(aarch64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(0x5);   /* R + X = 5 */
    data PT_LOAD FLAGS(0x6);   /* R + W = 6 */
}

SECTIONS {
    . = 0x400000;

    .text : ALIGN(4096) {
        *(.text._start)
        *(.text .text.*)
    } :text

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } :text

    . = ALIGN(4096);

    .data : ALIGN(16) {
        *(.data .data.*)
        *(.data.__stack_chk_guard)
    } :data

    .bss : ALIGN(16) {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.ARM.attributes)
        *(.ARM.exidx)
        *(.ARM.exidx.*)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.interp)
        *(.dynamic)
        *(.rela.*)
        *(.rel.*)
        *(.gnu.version)
        *(.gnu.version_d)
        *(.gnu.version_r)
        *(.plt)
        *(.got)
        *(.got.plt)
    }
}
