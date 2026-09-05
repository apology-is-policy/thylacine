---
id: sub-viv
type: sub
title: "viv: the container runner"
parent: moc-userspace
code: ["usr/viv/src/main.rs", "usr/viv/src/json.rs"]
audit: light
guarded-by: [inv-i43, inv-i23]
validated-by: [prose]
locks: []
design: ["docs/VIVARIUM.md"]
created: 2026-08-06
updated: 2026-08-06
---
## Purpose

`viv run <bundle>` is the **runtime** half of the OCI split — the runc
factoring, where image *acquisition* is a separately-owned sibling. It
consumes a pre-assembled bundle (a directory holding `rootfs/` and
`config.json`), builds the container's territory, and spawns the
entrypoint.

The claim that makes it interesting is what it does **not** hold:

> viv holds **no capability beyond the invoker's**.

`chroot`, `mount` and `chdir` are per-territory operations; the container
principal is the invoker's, with no `CAP_SET_IDENTITY` anywhere; no
hardware allowance is conferred; and the only spawn permission it passes
on is `MAY_POST_SERVICE` to its own diorama — which is also the one
permission viv itself must be spawned with. A container is a *namespace*,
not a privilege domain, which is what [[inv-i23]] means in practice.

It is also the **only thing in the system that can declare a Linux
phenotype**. The manifest annotation is the declaration; the ELF byte is a
hint that may never decide.

## Contract

```
viv run <bundle-dir>          # bundle must be an absolute path
```

Exit status is the container's own, or 1 for a viv-side failure, or 2 for
usage. The manifest subset read from `config.json`:

| Key | Effect |
|---|---|
| `root.path` | the rootfs, absolute or bundle-relative |
| `root.readonly` | **parsed for shape, not acted on** — see Caveats |
| `process.args` | argv; `args[0]` is the entrypoint |
| `process.env` | becomes the container's entire environment |
| `process.cwd` | must be absolute |
| `annotations["org.thylacine.net"]` | `"granted"` binds `/net` |
| `annotations["org.thylacine.phenotype"]` | `"linux"` sets `SPAWN_PHENO_LINUX` |
| `annotations["org.thylacine.sigpipe-selftest"]` | `"yes"` — the bundle-scoped signal probe |

Every bound is checked here and fails closed; the kernel's own spawn and
env bounds sit *behind* these, so nothing relies on downstream rejection.

## Mechanism

**The assembly order is forced by capability mechanics**, and is the part
worth reading twice:

1. Parse the manifest; spawn the **per-container diorama**
   (`--vivarium <us>`, posting `/srv/viv-dio`) and mount it over `/dio` in
   *our own* territory.
2. Set our own `/env` to exactly the manifest's set — the child's
   environment is the kernel Env clone taken at spawn, so "inherits
   nothing the manifest does not name" is achieved by making the *parent*
   hold only that.
3. **Pre-open every capability the container world needs as an fd**: the
   rootfs, the `/dio/proc` and `/dio/sys` subtrees, the trivial `/dev`
   leaves, `/env`, `/net` when granted, and the diorama's `/proc/<pid>/ctl`
   kill channel. **fds survive chroot; paths do not.**
4. `chroot` to the rootfs — viv itself enters the container world, because
   nothing it still needs is path-reachable — then mount the held fds over
   the rootfs's anchor paths, `chdir`, and spawn the entrypoint.
5. Wait by pid, then kill the diorama through the held ctl fd and reap it.

Step 3 is the whole design. Once chrooted, viv cannot name anything
outside the container, so every out-of-container resource must already be
in hand as a descriptor. The diorama's kill channel is the sharpest case:
its `/proc` path is unreachable post-chroot **and the container's own
`/proc` would not show it anyway**, because the diorama is deliberately
not a member of its own view. So an unopenable ctl on a *live* diorama is
fatal early, while the path-based kill still works — proceeding without
one risks waiting on an unkillable child.

**The phenotype declaration lands at exactly one call.** The diorama is a
native Thylacine server that happens to serve a Linux-shaped world, so it
spawns native; only the container's own entrypoint carries the manifest's
phenotype, and its descendants inherit it through rfork.

**`spawn_raw` is deliberately not `process::Command`.** Command always
endows the parent's fds 0/1/2, and viv is routinely **fd-less** — joey
spawns its boot daemons with no fds and output rides `SYS_PUTS` — so the
endowment's handle lookup would fail the whole spawn.

**`stdio_born` is captured before any open**, and the reason is a real
bug class: viv's own transient opens recycle low fd numbers, so a late
`fstat(0)` can see the diorama's ctl fd sitting at slot 0 and mis-endow a
half-empty trio, which fails the whole spawn at the kernel's fd bump. It
is a fact about *how viv was spawned*, so it must be read when that is
still the only thing true of fd 0.

**The `/dev/tty` bind decodes the pts qid.** When fd 0 is a ptyfs slave
(discriminated by a flag bit in the qid, mirrored from ptyfs), viv opens
an O_PATH fd of the corresponding `/dev/pts/<n>` as the bind *source* —
the open fd 0 itself cannot be one, because crossing clone-walks the
source and an opened 9P fid cannot be walked. Not a pts (the boot gate's
console-inherited stdio) means the bind is simply omitted.

**The JSON parser accepts complete syntax and reads a subset.** Objects,
arrays, strings with the full escape set including surrogate pairs,
numbers, booleans, null — so any well-formed `config.json` parses and
unknown fields skip cleanly. Numbers are **syntax-validated and
discarded**, because nothing in the read subset is numeric. A parse error
fails the whole manifest: viv refuses to run a container it only partly
understood.

