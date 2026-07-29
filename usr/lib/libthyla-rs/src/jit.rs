// I-42 / CL-7k -- the native JIT surface (docs/JIT-ON-WX-DESIGN.md; the
// LLVM-DESIGN.md section 8 userspace half).
//
// A `CodeRegion` is one dual-mapped area: the SAME physical pages reachable
// through a writable pointer (emit here) and an executable pointer (call here).
// No page is ever writable AND executable, which is what lets a JIT exist at
// all on a system that enforces W^X with no prot-mutation syscall.
//
// The shape is deliberately the one LLVM's ORC MemoryMapper already wants -- a
// "working address" you write and an "execution address" you branch to -- so a
// DualMapMemoryMapper over this is a thin adapter rather than a translation
// layer. That mapper is CL-7's business; this module is the primitive.
//
// Usage:
//     let mut r = CodeRegion::new(4096)?;   // needs CAP_JIT
//     r.write(0, &encoded_instructions);
//     r.publish();                          // the cache dance -- REQUIRED
//     let f: extern "C" fn(u64) -> u64 = unsafe { r.entry(0) };
//
// The `publish()` call is not optional and not an optimization. Between a data
// write and an instruction fetch of the same address the architecture requires
// a D-cache clean plus an I-cache invalidate; without it the CPU may fetch
// whatever the instruction cache held before, which for a fresh region is the
// zero pattern (UDF #0) and for a reused one is the previous function.

use crate::{t_icache_sync, t_jit_create, t_jit_destroy};

// Mirrors <thylacine/errno.h>. The denial is EACCES, not EPERM: errno.h forbids
// a handler returning -T_E_PERM because its value (1) collides with the kernel's
// flat -1 generic-failure sentinel, so a caller could not tell "you lack CAP_JIT"
// from "something broke".
const E_NOMEM: i64 = 12;
const E_ACCES: i64 = 13;
const E_INVAL: i64 = 22;

/// The two aliases of one code region, as `SYS_JIT_CREATE` returns them.
/// Mirrors `struct t_jit_region` in <thylacine/syscall.h>.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RawRegion {
    writer_va: u64,
    exec_va: u64,
}

const _: () = assert!(core::mem::size_of::<RawRegion>() == 16);
const _: () = assert!(core::mem::offset_of!(RawRegion, writer_va) == 0);
const _: () = assert!(core::mem::offset_of!(RawRegion, exec_va) == 8);

/// Why a code region could not be created.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum JitError {
    /// The Proc does not hold `CAP_JIT`. The capability is elevation-only, so
    /// it is never inherited -- it must be acquired through a clearance grant.
    NotPermitted,
    /// Zero length, or larger than the kernel's per-region cap.
    BadLength,
    /// The per-Proc page budget, the VA space, or the allocator said no.
    OutOfMemory,
    /// Anything else the kernel reported.
    Other(i64),
}

impl JitError {
    fn from_rc(rc: i64) -> Self {
        match rc {
            r if r == -E_ACCES => JitError::NotPermitted,
            r if r == -E_INVAL => JitError::BadLength,
            r if r == -E_NOMEM => JitError::OutOfMemory,
            other => JitError::Other(other),
        }
    }
}

/// One dual-mapped executable region.
///
/// Not `Send`/`Sync`: the raw addresses are only meaningful inside the Proc
/// (and the address space) that created them, and handing one to another thread
/// while emitting into it would be a data race on the region's own bytes.
pub struct CodeRegion {
    writer: *mut u8,
    exec: *const u8,
    len: usize,
}

impl CodeRegion {
    /// Create a region of at least `len` bytes. Requires `CAP_JIT`.
    ///
    /// The kernel rounds the length up to whole pages and zero-fills them.
    /// Zero is `UDF #0` on AArch64, so any part of the region that has not
    /// been emitted into traps if executed rather than running stale bytes.
    pub fn new(len: usize) -> Result<Self, JitError> {
        if len == 0 {
            return Err(JitError::BadLength);
        }
        let mut raw = RawRegion::default();
        let rc = unsafe { t_jit_create(len as u64, &mut raw as *mut RawRegion as u64) };
        if rc != 0 {
            return Err(JitError::from_rc(rc));
        }
        // The kernel rounds up; report the rounded size so `write` bounds-check
        // against what actually exists rather than what was asked for.
        let rounded = (len + 0xFFF) & !0xFFF;
        Ok(CodeRegion {
            writer: raw.writer_va as *mut u8,
            exec: raw.exec_va as *const u8,
            len: rounded,
        })
    }

