---
id: sub-kernel-dev
type: sub
title: "The Dev vtable and the bestiary — the interface every namespace resolves through"
parent: moc-kernel-namespace
code: ["kernel/dev.c", "kernel/include/thylacine/dev.h", "kernel/devnone.c", "kernel/null.c", "kernel/zero.c", "kernel/full.c"]
audit: light
guarded-by: []
validated-by: [gate-smp]
locks: []
hazards: []
abis: []
design: ["docs/ARCHITECTURE.md section 9.2"]
created: 2026-08-03
updated: 2026-08-03
---
## Purpose

`struct Dev` is the Plan 9 device vtable, kept verbatim in shape: the
25 function pointers through which every namespace operation reaches
its backing, whether that backing is a RAM table, a 9P session, a
process's own state, or nothing at all. `bestiary[]` is Plan 9's
`devtab` — the sentinel-terminated registry of every Dev the kernel
knows, built once at boot.

Together they are the seam that makes "the filesystem is the OS"
mechanical rather than aspirational: [[sub-kernel-stalk]] resolves a
path without knowing what backs any component, because every backing
answers the same questions. This dossier covers the interface, the
registry, the shared leaf helpers, and the trivial Devs that exist to
be the interface's simplest possible instances. Each substantial Dev
has its own dossier.

## Contract

```c
int          dev_register(struct Dev *d);       // appends; EXTINCTS on any rejection
void         dev_init(void);                    // spoor_init + register + walk ->init()
struct Dev  *dev_lookup_by_dc(int dc);          // NULL if absent
struct Dev  *dev_lookup_by_name(const char *);  // NULL if absent
int          dev_count(void);                   // registered, excluding the sentinel

struct Spoor *dev_simple_attach(struct Dev *d, u8 qtype);   // alloc + qid{0,0,qtype}
struct Spoor *dev_simple_open(struct Spoor *c, int omode);  // set COPEN + mode
void          dev_simple_close(struct Spoor *c);            // clear COPEN

extern struct Dev *bestiary[];                     // BESTIARY_MAX + 1, NULL-terminated
extern struct Walkqid dev_walk_attrs_unsupported;  // address-compared sentinel
```

`dev_register` does not return errors — every rejection is an
extinction: a NULL Dev, a `dc` collision, a `name` collision, a full
bestiary, or a `.wstat_native` slot on a Dev that does not set
`.perm_enforced`. That last one is a structural gate rather than a
hygiene check, and it is the most interesting line in the file
(Mechanism).

**Sixteen of the 25 slots are mandatory; nine are NULL-permitted.** The
optional set is `stat_native`, `wstat_native`, `walk_attrs`,
`open_cached`, `fsync`, `readdir`, `rename`, `unlink`, `poll`. A NULL
slot has a defined meaning per slot, and the meanings are not uniform:
a NULL `poll` means ALWAYS READY (the POSIX-correct answer for a
regular file), while a NULL `fsync`, `readdir` or `stat_native` means
the corresponding syscall returns -1. So one absent slot is a graceful
default and another is a hard refusal, and only the header says which.

## Mechanism

**Registration** appends to a fixed array and maintains the NULL
sentinel after every append. Both lookups are linear scans; at the
current 18 registered Devs that is well inside a cache line's worth of
pointer chasing, and the header records the intent to revisit past the
"cache-line frontier".

