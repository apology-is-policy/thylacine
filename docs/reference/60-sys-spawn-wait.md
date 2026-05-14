# 60. SYS_SPAWN + SYS_WAIT_PID (P5-spawn-wait)

The smallest userspace orchestration primitive: `SYS_SPAWN` combines `rfork(RFPROC)` + `exec` on a binary loaded from the boot initrd, and `SYS_WAIT_PID` reaps a ZOMBIE child. With these two syscalls and the existing `SYS_EXITS`, `/joey` extends from "prints and exits" to a real orchestrator — it can spawn children, wait for them, and decide what to do based on their exit status.

This is the prerequisite for the rest of the P5-stratumd-bringup arc: `/joey` will eventually spawn `stratumd-stub` (and later real `stratumd-system`) the same way it spawns `/hello` today.

---

## Why combined fork+exec at v1.0

ARCH §11.2 lists `rfork(flags)` + `exec(name, argv)` as separate syscalls. A real `SYS_RFORK` returning 0 in child / pid in parent requires:

1. COW duplication of the parent's address space.
2. Saved CPU context restoration so the child returns from the syscall in its own address space.
3. Child-side eret with x0 = 0.

None of those exist yet at v1.0 — the kernel-internal `rfork(flags, entry, arg)` runs the child via a C entry function on a kthread kstack and never returns to the parent's userspace context.

The Plan 9 `rfork(RFPROC|RFEXEC)` idiom avoids all of that by combining fork + exec: the child never sees the parent's address space; instead, it exec's a fresh binary. `SYS_SPAWN` is exactly that idiom at v1.0: child gets a fresh address space populated from the named binary in devramfs. A separate `SYS_RFORK` with proper COW lands in a later chunk when needed.

---

## ABI

### SYS_SPAWN (= 21)

```
SYS_SPAWN(name_va, name_len) → child_pid (>0) / -1

Args:
  x0 = name_va        user-VA pointer to the binary name
  x1 = name_len       bytes; in 1..SYS_SPAWN_NAME_MAX = 64

Return:
  x0 = child_pid      >0 on success
  x0 = -1             on:
       - name_len out of range
       - name_va outside user-VA bound
       - embedded NUL inside name_len
       - binary not in devramfs
       - blob > SYS_SPAWN_BLOB_MAX (32 KiB)
       - kmalloc / rfork OOM
```

The child inherits **no capabilities** (`CAP_ALL & 0u = 0`). v1.0 model: "the child fully describes its needs," so capabilities are added per-child explicitly. A future SYS_RFORK with a `cap_mask` argument lands when a use case calls for inheritance.

### SYS_WAIT_PID (= 22)

```
SYS_WAIT_PID(status_out_va) → reaped_pid / -1

Args:
  x0 = status_out_va  user-VA pointer to int (4 bytes); 0 to skip write

Return:
  x0 = reaped_pid     >0 on success
  x0 = -1             immediately if caller has no children;
                      -1 if status_out_va is non-NULL but invalid;
                      -1 if uaccess_store_u8 fails mid-write
```

Blocks until one of the caller's children enters `ZOMBIE`, then reaps. Mirrors `kernel/proc.c::wait_pid` (Plan 9 wait(2) shape; no PID selector at v1.0).

---

## Implementation

### Lifetime of the ELF blob copy

`SYS_SPAWN`'s lifetime invariant: the ELF bytes must live from `rfork` return through `exec_setup` completion in the child's thunk, but the caller may return to userspace immediately after `rfork`. So the blob can't live on the caller's kernel stack.

Solution:

1. `kmalloc(cpio_size, 0)` for the 8-aligned blob copy.
2. `kmalloc(sizeof(struct spawn_args), KP_ZERO)` for the thunk args.
3. Pass the args struct as rfork's `arg`.
4. Child's `sys_spawn_thunk` reads args, `kfree`s args, calls `exec_setup`, `kfree`s blob, then `userland_enter`.
5. On rfork failure: parent `kfree`s both.

