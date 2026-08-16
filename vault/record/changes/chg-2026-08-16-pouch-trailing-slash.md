---
id: chg-2026-08-16-pouch-trailing-slash
type: chg
title: "pouch: the third layer could not delete its normalisation, so the rule is 'never decide'"
date: 2026-08-16
arc: arc-vault
commits: ["1586527b"]
touched: [sub-pouch-fs]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
This completes the triptych. The same defect family was repaired at three
layers in one window — the kernel's cwd join, this boundary-line splitter, and
the native runtime's `Path` — and swept here across three sessions in the
reverse order it was built. Having all three in view changes the principle I
recorded for the first two.

## The blanket reject was wrong in both directions

`__pouch_open_parent` refused **any** trailing slash with `EINVAL`. POSIX 4.13
makes a trailing `/` assert *the leaf names a directory*, so `unlink("f/")`
wants `ENOTDIR` — and `rmdir("dir/")` must **succeed**. That second row is the
one that actually bites: `rm -r dir/`, and every script that builds `"$DIR/"`.

The fix is **strip, report, and let the kernel decide**. The splitter removes
the separator run and reports the assertion through a `dir_required`
out-parameter; the leaf becomes a length-bounded slice, so every caller passes
that length rather than a `strlen` that would run back into the stripped
separators. A shared probe then issues **one `SYS_STAT` of the still-
slash-terminated path**, which runs the kernel's own audited gate — zero only
for an existing directory, otherwise the real `ENOTDIR`/`ENOENT`/`EACCES`.

**The POSIX rule is enforced where it is audited, never re-derived here.**

## The correction to my own principle

For the kernel and the native runtime I recorded the lesson as: *when N layers
each normalise, they each normalise wrong — so delete the normalisation.* Both
of those fixes did exactly that.

**This layer could not.** Splitting `(parent, leaf)` is structurally required,
because the kernel's mutation primitives take a parent fd and a leaf name
rather than a path. The separator genuinely has to come off.

So the rule is not *never transform*. It is **never DECIDE**:

> A layer that must transform a path may strip, but it may not adjudicate. It
> reports what the original asserted and re-asks the authority.

Under that reading all three are one fix. The other two had nothing to
transform, so "do not decide" collapsed to "delete the cleaning" and I mistook
the collapsed form for the general one. Here it means strip, carry the
assertion in a side channel, and spend an extra `SYS_STAT` so the audited gate
answers.

Worth keeping as a method note about writing these lessons: **a principle
derived from two instances that happen to share an accident reads as more
general than it is.** Two layers with nothing to transform made "delete it"
look like the rule. The third was the disconfirming case, and it arrived only
because the sweep kept going.

## The row that proves the strip alone is wrong

`unlink("x/")` **must never reach the unlink syscall**. Stripped and passed
through, a plain unlink deletes the very file the slash asserts must be a
directory.

That is byte-for-byte the defect [[sub-libthyla-rs]] had — `remove_file("f/")`
deleting `f` — reached from the opposite direction. One layer got there by
cleaning too eagerly, the other by rejecting too bluntly and then being
tempted into the same shortcut when the rejection was lifted. The failure mode
is a property of the *operation*, not of either layer's style.

## Method: measured against a POSIX host, and Linux wins the ties

Every row was measured on a real POSIX host before being written, and where
macOS and Linux diverge the **Linux** answer was chosen deliberately — musl's
target ABI is Linux. Two rows differ: `open("x/", O_CREAT)` on an absent path
is `EISDIR` on Linux and `ENOENT` on macOS; `rename(file, "absent/")` is
`ENOTDIR` on Linux and `ENOENT` on macOS.

A conformance fix that reads the standard and reasons would have had a
defensible answer for both and no way to know which one a ported program
expects.

## The task's framing was wider than the code, again

The task said "every leaf-name syscall". The splitter serves exactly **four**
callers — the `O_CREAT` arm, `mkdirat`, `renameat` (both names), `unlinkat`
(which is also `rmdir` via `AT_REMOVEDIR`). The rest pass full paths and were
already kernel-gated.

Third instance in this family, after two tasks that named `..` where the
measurement showed both dot tokens, and two that proposed a vtable out-param
where the field already existed. **A task filed at discovery describes the
surface as it looks from outside the code.** That is not a criticism of the
tasks — it is an argument for re-scoping at implementation time as a routine
step rather than a discovery.

## Two traps, one of them specific to this seam

**A patch's CONTEXT lines are a dependency on another patch's output.** 0030's
`openat` hunks carried the old `strlen(leaf)` call as *context*, so editing
0024 alone would have broken 0030's apply — silently, at bake time, in the
direction this project has already been bitten by (`patch` exits 0 and drops
lines). The series was regenerated from a scratch tree — apply the series,
edit real `.c` files, re-diff only the touched files, splice — and dry-run
verified before the build.

This belongs in the dossier's prosecution list because it is invisible from
the file being edited: nothing in 0024 mentions 0030.

**`size_t` in scope for every CALLER is not every INCLUDER.** A header gaining
a prototype with a `size_t` parameter must include `<stddef.h>` itself; a file
that included it only for the syscall constants broke the build under
`-nostdinc`.

## The prover

Twenty new legs, covering every row above, **both** splitter branches, and —
the one that matters most — a **file-survival assertion**: "`unlink(file/)`
left the file alone". A strip-only wrong fix passes every errno assertion and
fails only that one.
