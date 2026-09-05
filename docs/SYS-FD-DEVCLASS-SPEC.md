# SYS_FD_DEVCLASS -- a read-only "what Dev backs this fd" syscall

**Status: adopted (2026-06-19) via the aux-merge intermezzo. The syscall is a NEW
kernel ABI -> the impl is an escalation-bearing future Phase-7 task that follows
the audit-trigger discipline (a new syscall surface). The number is reassigned
79 -> 80 (main took 79 for `SYS_CLOCK_SETTIME`, net-7a). This doc is the binding
spec; the coreutils `ls --color=auto` flip [docs/COREUTILS-THYLACINE-DESIGN.md]
is its first consumer (the seam is stubbed to `always` until this lands).**

Implementation-ready spec, aux-authored for the main agent to pick up. (The
parenthetical here read "kernel is off-limits to the aux track" -- false since
at least 2026-07, corrected 2026-08-16, aux#237: the aux track works `kernel/`
routinely. Whoever picks this up, either track may.) Motivated by the
coreutils-exotic arc
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
| `t` | dev9p (a pts SLAVE) | a registered pts slave -- a terminal its host renders (H-4d-2a); the MASTER stays `9` |
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

**pts normalization (H-4d-2a, 2026-09-05; AS-BUILT):** a dev9p Spoor the
kernel's pts registry knows as a SLAVE (`pts_resolve_spoor` -- the tty
seam's own resolve: a ref-held (conn, qid) binding, pointer-compared, never a
server-settable qid bit) answers `'t'`; the MASTER side and every other dev9p
file stay `'9'`. `is_terminal == (dc == 'c' || dc == 't')`;
`spoor_is_console` (the I-27 gate) is untouched -- `'t'` confers nothing.

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
   byte-clean automatically. **LANDED at H-1c-2 (2026-09-01)**: every
   `stdout_is_console()` stub in the tree (18 bins) now calls
   `libthyla_rs::stdout_is_terminal()`; ls's default flipped to `Auto` (the
   new `ps` is born `Auto`); the in-guest proof is coreutil-smoke's
   `ls auto pipe clean` legs (piped `ls` asserts zero ESC bytes).
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
- H-4d-2a: `fd_devclass(a pts SLAVE fd) == 't'` -- in-guest only (no kernel-test
  fixture builds a dev9p Spoor over a live SrvConn): a session tile's `ut` says
  `beacon rich (transcript zones armed)` over a real ptyfs slave
  (ls-gfx-session), which only the `'t'` answer produces; `devdev.fd_devclass`
  pins `spoor_devclass` on devcons / devdev / devsrv Spoors so `'t'` reaches no
  other Dev.

## Status

Spec only (aux). The kernel syscall + the libthyla-rs wrapper are main-track; the
ls + tool wiring is aux (trivial, gated on the wrapper). Until it lands, ls is
color-on by default with `--color=never` for a clean pipe.

## AS-BUILT (H-1, 2026-09-01 -- the Beacon arc pulled this forward; BEACON.md 12.4)

Landed as specified, with the following deltas and decisions, each verified
against the tree at implementation time:

- **Number 80 held.** The table gap between `SYS_CLOCK_SETTIME` (79) and
  `SYS_WEFT_SHARE` (81) was still reserved; `SYS_FD_DEVCLASS = 80`
  (`kernel/include/thylacine/syscall.h`).
- **The handler is simpler than the sketch**: `struct Spoor` carries a cached
  `int dc` ("matches dev->dc; cached for cheap dispatch", set at
  `spoor_alloc`), so no `c->dev` indirection is needed. The C-2 lesson holds
  by construction: the dc is the KERNEL's field, never a server-supplied
  stat/qid bit -- a 9P server cannot forge "I am the console."
