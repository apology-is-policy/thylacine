---
id: dec-2026-10-22-cached-open-retains-walk-fid
type: dec
title: "A cached open keeps its walk fid unopened, so a metadata change through it works"
date: 2026-10-22
status: standing
decided-by: user-vote
affects: [sub-kernel-ninep-dev9p]
created: 2026-10-22
---
## Fork

FID-LIFECYCLE-DESIGN 3.3's cached open serves a read-only open of a small
fully cached file from a snapshot, with no server fid. Tsetattr is
fid-addressed, so a wstat through that descriptor (fchmod, fchown,
ftruncate, futimens) answers EOPNOTSUPP. 3.3 deferred the fix "if a real
consumer appears". Lease 5 measured a wider reach than its premise: G1's
write-populate makes a just-written file qualify, and an empty file
always does; Go's `syscall.Futimes` on `os.Open` of a just-written file
hit it on the device.

## Options

1. **Retain the walk fid unopened**: the revalidating Twalkgetattr binds a
   fid, the open keeps it without Tlopen, wstat and getattr address it,
   and close clunks it asynchronously (3.3's own named fix).
2. Narrow eligibility: refuse the cached open for files with own-written
   pages and for empty files. A cold cached file still refuses.
3. Keep the refusal, documented, until a consumer appears.

## The call

Option 1 (operator, 2026-10-22, the recommended option), sequenced on
`b3` after [[dec-2026-10-22-env-bind-at-open-sequenced]]. Scripture first
(3.3's seam paragraph and its round-trip accounting).

## Rationale

Every descriptor then behaves as POSIX says. An open still costs one
synchronous round trip; the price is a server fid per open file and the
asynchronous clunk, which the design had already counted.