### Why the 8-aligned copy is mandatory

Identical to `kernel/joey.c` and `kernel/test/test_userspace_ramfs.c`: cpio newc format pads to 4 bytes; `elf_load` requires 8 (`R5-G F61` alignment precondition on the Ehdr cast). `kmalloc` returns naturally aligned memory for power-of-two requests, and routes `>SLUB_MAX_OBJECT_SIZE = 2048` requests through `alloc_pages` (page-aligned, satisfies 8). A defensive `((uintptr_t)blob_copy & 0x7) != 0` check is in the handler for paranoia; if it ever fires, kmalloc itself is broken and the chunk has bigger issues.

### Per-byte uaccess for the status write

`SYS_WAIT_PID` writes `sizeof(int) = 4` bytes via `uaccess_store_u8` × 4. This is consistent with the existing pattern (`SYS_READ` etc.) — no `uaccess_store_u32` primitive yet. If the user-VA pointer faults mid-write, the function returns `-1` even though the child is already reaped. The child can only be reaped once; the partial-write fault should be vanishingly rare (caller would have to munmap the status buffer between SYS_WAIT_PID entry and the store).

### Factoring for test access

The handler's complex part (devramfs lookup + alignment copy + rfork) is extracted into a non-static inner:

```c
int sys_spawn_for_proc(struct Proc *p, const char *name, size_t name_len);
```

The SVC wrapper does user-VA validation, copies the name into a kernel-stack buffer (≤64 bytes), and calls the inner. Tests call the inner directly with a kernel-resident name.

`sys_wait_pid_for_proc` is NOT extracted: the SVC wrapper's only non-trivial step is the per-byte uaccess store, and the kernel-internal `wait_pid()` is already directly callable from tests.

---

## Userspace API — `<thyla/syscall.h>`

```c
long t_spawn(const char *name, size_t name_len);
long t_wait_pid(int *status_out);

#define T_SPAWN_NAME_MAX  64u
```

Inline-asm stubs matching the kernel ABI shape.

---

## Joey's orchestration demonstration

`usr/joey/joey.c` is now:

```c
int main(void) {
    t_putstr("joey: hello from /joey ...\n");

    long pid = t_spawn("hello", 5);
    if (pid <= 0) { /* fail-path */ return 1; }

    /* print "joey: spawned /hello pid=<N>" */

    int status = -1;
    long reaped = t_wait_pid(&status);
    if (reaped != pid || status != 0) { /* fail-path */ return 1; }

    t_putstr("joey: /hello reaped status=0; orchestration verified\n");
    return 0;
}
```

Boot log (representative):

```
  joey: rforking child for /joey (16624 byte ELF from initrd)
joey: hello from /joey (real userspace binary, loaded from ramfs)
joey: spawned /hello pid=1357
hello from /hello (built via tools/build.sh userspace)
joey: /hello reaped status=0; orchestration verified
  joey: /joey pid=1356 exited cleanly (status=0)
Thylacine boot OK
```

The four-line interleaving of `joey:` and `hello from /hello:` lines is the live demonstration of `SYS_SPAWN` + `SYS_WAIT_PID` end-to-end at EL0 in the production boot path.

---

## Tests

`kernel/test/test_sys_spawn.c` — 7 tests:

- `sys_spawn.happy_path` — spawn `/hello`, `wait_pid` reaps it, exit status = 0.
- `sys_spawn.rejects_null_name` — NULL → -1.
- `sys_spawn.rejects_zero_len` — `name_len = 0` → -1.
- `sys_spawn.rejects_oversize_name` — `name_len > SYS_SPAWN_NAME_MAX` → -1.
- `sys_spawn.rejects_missing_binary` — `devramfs_lookup` miss → -1.
- `sys_spawn.rejects_embedded_nul` — embedded NUL inside `name_len` bytes → -1.
- `sys_wait_pid.no_children_returns_neg1` — `wait_pid` with no children → -1 immediately.