- **The June dc table above is STALE in two rows -- the as-built bestiary**
  (from `kernel/include/thylacine/dev.h`): `c` devcons, `0` devnull, `z`
  devzero, `f` devfull, **`r` devrandom** (NOT ramfs), `n` devnotes, `p`
  devproc, **`C` devctl** (NOT consctl -- consctl is a devdev LEAF), **`m`
  devramfs**, `d` devdev, `H` devhw, `P` devpci, `E` devenv, `9` dev9p,
  **`|` devpipe** (`DEVPIPE_DC`, `kernel/include/thylacine/pipe.h`) -- so the
  test plan's `pipe != 'c'` holds structurally, `-` devnone.
- **The console normalization, DECIDED + BUILT**: every `/dev` leaf is a
  devdev Spoor (dc `'d'`, leaf kind in `qid.path`), so the handler routes
  `'d'` through `devdev_fd_devclass()` (`kernel/devdev.c`), which answers
  `'c'` for the cons DATA leaf ONLY. consctl/consdrain/consfeed/winsize and
  every other leaf stay `'d'` (control-plane files are not the terminal).
  `is_terminal == (dc == 'c')` is exact across both mint paths
  (`SYS_CONSOLE_OPEN` and a walked `/dev/cons`).
- **O_PATH visibility (deliberate)**: a `T_OPATH` open never calls `Dev.open`
  (both the single-hop and stalk paths guard on `SYS_WALK_OPEN_OPATH`), so an
  unattached Proc can query `/dev/cons`'s class through an OPATH fd. This is
  introspection only -- the fd is `CWALKONLY` (#81: byte I/O rejected) and the
  I-27 gate still guards every open-for-I/O -- so nothing is conferred; noted
  so the audit does not rediscover it.
- **Errno**: `-T_E_BADF` (9, POSIX-pinned) rather than the bare `-1` of the
  fd2path era.
- **Wrappers**: `t_fd_devclass` in BOTH `usr/lib/libt/include/thyla/syscall.h`
  (C; joey) and `usr/lib/libthyla-rs/src/lib.rs` (+ safe `fd_devclass(fd) ->
  Option<u8>` and `stdout_is_terminal()`).
- **Tests**: `devdev.fd_devclass` (kernel: the normalization + the 'd'
  control one variable away) + the joey `probe H1` E2E (cons 'c' / null 'd' /
  pipe '|' / ramfs 'm' / closed-fd negative) through the REAL syscall.
- **The sibling landed with it**: the consctl `beacon <tier>` verb
  (ARCH 23.5.4) -- the other half of the Beacon emission gate this syscall
  serves.

## AS-BUILT addendum (H-4d-2a, 2026-09-05 -- the pts slave class)

- **`'t'` for a registered pts slave.** `spoor_devclass` (kernel/syscall.c,
  the syscall's body, non-static for the tests) applies two normalizations
  over the Dev's own char: the devdev `/dev/cons` leaf -> `'c'` (H-1), and a
  dev9p Spoor `pts_resolve_spoor` reports as a SLAVE -> `'t'`. The master
  stays `'9'`. The resolve is the tty seam's (`dev9p_client_fid` -> the
  SrvConn transport downcast -> a pointer-compare against ref-held bindings
  under `g_pts_lock`, a leaf lock), so a freed / stale pts fails closed and a
  Spoor on another server's connection cannot match.
- **Why a class, not the env alone.** The Beacon gate's Auto arm exists so
  frames never land in a pipe or a file (BEACON.md 12.4); a pts is a terminal
  something renders, so it belongs on the terminal side of that line -- but
  WHICH tier is the host's to say, hence the env half (`kaua-term --beacon`).
  Both halves are required: a tile shell whose env says rich but whose stdout
  is redirected emits nothing, and a pts whose host declared nothing is plain.
- **Consumers moved with it**: `beacon::effective_tier` (+ `DC_PTS`),
  `libthyla_rs::stdout_is_terminal()` (`--color=auto` in every coreutil now
  colors inside a tile), `ut`'s pts branch (`env_beacon_tier`), the `halcyon`
  tool's `stdout_is_rich`.