`json::selftest()` runs **before any container assembly** and gates the
run — the diorama's selftest-before-serve pattern, scaled to one parser.
It pins the real manifest shape, escape decoding, the depth cap, and six
malformed documents that must fail rather than half-parse.

## Data structures

`Manifest` — the extracted subset: root path, argv, env pairs, cwd, and
three booleans (`net_granted`, `pheno_linux`, `sigpipe_selftest`).

`Json` — a six-variant enum. `Num` carries no value, deliberately.

Bounds, all fail-closed: `CONFIG_MAX` 64 KiB, `ARGS_MAX` 64, `ENV_MAX` 64,
`PATH_MAX` 512, `ENV_NAME_MAX` 128, `ENV_VALUE_MAX` 3900, `DEPTH_MAX` 16
(against a hostile deeply-nested document exhausting the stack; a real
config's depth is about 3).

Env names are additionally restricted to alphanumerics and underscore.

## Concurrency

Single-threaded, no locks. The only concurrency it must reason about is
**its own child's liveness while waiting for `/srv/viv-dio` to appear**,
and the poll loop handles it by asking `child_exited` each iteration
rather than burning the full timeout: a dead diorama can never post.

`sleep_ms` is implemented as a poll on the read end of a pipe nothing
writes — there is no sleep syscall, so a wait with a timeout on a
never-ready fd is the sleep.

**The fixed `/srv/viv-dio` name means containers cannot run concurrently.**
That is a known limit rather than a fault, and the failure message says so
explicitly, because the symptom (a diorama that exits before posting) is
otherwise indistinguishable from a broken bundle.

## Invariants enforced

- [[inv-i43]] — viv is where the phenotype is *declared*, and the
  declaration is a manifest annotation rather than an inference. Absent,
  or anything but `"linux"`, yields native: a bundle written by a tool
  that knows nothing about phenotypes gets the safe default, and nothing
  is ever inferred into a non-default ABI.
- [[inv-i23]] — the container's authority is exactly the invoker's,
  bounded by the territory viv assembles. Every capability the container
  gets is one viv could already reach and chose to bind.

## Error paths

Every failure after the diorama spawns **kills and reaps it** — a leaked
half-container daemon would hold the fixed `/srv/viv-dio` name against the
next run. The one exception is the already-reaped case, where
`child_exited` did the reap and a second `reap_diorama` would write `kill`
to a reaped pid's `/proc` path.

Two error messages are unusually detailed, and both earn it:

- **the diorama-never-posted message** names the concurrent-`viv run`
  cause, because that is the likely one and it is otherwise a mystery;
- **the failed-entrypoint-spawn message** re-opens `args[0]` with OEXEC
  and reports whether *that* passes. OEXEC runs the same leaf permission
  gate and Dev open the spawn-time resolve runs, so a failed spawn with a
  passing OEXEC open points past resolution — it names the failure class
  for the operator rather than handing them an rc.

A non-zero container exit is reported with `stdio_born`, the phenotype,
the entrypoint and the pid. Until that line existed, "the container
failed" and "viv failed" looked identical from outside, and neither said
whether the container had anywhere to write — a healthy container with
`stdio_born=false` looks exactly like one that never ran.

## Performance

Not a hot path: one process, a bounded manifest parse, ~15 opens, ~10
mounts, two spawns. The diorama poll is up to 50 iterations at 100 ms, cut
short the moment the child is seen dead.

## Prosecution

What a change must re-establish:

- **that every out-of-container resource is opened before the chroot.**
  Adding a bind whose source is opened after is a path that silently
  cannot resolve;
- **that `stdio_born` is still read before any open**;
- **that the phenotype is set on the entrypoint spawn only** — putting it
  on the diorama would make a native server decode Linux numbers;
- **that every post-spawn failure path reaps the diorama exactly once**;
- **that new manifest fields are bounded here**, not left to the kernel.

## Seams

- **`viv pull`** — image acquisition, the separately-owned v1.x sibling.
  The v1.0 bundles are host-baked into the pool by the build script.
- **Concurrent containers**, blocked on the fixed `/srv/viv-dio` name.
- **The sigpipe selftest is a bundle-scoped test facility**, and it exists
  because it is *the only way a v1.0 Linux guest can cause a catchable
  signal at all*: `kill` and `tkill` are not translation rows, and `clone`
  admits only the fork and vfork shapes, so a guest can reach neither
  another Proc nor a peer thread of its own. Handing the entrypoint a
  reader-less pipe at fd 0 makes its own `write()` post `pipe` — a SIGPIPE
  the guest inflicts on itself, synchronously, with no second Proc timing
  anything. It confers no authority: viv is the parent and already holds a
  kill channel, which is strictly stronger.

## Caveats

- **`root.readonly` is parsed and not enforced.** There is no read-only
  bind flag, so the FS permission model — a SYSTEM-owned bake versus a
  non-SYSTEM invoker — is the whole enforcement. A bundle that sets it
  gets no additional protection from viv. Documented at the parse site
  rather than silent, but a manifest author would reasonably expect
  otherwise.
- **The reason the sigpipe selftest exists was stated wrongly once and
  fixed.** It used to be justified as "clone is not a table row", which
  the clone row's landing quietly falsified *without breaking anything* —
  the conclusion survived on a different premise (the row admits neither
  CLONE_THREAD nor a shape that reaches another Proc). The corrected
  reasoning is at the site.
- **A `/dev/tty` bind is omitted, not faked, when viv has no pts.** The
  container then has no `/dev/tty`, which is correct and can surprise a
  program that assumes one exists.
- **An unenumerable `/env` fails the run**, deliberately: carrying the
  invoker's environment into the container would break the manifest's
  "inherits nothing it does not name". The names are collected before any
  unlink, because unlinking while enumerating would race the readdir
  cursor.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