**The `.wstat_native` gate.** `dev_register` refuses to boot a Dev that
exposes `.wstat_native` without `.perm_enforced`. The reason is a
closed audit finding (#47): `SYS_WSTAT`'s fd gate is kind-only, so
`perm_wstat_check` — which runs ONLY when `perm_enforced` is set — is
the sole write-authority check on the metadata-mutation path. A Dev
that offered metadata mutation without enforcement would let any handle
holder rewrite mode/uid/gid with no identity check at all. Rather than
document that as a rule for Dev authors, the registration path makes it
un-shippable: the combination fails the boot. **This is the pattern
worth copying** — a vtable-shape constraint whose violation is caught
at registration rather than at the call site that would misuse it.

**Initialization order** is explicit and load-bearing: `spoor_init`
first (the SLUB cache every Dev's attach needs), then `devnone`, then
the trivial leaves, then the directory Devs, then `devsrv` / `devcap` /
`devdev` / `devhw` / `devpci` / `devenv`. Only then does `dev_init`
walk the table calling each `->init()`. The walk re-reads `g_dev_count`
each iteration so a Dev whose `init` registers further Devs (a probe
fanning out to instances) gets those initialized too, while a
watermark ensures each is initialized exactly once. Two more Devs
register outside `dev_init` — `devpipe` from `pipe.c` and `dev9p` from
`dev9p.c` — so the bestiary's final contents are not readable from
`dev_init` alone.

**The simple-leaf helpers** exist because most kernel Devs are one
file: `dev_simple_attach` allocates and stamps a qid of `{path 0,
vers 0, type qtype}`; `dev_simple_open` sets `COPEN` and records the
mode; `dev_simple_close` clears `COPEN`. `dev_simple_open` is
deliberately idempotent — re-opening updates the mode, matching the
Plan 9 idiom.

**`devnone`** is the no-op Dev (`dc='-'`), registered first, and it is
better than it looks. Its documented role is an audit guard — "any
Spoor in production code with `dev == &devnone` is a bug" — and nothing
checks for that. It does not need to: every one of its mandatory ops
returns NULL or -1, so a Spoor that reached `devnone` by mistake fails
every subsequent operation rather than doing something plausible with
the wrong backing. The guard is enforced by being uniformly useless,
which is the correct shape for a sentinel.

**The trivial Devs** (`null.c`, `zero.c`, `full.c`, `random.c`) are the
minimal instances: a single file, no walk, `dev_simple_*` lifecycle,
and read/write semantics that are the entire point. None sets
`.stat_native`, `.seekable` or `.perm_enforced`.

**`dev_walk_attrs_unsupported`** is a distinguished return address for
`walk_attrs`, meaning "this session's backing does not implement the
fused op" — separable from NULL, which means a real first-component
failure. It is address-compared only, never read or freed, and callers
must not `walkqid_free` it. dev9p latches the answer per session, so
the probe costs one RPC ever.

## Data structures

`struct Dev` = 4 data fields + 25 function pointers.

The data fields are two identifiers (`dc`, `name`) and two behavioral
flags, and the flags have an instructive history. `perm_enforced`
selects whether the rwx layer runs for Spoors backed by this Dev
(devramfs and dev9p true; the introspection and control Devs false —
visibility without an identity check, gated at the read site instead).
`seekable` gates `SYS_LSEEK`. `seekable` was originally INFERRED from
`.stat_native != NULL`, and the inference broke when two Devs added
`stat_native` purely so `fstat` would work (#957 for devsrv, A-4b for
devproc), silently regressing `lseek` on a stream to succeed against an
unused offset. The explicit flag decouples "can be fstat'd" from "has a
meaningful position" — a worked example of one predicate answering two
questions and needing to be split.

`bestiary[]` is `BESTIARY_MAX + 1` (33) pointers with a NULL sentinel
at `dev_count`. `g_dev_count` and `g_dev_init_done` are plain statics.

## Concurrency

None, by construction. The bestiary is mutated only during `dev_init`
and the two out-of-band registrations, all before secondaries come up
and before any EL0 code runs; afterward it is read-only and the lookups
need no lock. `dev_init` extincts on re-entry.

This is worth stating explicitly rather than leaving as an absence: the
registry is the one piece of Dev infrastructure that is genuinely
single-threaded, and a future dynamic-Dev path (hot-plug, a driver
registering at runtime) would need a lock that does not exist today and
would invalidate every lockless `dev_lookup_by_dc` call site.

## Invariants enforced

No section-28 invariant is enforced here directly. The Dev layer is the
interface across which other layers' invariants are stated:
[[inv-i28]]'s per-component X-search runs in the resolver against
`stat_native` results this vtable supplies, and [[inv-i5]]'s
non-transferability is a property of handle kinds rather than of Devs.

The one enforcement that lives here is the `.wstat_native` /
`.perm_enforced` coupling described in Mechanism — an
[[inv-i22]]-adjacent structural gate that keeps the identity axis from
being bypassable by a Dev that simply never opted in.

## Error paths

Two disciplines, split by who is at fault:

- **Registration is all-extinction.** Every rejection is a programming
  error discoverable at boot, and a silently-skipped Dev would produce
  a namespace missing a component with no diagnostic.
- **Operations return NULL / -1.** `dev_lookup_*` return NULL for an
  absent Dev; the trivial Devs' unsupported ops return -1 or NULL;
  `devnone` returns failure from everything.

`dev_simple_attach` propagates a `spoor_alloc` NULL. The helpers are
NULL-safe in their Spoor argument.

## Performance

Both lookups are O(registered). Registration is O(registered) for the
collision scan, done 18 times at boot. `dev_init`'s banner walks the
table once to print the names.

## Prosecution

- **The mandatory-slot set must stay complete on every registered
  Dev.** `dev.vtable_slot_coverage` walks the whole bestiary asserting
  all 16 non-optional pointers are non-NULL, plus a non-empty name as a
  zero-init guard. This is the structural check that makes the
  "NULL-permitted" comments trustworthy: exactly the nine documented
  slots may be absent.
- **A Dev's `walk` must honor the reuse-`nc` contract** if its Spoors
  are ever user-walkable. `sys_walk_open_handler` rejects a Dev whose
  walk returns a Spoor other than the `nc` it was handed, because the
  handler opens `nc` and installs `nc` in the handle table — a
  self-cloning walk would open the unwalked Spoor and leak the walked
  one. Every mounted Dev learned this the same way (#57a): a walk that
  ignores `nc` is unreachable through the resolver.
- **A Dev holding refcounted state in `aux` must not return a partial
  Walkqid without normalizing it.** `sys_walk_open_handler`'s
  partial-walk exit clunks the clone with `aux` still shallow-shared
  from the source, which runs the close hook against the SOURCE's
  state. Every Dev that can reach that exit today is safe — six use
  `aux` nowhere at all, devctl's close only clears `COPEN`, devsrv
  refuses a non-registry source AND normalizes `nc->aux = NULL` on
  entry with the rationale written down, and dev9p returns NULL rather
  than a partial. But the constraint is unwritten and untested, and
  devsrv is the existence proof that a Dev CAN hold a refcounted
  connection in `aux` (task #75).
- **`dc` and `name` uniqueness is the registry's whole identity
  story.** A Spoor caches `dc` for dispatch, and the mount table keys
  on `(dc, devno, qid.path)`, so a duplicate `dc` would alias two Devs'
  namespaces in the mount table. The boot-time extinction is the only
  thing preventing it.

Covered by `dev.boot_registration_smoke`, `dev.vtable_slot_coverage`,
`dev.lookup_unknown`, `dev.devnone_ops_smoke`.

## Seams

- **The bestiary has no dynamic-registration path.** `dev_init`'s loop
  supports a Dev registering others from its `init`, but nothing
  supports registration after boot, and the lockless lookups assume it
  never happens.
- **`.reset` and `.power` are unused.** Both are mandatory slots that
  every Dev implements as a no-op, carried from the Plan 9 ARM-laptop
  heritage for runtime power management that does not exist.

## Caveats

- **The `.stat_native` documentation names two Devs that implement
  it.** `dev.h` says "trivial leaf Devs (devcons / devnull / devzero /
  devnotes) leave the slot NULL". devcons implements it (`cons.c`,
  added by #55 as the is-a-cons qid contract) and devnotes implements
  it (added by #97). devnull and devzero are still accurate. What makes
  this worth more than a stale-comment note is WHY both changed: a Dev
  with no `stat_native` fails `SYS_FSTAT`, and clang treats a
  non-EBADF fstat failure on fds 0/1/2 as fatal — which silently killed
  every concurrent `make -j4` job until #96/#97 landed. The header
  still teaches the shape that produced the bug, by worked example, to
  the one audience positioned to repeat it: the next author of a leaf
  Dev. Task #19 (`/dev/winsize` has no `stat_native`) is the same
  family and still open (task #76).
- **The coverage test's Dev-count assertion is a very loose lower
  bound.** `dev.vtable_slot_coverage` ends with `devs_checked >= 8`,
  and its comment enumerates the eight Devs of P4-A through P4-E. Ten
  more have landed since. The per-Dev slot assertions above it are
  strong; the count assertion beneath them is nearly vacuous — it would
  pass with ten Devs missing from the table.
- **`BESTIARY_MAX` headroom is stated against a stale estimate.** The
  cap is 32, described as "generous for v1.0's expected 7-12 devs". The
  real count is 18. Still comfortable, but the margin is half what the
  comment implies, and a Dev whose `init` fans out to per-instance
  registrations would consume it in a way nobody has budgeted.
- **`dev_init`'s comment describes a console routing that does not
  exist.** It registers cons first "so the boot banner could route
  through it; not yet, but the slot is reserved". The banner still goes
  direct to the UART.

## Provenance

(generated -- incoming `touched` backlinks, newest first; never hand-written)
