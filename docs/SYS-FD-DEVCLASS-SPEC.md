# SYS_FD_DEVCLASS -- a read-only "what Dev backs this fd" syscall

**Status: adopted (2026-06-19) via the aux-merge intermezzo. The syscall is a NEW
kernel ABI -> the impl is an escalation-bearing future Phase-7 task that follows
the audit-trigger discipline (a new syscall surface). The number is reassigned
79 -> 80 (main took 79 for `SYS_CLOCK_SETTIME`, net-7a). This doc is the binding
spec; the coreutils `ls --color=auto` flip [docs/COREUTILS-THYLACINE-DESIGN.md]
is its first consumer (the seam is stubbed to `always` until this lands).**

Implementation-ready spec, aux-authored for the main agent to pick up (kernel is
off-limits to the aux track). Motivated by the coreutils-exotic arc
(COREUTILS-THYLACINE-DESIGN.md); user-chosen 2026-06-16 over a minimal `isatty`.

## Why

There is currently **no way for a userspace tool to tell what kind of object an
fd is** -- specifically, the console from a pipe. Confirmed (kernel source + a
boot, 2026-06-16): a console fd (`SYS_CONSOLE_OPEN`) and a pipe fd are
indistinguishable from userspace --
- both are `KOBJ_SPOOR` handles,
- neither implements `stat_native` (so `SYS_FSTAT` fails on both),
- neither stamps a namespace `Path` (so `SYS_FD2PATH` returns 0 on both).

So `ls --color=auto` cannot suppress color into a pipe, and no `tool | tool`
discipline is possible. The fix is a tiny read-only introspection syscall that
returns the fd's **Dev class character** (the `struct Dev.dc` field).

## ABI

```
SYS_FD_DEVCLASS = 80          // 79 is SYS_CLOCK_SETTIME (net-7a); 78 = SYS_PCI_INFO

// returns the Dev class char of `fd` (a positive byte 0x20..0x7e),
// or a negative errno (-EBADF for an unknown / closed fd).
s64 sys_fd_devclass(fd)
```

- No capability required (read-only introspection; mirrors `SYS_FSTAT` /
  `SYS_FD2PATH`, which take `rights == 0`). Adds **no authority** -- I-22, I-5
  unaffected (the class char is not a handle, confers nothing).
- Pure: one `handle_get` + a field read; no allocation, no sleep, no side effect.

## The Dev class chars (`struct Dev.dc`, as built)

From the kernel today (`grep '\.dc =' kernel/*.c`):

| dc  | Dev            | meaning                          |
|-----|----------------|----------------------------------|
| `c` | devcons        | the kernel console (the TTY)     |
| `d` | devdev         | `/dev` aggregating dir           |
| `C` | (consctl)      | console control                  |
| `9` | dev9p          | Stratum-backed disk FS           |
| `r` | devramfs       | the boot ramfs                   |
| `p` | devproc        | `/proc`                          |
| `s` | devsrv         | `/srv`                           |
| `H` | devhw          | `/hw` (the DTB tree)             |
| `n` | devnotes       | notes / signals                  |
| `m` | (mmio)         | device MMIO                      |
| `0` `z` `f` `k` `-` | misc / null | per their Devs           |

A pipe's `dc` is whatever `devpipe` sets (one of the above, NOT `c`/`d`); the
point is only that it differs from the console.

## Kernel implementation sketch

```c
static s64 sys_fd_devclass_handler(u64 fd_raw, ...) {
    struct Thread *t = current_thread();  if (!t) return -1;
    struct Proc *p = t->proc;             if (!p) return -1;

    // Like fd2path: any KOBJ_SPOOR handle, no access right required. The
    // ref-transfer idiom (#844) -> spoor_clunk on every exit.
    struct Spoor *c = sys_lookup_spoor(p, (hidx_t)fd_raw, 0);
    if (c) {
        int dc = c->dev ? c->dev->dc : '-';   // the Dev backing the Spoor
        spoor_clunk(c);
        return (s64)(u8)dc;
    }
    // Not a Spoor (a future non-Spoor fd kind): map the kobj kind to a
    // synthetic class char, or return -EBADF. For v1.0 every fd is a Spoor,
    // so the lookup-fail path is just -EBADF.
    return -T_E_BADF;   // or the kernel's bad-fd errno
}
```

Reach `c->dev` however a Spoor names its Dev in the current tree (the field the
existing `dev->read`/`dev->stat_native` dispatch uses). If a Spoor does not carry
a direct `Dev*`, return the dc via the same indirection `dev_*` calls use.

**Console normalization (confirm):** a fd from `SYS_CONSOLE_OPEN` is `devcons`
(`dc == 'c'`). A fd from walking `/dev/cons` in the namespace may report the
`devdev` leaf (`dc == 'd'`) or resolve through to `devcons` (`dc == 'c'`) -- per
the #57b single-impl share. Decide one: simplest is that BOTH `c` and `d`-cons
report `'c'` (normalize the `/dev/cons` leaf), so `is_terminal == (dc == 'c')` is
exact. Document whichever you pick.

## libthyla-rs wrapper (main-track, with the syscall)

```rust
// raw
pub unsafe fn t_fd_devclass(fd: i32) -> i64 { svc(T_SYS_FD_DEVCLASS, fd, ...) }

// friendly
impl Stdout { pub fn is_terminal(&self) -> bool {
    unsafe { t_fd_devclass(1) }.ok().map_or(false, |dc| dc == b'c' as i64)
}}
// or fs::fd_devclass(fd) -> Result<u8>
```

## Consumers (aux wires these once the wrapper lands -- all one-liners)

1. **ls `--color=auto`** (the motivating case): `ls::stdout_is_console()` becomes
   `io::stdout().is_terminal()`. Then **flip the ls default from `Always` to
   `Auto`** -- interactive `ls` is colored+boxed, `ls | cat` and `ls > f` are
   byte-clean automatically. (Today `stdout_is_console()` is a `true` stub; this
   is the exact swap point.)
2. **The ls REALM column, sharper** (optional, costs an `open` per entry): label
   `disk` (`9`) / `boot` (`r`) / `dev` (`c`/`d`) / `graft` (`p`/`s`/`H`/...)
   precisely from the entry's `dc`, instead of inferring `graft` from an fstat
   failure. The fstat-failure heuristic stays the fallback.
3. **`realm` / `qid` tools** (the proposed Thylacine-distinctive tools): `realm
   <path>` prints each path's Dev class + name directly.
4. Any tool that wants `isatty(3)` semantics (a future `less`/pager, progress
   bars).

## Test plan (kernel-side)

- `fd_devclass(console_fd) == 'c'`; `fd_devclass(pipe_fd) != 'c'` and is the pipe
  Dev's char; `fd_devclass(a dev9p file fd) == '9'`; `fd_devclass(99) < 0` (bad
  fd). A boot E2E: `ls` colored interactive, `ls | cat` clean (once the default
  flips to Auto).

## Status

Spec only (aux). The kernel syscall + the libthyla-rs wrapper are main-track; the
ls + tool wiring is aux (trivial, gated on the wrapper). Until it lands, ls is
color-on by default with `--color=never` for a clean pipe.
