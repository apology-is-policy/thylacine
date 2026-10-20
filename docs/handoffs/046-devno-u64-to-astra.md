# Handoff 046 -- the 64-bit device number (to Astra)

**From**: main, 2026-10-06. **To**: Astra (and Corona, who works from the same
base). **Why you**: this landing changes the `t_stat` record every stat caller
reads, the kernel's device-number type, and one Linux-compat rendering.
A trial merge (`git merge-tree`) of the landing branch into codex/astra
(`5ff62b788`) and corona/async-memory (`a15284737`) conflicts in the same 26
files a merge of main does: this chunk adds none.

## What changed

The operator voted (2026-09-28, `dec-2026-09-28-t-stat-devno-u64`) to widen
`t_stat.devno` in place to 64 bits. Read `chg-2026-10-06-devno-u64` for the
whole of it.

- The kernel's device number (`spoor_next_devno`, Plan 9 `Chan.dev`) is `u64`,
  monotonic and never reused. Every stored copy is `u64` and pinned to the
  Spoor field's width by a `_Static_assert`: the Spoor, the Env, the mount
  entry (`PgrpMount`, still 40 bytes: `mp_devno` now sits before `mp_dc`, and
  `_pad` is gone), `struct mkey`, the shed's instance set, the Image cache entry
  and the FILE Burrow's key copy.
- `t_stat.devno` is `u64` at offset 80 over the old `_pad_dev`; the record is
  still 88 bytes. libt (`unsigned long devno`), libthyla-rs (`Metadata::dev()`
  returns `u64`, offset pinned), pouch patches 0010 / 0019 / 0021 and
  go-thylacine's `Stat_t.Dev` changed with it.
- diorama's `/proc/<pid>/maps` device column is `major(devno):minor(devno)`,
  the glibc/musl split, so it agrees with vivarium's `st_dev = devno` for every
  devno (it read `00:<devno>`, which agreed only below 256).
- On a KERNEL_TESTS boot the minter is advanced past 2^32 by a suite test, so
  all of userspace after the suite runs on device numbers above 2^32.

## Rules the merged code must keep

1. A devno is `u64` wherever it is stored, passed or compared. A new copy gets
   a `_Static_assert` against `sizeof(((struct Spoor *)0)->devno)`.
2. No code may reuse, refuse or wrap a device number; an Image cache entry
   outlives the Spoor it was keyed from.
3. Any new `t_stat` mirror declares `devno` as 64 bits at offset 80 and pins
   both the size (88) and the offset.
4. A new renderer of a device number for a Linux reader splits it as
   `gnu_dev_major` / `gnu_dev_minor` do.

If a branch of yours adds a `t_stat` reader, a `PgrpMount` initialiser or a
devno parameter, re-check it against these rules after the merge; the build
does not warn on a u64-to-u32 narrowing (no `-Wshorten-64-to-32`).

**Update 2026-10-07 (aux):** the kernel now builds with
`-Werror=shorten-64-to-32` (`chg-2026-10-07-narrowing-error`), so an implicit
narrowing in kernel C fails the build. The sentence above still holds for
userspace C (libt, joey, the pouch patches), which those flags do not cover.

## Carried from 045 (one site, named)

045's rule 5 (every change a Loom waiter must drive bumps `drive_gen` under
`l->lock`) has one concrete site on your line: `loom_post_pool_cqe`
(`kernel/loom.c`, from c822021a2, 2026-10-04) publishes a CQE (`cq_tail + 1`
under `l->lock`) and will need `loom_drive_moved_locked(l);` just before its
`spin_unlock(&l->lock)` when codex/astra is merged with main. It is the only
other writer of `h->cq_tail` on 5ff62b788 besides `loom_post_cqe`, which gets
the bump from main. Corona has recorded the same obligation for her merge.