    /// The region's size in bytes (the request rounded up to whole pages).
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Emit `bytes` at `offset`. Returns false if the range does not fit.
    ///
    /// Writes go through the WRITABLE alias; nothing here is executable, so a
    /// mis-emitted byte can only corrupt the region's own future behavior, not
    /// escape it.
    pub fn write(&mut self, offset: usize, bytes: &[u8]) -> bool {
        match offset.checked_add(bytes.len()) {
            Some(end) if end <= self.len => {}
            _ => return false,
        }
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), self.writer.add(offset), bytes.len());
        }
        true
    }

    /// Emit one 32-bit AArch64 instruction at `offset` (little-endian).
    /// A convenience over `write` for the common encode-and-place loop.
    pub fn write_insn(&mut self, offset: usize, insn: u32) -> bool {
        self.write(offset, &insn.to_le_bytes())
    }

    /// Make everything written so far fetchable.
    ///
    /// REQUIRED between emitting and calling. Skipping it is not a performance
    /// choice -- the instruction cache may still hold the region's previous
    /// contents, so the CPU can execute bytes that are no longer there.
    pub fn publish(&self) -> bool {
        unsafe { t_icache_sync(self.writer as u64, self.len as u64) == 0 }
    }

    /// Publish just `[offset, offset+len)`, for a JIT that patches a small
    /// piece of an already-published region and does not want to pay for the
    /// whole thing.
    pub fn publish_range(&self, offset: usize, len: usize) -> bool {
        match offset.checked_add(len) {
            Some(end) if end <= self.len => {}
            _ => return false,
        }
        unsafe { t_icache_sync(self.writer as u64 + offset as u64, len as u64) == 0 }
    }

    /// The writable base. Emit through this.
    pub fn writer_ptr(&self) -> *mut u8 {
        self.writer
    }

    /// The executable base. Branch through this.
    pub fn exec_ptr(&self) -> *const u8 {
        self.exec
    }

    /// Reinterpret `exec_ptr() + offset` as a callable function.
    ///
    /// # Safety
    /// The caller asserts that valid, published machine code implementing `F`'s
    /// signature and calling convention begins at that offset. Getting this
    /// wrong executes arbitrary bytes -- but only bytes inside this region,
    /// with this Proc's own authority. `CAP_JIT` governs who may emit code;
    /// it grants nothing to the code that runs.
    pub unsafe fn entry<F: Copy>(&self, offset: usize) -> F {
        debug_assert!(offset + 4 <= self.len);
        debug_assert_eq!(core::mem::size_of::<F>(), core::mem::size_of::<usize>());
        let addr = self.exec as usize + offset;
        core::mem::transmute_copy(&addr)
    }
}

impl Drop for CodeRegion {
    fn drop(&mut self) {
        // Tears down BOTH aliases and frees the pages. Named by the writer
        // alias, which is how the kernel identifies the region.
        unsafe {
            let _ = t_jit_destroy(self.writer as u64);
        }
    }
}

// A few AArch64 encodings, enough to emit a leaf function. Not an assembler --
// just the pieces a prover (and a first ORC adapter) needs, kept here so the
// encodings live next to the region that holds them.
pub mod a64 {
    /// `ret` -- return to the address in x30.
    pub const RET: u32 = 0xd65f_03c0;

    /// `mov xd, #imm16` (MOVZ, no shift). imm16 is the low 16 bits.
    pub fn movz(rd: u32, imm16: u16) -> u32 {
        0xd280_0000 | ((imm16 as u32) << 5) | (rd & 0x1f)
    }

    /// `add xd, xn, xm`.
    pub fn add(rd: u32, rn: u32, rm: u32) -> u32 {
        0x8b00_0000 | ((rm & 0x1f) << 16) | ((rn & 0x1f) << 5) | (rd & 0x1f)
    }

    /// `bti c` -- a branch-target landing pad for an indirect CALL.
    ///
    /// Emit this as the first instruction of any JITed function that will be
    /// reached through a function pointer. On BTI-enforcing silicon an indirect
    /// branch to an instruction that is not a landing pad raises a Branch
    /// Target exception; on silicon without BTI the encoding is a NOP, so
    /// emitting it unconditionally costs one instruction and is never wrong.
    pub const BTI_C: u32 = 0xd503_245f;
}
