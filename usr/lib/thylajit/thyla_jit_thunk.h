/* usr/lib/thylajit/thyla_jit_thunk.h -- a bounded write thunk for one
 * SYS_JIT_CREATE code region, sealed by SYS_JIT_CREATE_SEALED.
 *
 * A JIT that must not hold its region's writer address in readable memory
 * writes code through this thunk instead: write(offset, src, n) copies n bytes
 * to writer + offset, and executes `brk #0xc471` (the Proc dies of snare:brk)
 * when offset > size or n > size - offset, offset read unsigned. The writer
 * address exists only as immediates inside the thunk, whose pages are execute-only.
 * n == 8 at an 8-aligned destination is one 64-bit store (an atomic repatch);
 * otherwise 32-bit stores, then a byte tail. x3, the register that held the
 * writer address, is zeroed before ret; the rest are AAPCS64 temporaries.
 *
 * The words are hand-encoded A64, never assembled at run time, so no assembler
 * buffer ever holds a readable copy of the writer address. Shared by JSC's
 * allocator (usr/ports/webkit/patches/0002) and pouch-hello-jitthunk, the
 * witness that runs this exact encoding against the kernel. C99 and C++. */
#ifndef THYLA_JIT_THUNK_H
#define THYLA_JIT_THUNK_H

#include <stddef.h>
#include <stdint.h>
#include "thyla_jit.h"

#define THYLA_JIT_THUNK_WORDS 33
#define THYLA_JIT_THUNK_BRK   0xc471u

typedef void (*thyla_jit_write_fn)(int64_t offset, const void *src, size_t n);

static inline uint32_t thyla_a64_movz(unsigned rd, uint16_t imm, unsigned hw) { return 0xD2800000u | (hw << 21) | ((uint32_t)imm << 5) | rd; }
static inline uint32_t thyla_a64_movk(unsigned rd, uint16_t imm, unsigned hw) { return 0xF2800000u | (hw << 21) | ((uint32_t)imm << 5) | rd; }
static inline uint32_t thyla_a64_cmp(unsigned rn, unsigned rm) { return 0xEB000000u | (rm << 16) | (rn << 5) | 31u; }
static inline uint32_t thyla_a64_cmpi(unsigned rn, unsigned imm12) { return 0xF1000000u | (imm12 << 10) | (rn << 5) | 31u; }
static inline uint32_t thyla_a64_sub(unsigned rd, unsigned rn, unsigned rm) { return 0xCB000000u | (rm << 16) | (rn << 5) | rd; }
static inline uint32_t thyla_a64_add(unsigned rd, unsigned rn, unsigned rm) { return 0x8B000000u | (rm << 16) | (rn << 5) | rd; }
static inline uint32_t thyla_a64_subi(unsigned rd, unsigned rn, unsigned imm12) { return 0xD1000000u | (imm12 << 10) | (rn << 5) | rd; }
static inline uint32_t thyla_a64_tst7(unsigned rn) { return 0xF2400000u | (2u << 10) | (rn << 5) | 31u; }
static inline uint32_t thyla_a64_ldrx(unsigned rt, unsigned rn) { return 0xF9400000u | (rn << 5) | rt; }
static inline uint32_t thyla_a64_strx(unsigned rt, unsigned rn) { return 0xF9000000u | (rn << 5) | rt; }
static inline uint32_t thyla_a64_ldrw_post(unsigned rt, unsigned rn, unsigned imm9) { return 0xB8400400u | ((imm9 & 0x1FFu) << 12) | (rn << 5) | rt; }
static inline uint32_t thyla_a64_strw_post(unsigned rt, unsigned rn, unsigned imm9) { return 0xB8000400u | ((imm9 & 0x1FFu) << 12) | (rn << 5) | rt; }
static inline uint32_t thyla_a64_ldrb_post(unsigned rt, unsigned rn, unsigned imm9) { return 0x38400400u | ((imm9 & 0x1FFu) << 12) | (rn << 5) | rt; }
static inline uint32_t thyla_a64_strb_post(unsigned rt, unsigned rn, unsigned imm9) { return 0x38000400u | ((imm9 & 0x1FFu) << 12) | (rn << 5) | rt; }
static inline uint32_t thyla_a64_movzero(unsigned rd) { return 0xAA1F03E0u | rd; }
/* A branch distance counts instructions: the target's index minus the branch's. */
static inline uint32_t thyla_a64_b(int dist) { return 0x14000000u | ((uint32_t)dist & 0x3FFFFFFu); }
static inline uint32_t thyla_a64_bcond(unsigned cond, int dist) { return 0x54000000u | (((uint32_t)dist & 0x7FFFFu) << 5) | cond; }
static inline uint32_t thyla_a64_cbz(unsigned rt, int dist) { return 0xB4000000u | (((uint32_t)dist & 0x7FFFFu) << 5) | rt; }

