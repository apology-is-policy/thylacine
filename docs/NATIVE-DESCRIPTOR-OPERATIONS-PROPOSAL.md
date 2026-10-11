# Native descriptor operations for the Pouch toolchain

> **On main** (branch b3, B-3b, operator vote 7 of 2026-10-14) these calls are
> `SYS_HANDLE_FLAGS_GET = 129`, `SYS_HANDLE_FLAGS_SET = 130` and
> `SYS_HANDLE_DUP_TO = 131`: main's 128 is `SYS_ICACHE_SYNC_SPACE`. Linux
> 129-131 are the kill family, which main translates (VIVARIUM 6.22), so the
> collision guard on main is the kill family's TIER2 rows, consumed before
> native dispatch, not the ENOSYS rows described below. Main also extends
> DUP_TO with a lowest-free-at-or-above flag and FLAGS_GET with the open
> mode, nonblock and append (vote 7). The text below keeps the numbering
> and the collision analysis this design was written against on codex/astra.

Status: APPROVED by the operator, October 11, 2026; implementation and bounded
qualification complete. See NATIVE-DESCRIPTOR-OPERATIONS-REVIEW.md.
Source reviewed at 2e2ba81b9. This is a single-agent design review, not an
independent audit. The approval reserves native129/130 as specified below.

## Approved decision

Implement two native operations and their bounded Pouch consumers: a setter for
the existing per-handle close-on-exec flag, and atomic duplication onto an exact
handle index. Use the kernel's existing ownership and alias rules. Include
Pouch's current spawn interpreter in flag-consistency tests before qualifying
its consumers. This is a descriptor checkpoint on the path to native Ninja
self-build, not approval of a replacement process/signal architecture.

The setter addresses Ninja's SetCloseOnExec call. Exact-target duplication
addresses Pouch's open dup2/dup3 seam and its libc callers, including freopen.
Neither alone supplies Ninja's required process groups, masks and waits.

## Existing mechanisms and alternatives

`kernel/handle.c` already implements `handle_set_cloexec` and `handle_dup_to`.
The latter locks the table once, validates the source and alias policy, acquires
an object reference, replaces the destination, and releases its previous object
after unlocking. Rights are copied verbatim. These helpers are already used by
Vivarium; they are not exposed through native syscalls. Native128 supplies the
getter. Its query is an observation, not a reference or concurrent snapshot.

