# Reference: userspace syscall dispatch (P3-Ec)

## Purpose

Bridges the EL0 → EL1 sync exception (EC_SVC_AARCH64) to a syscall handler that reads numbered syscalls + args per the AArch64 ABI and dispatches to kernel-internal handlers. v1.0 P3-Ec ships the absolute minimum surface — `SYS_EXITS` + `SYS_PUTS` — sufficient for an EL0 thread to demonstrate it ran.

ARCH §13: "Userspace ↔ kernel boundary is the SVC instruction. AArch64 ABI: x8 syscall number, x0..x5 args, x0 return. SVC #imm immediate currently unused at v1.0; reserved for class selectors at Phase 5+."

## Public API

### `<thylacine/syscall.h>`

```c
enum {
    SYS_EXITS = 0,
    SYS_PUTS  = 1,
};

struct exception_context;

void syscall_dispatch(struct exception_context *ctx);
```

#### `syscall_dispatch(ctx)`

Reads `ctx->regs[8]` for the syscall number; reads `ctx->regs[0..5]` for arguments per the AArch64 ABI. Writes the return value to `ctx->regs[0]`. Returns normally; `vectors.S` ERETs back to EL0 via `.Lexception_return`, restoring `ctx` (with the modified `regs[0]`) to the user thread's register state.

Special case: `SYS_EXITS` does not return — it calls kernel `exits()`, which marks the Proc ZOMBIE + the calling Thread EXITING + wakes parent's child_done + calls `sched()` to context-switch away. The user thread's register state is abandoned (the exception_context on its kernel stack stays alive until `wait_pid` reaps via `thread_free`).

Unknown syscall number: returns `-1` (ENOSYS-equivalent) without extincting. Phase 5+ delivers a SIGSYS-like note.

### Syscall details

#### `SYS_EXITS(status)` — terminate calling process

| AArch64 reg | Meaning |
|---|---|
| `x8`  | `SYS_EXITS = 0` |
| `x0`  | exit status (integer) |

Maps to kernel `exits()`:
- `status == 0` → `exits("ok")`  → `p->exit_status = 0`
- `status != 0` → `exits("fail")` → `p->exit_status = 1`

Phase 5+ extends to a u64 exit_status carrying the full integer payload.

Never returns.

#### `SYS_PUTS(buf, len)` — write to UART

| AArch64 reg | Meaning |
|---|---|
| `x8`  | `SYS_PUTS = 1` |
| `x0`  | pointer to bytes (user VA) |
| `x1`  | byte count |

Returns `len` on success; `-1` on validation failure (NULL buf, `len > 4096`).

The kernel reads `len` bytes one at a time via `uart_putc`. Reads pass through demand paging — user pages get installed if not yet present. v1.0 doesn't validate that `buf` is in a VMA; if it isn't, `userland_demand_page` returns `FAULT_UNHANDLED_USER` and `exception_sync_lower_el` extincts. Phase 5+ adds copy_from_user-style bounds + fault-recovery that translates the fault into a `-EFAULT` return.

## Implementation

### `kernel/syscall.c`

```c
__attribute__((noreturn))
static void sys_exits_handler(u64 status);

static s64 sys_puts_handler(u64 buf_va, u64 len);

void syscall_dispatch(struct exception_context *ctx) {
    u64 nr = ctx->regs[8];
    switch (nr) {
    case SYS_EXITS: sys_exits_handler(ctx->regs[0]);                   /* noreturn */
    case SYS_PUTS:  ctx->regs[0] = (u64)sys_puts_handler(ctx->regs[0], ctx->regs[1]);
                    return;
    default:        ctx->regs[0] = (u64)(s64)-1;
                    return;
    }
}
```

### `arch/arm64/exception.c::exception_sync_lower_el`

The EC_SVC_AARCH64 case routes to `syscall_dispatch`:

```c
case EC_SVC_AARCH64:
    syscall_dispatch(ctx);
    return;
```

(Was `extinction_with_addr("EL0 SVC ... not implemented at v1.0")` pre-P3-Ec.)

The `return` is reached only for non-EXITS syscalls; SYS_EXITS never returns from `syscall_dispatch`.

## Spec cross-reference

No new TLA+ at P3-Ec. The syscall dispatch is a bounded switch over a small enum; each case calls into existing primitives (`exits()` from P2-D, `uart_putc` from P1-G). No new concurrency, refcount, or invariant introduced.

Phase 5+ syscall extension adds:
- File handle ops (`open` / `close` / `read` / `write`) — interacts with handles.tla.
- Thread/Proc ops (`rfork`, `wait`, `notify`) — interacts with scheduler.tla.
- Memory ops (`mmap`, `munmap`, `mprotect`) — interacts with burrow.tla.
- Each large-surface syscall family is its own spec extension.

## Tests

`kernel/test/test_syscall.c` — five tests:

- `syscall.dispatch_unknown`: nr=9999 → `ctx->regs[0] = -1`.
- `syscall.dispatch_puts_smoke`: SYS_PUTS with valid/NULL/oversized/zero-length args; verifies return values + UART write.
- `syscall.dispatch_exits_ok`: child rfork'd; child calls `syscall_dispatch(ctx with SYS_EXITS, status=0)`; parent `wait_pid` observes `exit_status=0`.
- `syscall.dispatch_exits_fail`: same but status=42 → `exit_status=1` (binary mapping).
- `syscall.dispatch_args_in_x0_to_x5`: poisons all unused regs with unique values; verifies SYS_PUTS reads only x0+x1.

The SYS_PUTS test channel produces visible UART output during the test run — `[syscall.puts test channel]` appears in the boot log. v1.0 doesn't capture/parse that output; Phase 5+ end-to-end tests will.

## Status

- **Implemented at P3-Ec**: `syscall_dispatch`, SYS_EXITS, SYS_PUTS, exception.c routing, 5 unit tests.
- **Stubbed**: full Thylacine syscall surface (Phase 5+).
- **Stubbed**: copy_from_user / copy_to_user with fault-recovery (Phase 5+).

Commit landing point: `48dfc5c`.

## Known caveats / footguns

1. **No userspace pointer validation at v1.0**. SYS_PUTS dereferences `buf_va` directly. If the VA isn't in a VMA, `userland_demand_page` returns `FAULT_UNHANDLED_USER` from the SECOND fault path (the kernel's read of buf), which extincts the kernel. Phase 5+ adds proper validation.

2. **SYS_EXITS doesn't preserve numeric payload**. Status is collapsed to `0` (ok) / `1` (fail). Phase 5+ extends.

3. **Single-call-per-SVC**. v1.0 ignores `imm16` in `svc #imm`. Phase 5+ may use `imm` as a fastpath selector or a class.

4. **Syscall numbers are unstable until Phase 5+**. v1.0 P3-Ec syscalls (SYS_EXITS, SYS_PUTS) are placeholders; the full Thylacine syscall ABI is defined at the syscall surface freeze (Phase 5).

5. **Syscalls run in the calling thread's kernel context** (the EL0 thread's kstack with the saved `exception_context`). Syscalls that block (Phase 5+ `read`, `wait`) call `sleep` from this context; the wakeup resumes the syscall handler and the eventual ERET goes back to user.

## Naming rationale

`syscall_dispatch` (not `do_syscall` or `sys_handler`) — matches the dispatch / handler naming used elsewhere (`arch_fault_handle`, `userland_demand_page`).

`SYS_*` enum prefix — Linux convention. Phase 5+ stable numbers may use `THYLACINE_SYS_*` to make the namespacing explicit.
