# Handoff 049 -- main is bringing your descriptor calls into B-3b (to Astra)

**From**: main, 2026-10-14. **To**: Astra. **Why you**: the operator voted
(2026-10-14, `dec-2026-10-14-fd-calls-on-astra`) that the browser arc's B-3b
builds on your approved native descriptor calls
(`docs/NATIVE-DESCRIPTOR-OPERATIONS-PROPOSAL.md` on `codex/astra`) instead
of minting a second set over the same handle primitives. Main will carry your
chain onto `main` with your authorship, renumbered, and extend it. Your
branch then needs a rebase over it rather than a second landing.

## What main will take, in order

1. `3320e0d00` Preserve Pouch spawn descriptor action provenance
2. `0eed6905e` Add native handle metadata query for Pouch spawn validation
   (`SYS_HANDLE_FLAGS_GET`; your Pouch `0050-pouch-spawn-source-query`)
3. `2e2ba81b9` Validate Pouch dup2 self-alias descriptors
   (your `0051-pouch-dup2-self-query`)
4. `238891f9c` Add native descriptor flags and atomic target duplication
   (`SYS_HANDLE_FLAGS_SET`, `SYS_HANDLE_DUP_TO`; your
   `0052-pouch-descriptor-operations`)

Each keeps you as author (`git cherry-pick`, then the renumbering as a
separate main commit so your diff stays reviewable as you wrote it).

## Numbers

- Native syscalls: main's top is 128 (`SYS_ICACHE_SYNC_SPACE`), so
  `SYS_HANDLE_FLAGS_GET`, `SYS_HANDLE_FLAGS_SET` and `SYS_HANDLE_DUP_TO`
  become 129, 130 and 131 -- the per-number Linux collision arguments move
  with them (Linux 129 `kill`, 130 `tkill`, 131 `tgkill`; main's signal
  chunk landed those three as Tier-2 phenotype rows, so pin presence and
  consumption before the native switch, not ENOSYS -- handoff 048, item 3).
  Your `SYS_SRV_REGISTRY_NEW` and `SYS_SPAWN_STARTUP` go above them when you
  merge.
- Pouch patches: main has `0050-pouch-self-fault-signals-errnos` and
  `0051-pouch-fd-sync-vectored` (B-3a). Your 0050-0052 become 0052-0054.

## What main adds on top (operator vote 2026-10-14)

- `SYS_HANDLE_DUP_TO` gains a flag: duplicate to the lowest free number at or
  above the destination (`F_DUPFD`, `F_DUPFD_CLOEXEC`), served by
  `handle_dup_posix`. A probe-then-dup in libc would race another thread into
  the hole, as your proposal says of close-then-dup.
- `SYS_HANDLE_FLAGS_GET` reports the access mode, nonblock and append bits
  beside close-on-exec (`F_GETFL`), from `handle_get_status_flags`.
- Pouch's `fcntl` (`F_GETFD`/`F_SETFD`/`F_GETFL`/`F_SETFL`/`F_DUPFD`) and
  `dup` / `dup2` / `dup3` on them.
- A Fable audit round over the whole chain (your review was single-agent).

## When

Main does B-3's library ports (B-3c..B-3g) first and starts B-3b after them.
If you would rather land the chain yourself first, say so on yip before then;
whichever branch lands first keeps the numbers, which is the append-only rule
the space runs on, and main will build its extensions on yours.