/* Fills w with the thunk for a region of `size` bytes whose writer alias is at
 * `writer`. Returns 0, or -22 (EINVAL) when size is 0 or does not fit 32 bits. */
static inline int thyla_jit_thunk_emit(uint32_t w[THYLA_JIT_THUNK_WORDS], uint64_t writer, uint64_t size)
{
    enum { X0 = 0, X1 = 1, X2 = 2, X3 = 3, X4 = 4, X9 = 9, X10 = 10 };
    enum { NE = 1, LO = 3, HI = 8 };
    enum { WORDS = 19, BYTES = 25, DONE = 30, TRAP = 32 };
    if (size == 0 || size >= ((uint64_t)1 << 32))
        return -22;
    w[0]  = thyla_a64_movz(X9, (uint16_t)size, 0);
    w[1]  = thyla_a64_movk(X9, (uint16_t)(size >> 16), 1);
    w[2]  = thyla_a64_cmp(X0, X9);
    w[3]  = thyla_a64_bcond(HI, TRAP - 3);
    w[4]  = thyla_a64_sub(X10, X9, X0);
    w[5]  = thyla_a64_cmp(X2, X10);
    w[6]  = thyla_a64_bcond(HI, TRAP - 6);
    w[7]  = thyla_a64_movz(X3, (uint16_t)writer, 0);
    w[8]  = thyla_a64_movk(X3, (uint16_t)(writer >> 16), 1);
    w[9]  = thyla_a64_movk(X3, (uint16_t)(writer >> 32), 2);
    w[10] = thyla_a64_movk(X3, (uint16_t)(writer >> 48), 3);
    w[11] = thyla_a64_add(X3, X3, X0);
    w[12] = thyla_a64_cmpi(X2, 8);
    w[13] = thyla_a64_bcond(NE, WORDS - 13);
    w[14] = thyla_a64_tst7(X3);
    w[15] = thyla_a64_bcond(NE, WORDS - 15);
    w[16] = thyla_a64_ldrx(X4, X1);
    w[17] = thyla_a64_strx(X4, X3);
    w[18] = thyla_a64_b(DONE - 18);
    w[19] = thyla_a64_cmpi(X2, 4);              /* WORDS */
    w[20] = thyla_a64_bcond(LO, BYTES - 20);
    w[21] = thyla_a64_ldrw_post(X4, X1, 4);
    w[22] = thyla_a64_strw_post(X4, X3, 4);
    w[23] = thyla_a64_subi(X2, X2, 4);
    w[24] = thyla_a64_b(WORDS - 24);
    w[25] = thyla_a64_cbz(X2, DONE - 25);       /* BYTES */
    w[26] = thyla_a64_ldrb_post(X4, X1, 1);
    w[27] = thyla_a64_strb_post(X4, X3, 1);
    w[28] = thyla_a64_subi(X2, X2, 1);
    w[29] = thyla_a64_b(BYTES - 29);
    w[30] = thyla_a64_movzero(X3);              /* DONE */
    w[31] = 0xD65F03C0u;                        /* ret */
    w[32] = 0xD4200000u | (THYLA_JIT_THUNK_BRK << 5);   /* TRAP: brk #0xc471 */
    return 0;
}

/* Zeroes n bytes through volatile stores: a memset of a dying local is a dead
 * store the compiler may drop. */
static inline void thyla_jit_scrub(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--)
        *b++ = 0;
}

/* Emits the thunk, seals it, scrubs the words. Returns 0 and the thunk's VA in
 * *out_va, or -errno (see thyla_jit_create_sealed). The caller scrubs its own
 * copies of `writer`. */
static inline long thyla_jit_thunk_create(uint64_t writer, uint64_t size, uint64_t *out_va)
{
    uint32_t w[THYLA_JIT_THUNK_WORDS];
    long rc = thyla_jit_thunk_emit(w, writer, size);
    if (rc == 0)
        rc = thyla_jit_create_sealed(w, sizeof w, out_va);
    thyla_jit_scrub(w, sizeof w);
    return rc;
}

#endif