[Plan 9 rfork](https://9p.io/magic/man2html/2/fork) makes descriptor-table copying
or clearing an explicit process-creation choice. Fuchsia's
[pinned historical fdio spawn interface](https://fuchsia.googlesource.com/zircon/+/13ee3dc5e4c46bf127977ad28645c47442ec517d/system/ulib/fdio/include/lib/fdio/spawn.h)
distinguishes cloning a descriptor into a chosen child slot from transferring
ownership. These precedents support explicit native ownership operations with
the POSIX adaptation kept in Pouch; they do not establish Thylacine correctness.

Options considered:

1. **Recommended: expose the existing native operations.** Small register-only
   ABI addition, no new handle ownership model; requires compatibility guards
   and full source-matched kernel qualification.
2. **Prepare the full fork/exec Pouch migration first.** Potentially replaces the
   static spawn interpreter, but entails environment, child error reporting,
   multithreaded safety and signal policy. It still needs descriptor operations.
   Native EXECVE104 and RFORK105 already exist: Pouch's comment that the kernel
   has no fork/exec is stale and must not justify inventing another spawn ABI.
3. **Retain explicit refusal.** No new ABI, but descriptor-dependent native
   toolchain work remains incomplete. Close-then-lowest-free-dup is not an
   implementation alternative: another thread could occupy the target hole.

## Approved register contract

Use existing `SYS_HANDLE_CLOSE_ON_EXEC` bit0 (value1). No Linux flag numbers
cross this boundary. All arguments are full-width registers; validate before
narrowing. Neither call takes user pointers or names another process.

| Operation | Arguments | Success |
| --- | --- | --- |
| SYS_HANDLE_FLAGS_SET = 129 | x0 handle, x1 flags (0 or bit0) | 0 |
| SYS_HANDLE_DUP_TO = 130 | x0 source, x1 destination, x2 flags (0 or bit0) | destination index |

Errors are negative native errno. Validation order is: current thread/process
(EINVAL), flag bits (EINVAL), full-width index bounds (EBADF), and for DUP_TO
same index (EINVAL), then locked source validity (EBADF) and alias policy
(EACCES). SET checks the live slot under the same lock as its mutation. DUP_TO
allows a free or occupied destination in the caller's fixed-size table; a full
table does not prohibit replacing an occupied target. Invalid requests leave
both slots and flags unchanged. EPERM is deliberately not an output of either
contract; old kernels' unknown-call raw -1 can therefore be mapped to ENOSYS at
these specific Pouch adapters, as with128. Native wrappers retain raw results.

SET replaces only the existing descriptor-local flag, without object I/O,
reference acquisition, rights tests or changing open-file status. This flag
controls existing image replacement; it does not redefine native explicit
spawn transfers or create a close-on-fork flag.

DUP_TO uses the existing alias predicate, including refusals for hardware,
Srv/Loom and devsrv Spoors. It cannot increase rights. A narrow-rights source
retains exactly those rights at the target. The target flag is assigned from
the argument, never inherited from either slot. Acquiring the new reference
and publishing the replacement happen while the table is locked. The outgoing
reference is released after unlocking; its close hook may sleep. This preserves
the current helper's release behavior, with no new post-publication error or
rollback path. No intermediate empty slot may be visible.

Introduce an errno-returning internal core if needed; preserve the existing
flat-failure helper for current callers. Determine EBADF versus EACCES inside
that same lock, never through a second query after failure. No caller may use
GET followed by SET/DUP as an authority or lifetime proof. Concurrent close or
reuse is ordered at each individual operation's lock acquisition.

## Linux collision protection is part of the same change

Linux kill129 and tkill130 currently have no classification rows. They forward
only because they lie above the native ceiling128 and ultimately fail. Raising
the ceiling without changing that policy would let Linux signal arguments reach
native descriptor mutation. Add mandatory VIV_ENOSYS rows for both numbers in
the same implementation checkpoint, pin their collisions with static asserts,
and move the next above-ceiling assertion to tgkill131. Keep restart128 refusal.
Test actual Linux-personality calls with arguments that would mutate a live
native handle if forwarded, checking both ENOSYS and unchanged descriptors.
No Linux signal implementation or Linux-number forwarding is approved here.

## Bounded Pouch consumers and consistency

- Mirror constants and raw operations in native C/Rust headers and wrappers.
- Implement public fcntl F_GETFD via128 and F_SETFD via129. GET consumes no
  variadic argument; SET reads the promoted int. Translate FD_CLOEXEC explicitly
  and reject unsupported descriptor flag bits with EINVAL. Other fcntl commands
  remain ENOSYS without reading an argument. No fake success or generic Linux
  syscall dispatcher; audit internal raw SYS_fcntl users and label their gaps.
- dup2 with distinct native descriptors uses130 with flags0; its qualified
  self-alias behavior remains unchanged. dup3 uses130 with translated O_CLOEXEC,
  rejects self-alias and unsupported flags with EINVAL. Preserve __dup3 linkage.
- Pouch tagged sockets have a different owner. Never pass either tagged operand
  to the native table. Existing self-dup validation remains; live tagged flag
  queries/setters and distinct-target duplication remain explicitly ENOSYS until
  their owner has a coherent implementation. Invalid tagged lookups return
  EBADF. Test that no unsupported tagged request mutates either owner.
- Adding a setter must not let the current Pouch spawn path silently ignore
  flags. Track descriptor-local flags for seeded standard streams; process
  ordered file actions before removing final close-on-exec slots. A DUP2 action
  clears the child's target flag, including a self-action; ordinary dup2(fd,fd)
  instead preserves it. Parent flags must never be changed to simulate a child.
  Keep all existing original-source validation and temporary-ownership guards.
- For this bounded interpreter, reject OPEN actions requesting O_CLOEXEC with
  ENOSYS before opening temporaries. Its parent-side open/child mapping cannot
  yet promise the complete open-action flag behavior. Ordinary supported OPEN
  actions establish a child slot with its flag clear. Preserve the existing
  limit and refusal for sparse final tables: never compact a hole or leak a
  flagged descriptor to force a contiguous list.

The spawn ordering follows [POSIX spawn](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html).
The special self-action rule is also documented in the
[adddup2 interface](https://docs.oracle.com/cd/E88353_01/html/E37843/posix-spawn-file-actions-adddup2-3c.html).
The supported dup behavior follows [POSIX dup](https://pubs.opengroup.org/onlinepubs/9799919799/functions/dup.html).
These are explicit supported subsets, not a full POSIX.1-2024 claim.
General inheritance, arbitrary sparse child tables, socket ownership, atomic
open-with-flags and unsupported internal fcntl users remain named gaps; do not
present this checkpoint as clearing them or qualifying the complete workload.

## Required qualification after approval

1. Commit approved design before implementation. Add ABI registry/assertions,
   syscall dispatch, C/Rust wrappers and collision guards together. Preserve
   current Vivarium helper behavior. Review existing handles model obligations.
2. Test full-width narrowing aliases, flags, absent process, absent source,
   alias-policy failures, narrow rights, full table, free/occupied targets,
   last-reference close, shared-object replacement and exact counter balances.
   Cover concurrent source close/reuse and target allocation ordering without
   assuming a stable descriptor identity between calls. Release must be outside
   the table lock. Existing clean/mutant models remain applicable.
3. Actual-source Pouch host tests at O0/O2 with ASan+UBSan: getter without a
   vararg, setter/dup flags, negative and tagged inputs, legacy -1/ENOSYS,
   spawn action order and self-actions, flagged-stream exclusion, sparse refusal,
   failed OPEN preflight and cleanup. Mutants remove flags, collision guards or
   validation; demonstrate the corresponding witnesses fail.
4. Build official sysroot and source-matched kernel; compile native wrappers.
   Ordinary-user guest witnesses exercise real metadata, exact target identity,
   flag effects through native image replacement, Pouch spawn inheritance,
   full-table operation and raw Linux129/130 refusal. Check old-kernel refusal
   without state mutation; retain every failed artifact and disposition.
5. Run required kernel default/UBSan SMP matrix with matching source/artifacts,
   including required existing model gates. Verify raw results, cleanup and
   protected-file hashes, perform single-agent self-review, then checkpoint with
   normal hooks. Preserve 8GiB floor and acquire/release the Mac resource.

After this descriptor checkpoint, design the remaining startup/mask/wait/group
contracts against existing104/105 rather than stale Pouch comments. Native Ninja
self-build and CMake/OpenRCT2 source builds still need those concrete gates.
No Main landing, parked SQ work, MM phase skip, private async/clipboard activation,
128MiB protection removal or monitor reactivation is part of this proposal.
