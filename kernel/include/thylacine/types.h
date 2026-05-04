// Thylacine common types.
//
// Freestanding C99 baseline types. Clang's freestanding mode provides
// stdint.h (uintN_t / intN_t) and stddef.h (size_t / NULL); we re-export
// them here through a single project header so call sites have one
// include for the full kernel-wide vocabulary.

#ifndef THYLACINE_TYPES_H
#define THYLACINE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Common Plan 9 / 9Front kernel idiom: u32, u64, etc. Some prefer the
// explicit uint32_t form; both work, but pre-existing Plan 9 code in
// porting candidates uses u32 / u64. Keep both available.
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uintptr_t vaddr_t;   // Virtual address.
typedef uintptr_t paddr_t;   // Physical address. Same width on ARM64 v1.0.

#endif // THYLACINE_TYPES_H
