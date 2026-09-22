// Fixed-form MMIO accesses. Keep address calculation outside the instruction:
// pre/post-indexed loads/stores do not provide the ISV syndrome QEMU/HVF needs.
// Memory clobbers are compiler barriers; callers still supply device/DMA fences.
#ifndef THYLACINE_ARM64_MMIO_H
#define THYLACINE_ARM64_MMIO_H
#include <thylacine/types.h>
static inline u8 io_read8(const volatile void *address) {
    u32 v; __asm__ volatile("ldrb %w0, [%1]" : "=r"(v) : "r"(address) : "memory"); return (u8)v;
}
static inline u16 io_read16(const volatile void *address) {
    u32 v; __asm__ volatile("ldrh %w0, [%1]" : "=r"(v) : "r"(address) : "memory"); return (u16)v;
}
static inline u32 io_read32(const volatile void *address) {
    u32 v; __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(address) : "memory"); return v;
}
static inline u64 io_read64(const volatile void *address) {
    u64 v; __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(address) : "memory"); return v;
}
static inline void io_write8(volatile void *address, u8 value) {
    __asm__ volatile("strb %w0, [%1]" :: "r"((u32)value), "r"(address) : "memory");
}
static inline void io_write16(volatile void *address, u16 value) {
    __asm__ volatile("strh %w0, [%1]" :: "r"((u32)value), "r"(address) : "memory");
}
static inline void io_write32(volatile void *address, u32 value) {
    __asm__ volatile("str %w0, [%1]" :: "r"(value), "r"(address) : "memory");
}
static inline void io_write64(volatile void *address, u64 value) {
    __asm__ volatile("str %0, [%1]" :: "r"(value), "r"(address) : "memory");
}
#endif
