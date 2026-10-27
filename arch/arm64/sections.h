// Symbols arch/arm64/kernel.ld and start.S define for C. No compiler sees
// their definitions, so each is declared once, here, and every reader shares
// one type (.claude/rules/kernel-code.md, "Cross-file declarations").

#ifndef THYLACINE_ARCH_ARM64_SECTIONS_H
#define THYLACINE_ARCH_ARM64_SECTIONS_H

#include <thylacine/types.h>

// kernel.ld: the image bounds. kaslr also takes _kernel_start as the
// PC-relative anchor for the load PA (adrp+add with the MMU off).
extern char _kernel_start[];
extern char _kernel_end[];

// kernel.ld: section boundaries, each page-aligned, in the order .text,
// .rodata, .data, .bss.
extern char _text_end[];      // end of .text (== start of .rodata)
extern char _rodata_end[];    // end of .rodata (== start of .data)
extern char _data_end[];      // end of .data (== start of .bss)
extern char _bss_start[];

// kernel.ld: bounds of .rela.dyn (kaslr's self-relocation).
extern char _rela_start[];
extern char _rela_end[];

// kernel.ld: the boot CPU's static 16 KiB stack and the 4 KiB guard slot
// immediately below it. mmu.c unmaps the guard so an overflow faults
// synchronously; the in-kernel test suite runs on this stack.
extern char _boot_stack_guard[];
extern char _boot_stack_bottom[];
extern char _boot_stack_top[];

// start.S: words stored before boot_main runs.
extern volatile u64 _saved_dtb_ptr;      // the DTB PA from x0 (Linux arm64 boot protocol)
extern volatile u64 _entered_at_el2;     // 1 if entered at EL2 and dropped to EL1
extern volatile u64 _boot_start_cntpct;  // CNTPCT_EL0 at the top of _real_start

#endif // THYLACINE_ARCH_ARM64_SECTIONS_H