Each test calls `drain_zombies()` first to reap any leftover children from prior tests, keeping `sys_wait_pid` coverage deterministic.

The SVC wrapper's user-VA validation paths (out-of-range name_va, in-band uaccess fault) are not separately tested at kernel level — they're shared with `SYS_PUTS`/`SYS_READ`/`SYS_WRITE` and have been audited at those surfaces (R12-uaccess). Userspace happy-path coverage is the live `/joey` orchestration.

Test count: 416 → 423 PASS × default + UBSan.

---

## Failure paths

| Failure | Symptom | Resolution |
|---|---|---|
| Binary not in initrd | `t_spawn` returns -1 | Verify `tools/build.sh::build_ramfs` includes the binary in `usr_bins`. |
| Binary > 32 KiB | `t_spawn` returns -1 | Bump `SYS_SPAWN_BLOB_MAX`. Most v1.0 binaries are well under. |
| exec_setup fails in child | child exits "fail-exec" → status = 1; `t_wait_pid` returns pid with status=1 | Diagnose via `kernel/elf.c::elf_load_status`. |
| status_out_va bound violation | `t_wait_pid` returns -1 (child still reaped) | Validate userspace pointer before the call. |

---

## Composition with future chunks

- **P5-stratumd-stub-bringup**: `/joey` extends to spawn a `stratumd-stub` userspace binary that serves a 9P tree; joey then drives `SYS_ATTACH_9P` + `SYS_MOUNT` against the stub's pipes.
- **P5-corvus-bringup**: `/joey` (post-pivot) spawns `/sbin/corvus` and `/sbin/login` per `CORVUS-DESIGN.md §3 D4`.
- **P5-spawn-caps** (future): a separate `SYS_SPAWN_WITH_CAPS(name_va, name_len, cap_mask)` exposes `rfork_with_caps` so joey can spawn corvus with `CAP_LOCK_PAGES | CAP_CSPRNG_READ` etc. Held until a use case calls for it.
- **P5-rfork-cow** (future): a real `SYS_RFORK(flags) → 0/pid` with COW address-space cloning. Substantial chunk; held until a v1.x workload (shell pipelines + arbitrary userspace) requires it.

---

## Status

| Item | State |
|---|---|
| SYS_SPAWN handler + libt stub | LANDED |
| SYS_WAIT_PID handler + libt stub | LANDED |
| Joey orchestrates `/hello` end-to-end in boot path | LANDED |
| `sys_spawn_for_proc` non-static inner for tests | LANDED |
| 7 kernel-internal tests | LANDED |
| Real `SYS_RFORK` with COW | DEFERRED (no v1.0 use case yet) |
| `SYS_SPAWN_WITH_CAPS` (cap-mask arg) | DEFERRED (corvus startup uses spawn-with-zero-caps + corvus-side cap-setup at v1.0) |

---

## Known caveats

1. **Child inherits no capabilities at v1.0.** `rfork(RFPROC, ...)` is called with `caps_mask = CAP_NONE`. corvus / per-user stratumd at v1.0 will need either explicit cap-setup post-spawn or a future `SYS_SPAWN_WITH_CAPS`.
2. **No argv/envp at v1.0.** The spawned binary's `main()` takes no arguments. argv arrives when the kernel's `exec` syscall supports the auxv layout (Phase 5+).
3. **No fork-without-exec.** A real `SYS_RFORK` returning 0 in child / pid in parent is deferred. v1.0 uses combined spawn.
4. **status_out_va partial-fault hazard.** If the user-VA buffer faults mid-store, the child is reaped but the status write is partial. `SYS_WAIT_PID` returns -1 in that case so userspace observes the fault explicitly; the partial-state hazard is impossible to fully avoid without an `uaccess_store_u32` primitive (a P5 follow-on if it ever becomes load-bearing).
5. **SYS_SPAWN_BLOB_MAX = 32 KiB.** Comfortable for the v1.0 binaries (joey: 16624, hello: 12792, stratumd-stub future ~25 KiB est). Bump on demand.
