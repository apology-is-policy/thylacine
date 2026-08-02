---
id: seam-dtb-blob-internally-trusted
type: seam
status: open
title: "The device-tree parser validates its callers, not its input"
surface: [sub-kernel-dtb]
opened-by: chg-2026-08-02-boot-sweep
tracker: ""
created: 2026-08-02
updated: 2026-08-02
---
## What

Initialization checks the blob's magic number and its format version. It does not
check that the block offsets and sizes in the header lie within the blob's stated
total size. The walker takes the structure block's declared length at face value
and sets its end pointer from it.

Within the walk, a property's declared length and its name offset are likewise
used without bounds checks. A caller that reads the declared number of bytes from
a property — which several boot lookups do — reads past the block if the length
is wrong. A name offset can point outside the strings block.

The tree-walk surface layered on top for userspace consumption is different: every
*caller-supplied* offset is validated against the block size before a pointer is
formed, and its name scans are length-bounded. That surface's header comment is
accurate about what it does. But of its two entry points, only one also validates
the *blob-supplied* name offset it hands back; the other forms the pointer from
the untrusted offset and then measures it with an unbounded scan.

## Why this is the right posture today

The blob is supplied by firmware, before anything else runs. A machine whose
firmware emits a malformed tree has a problem the kernel cannot mitigate, and the
kernel has no way to obtain a second opinion — the tree is the only description
of the machine there is. Validating it thoroughly buys nothing against an
adversary, because at that point in boot there is no adversary who is not already
in a stronger position.

The parser's soft-failure posture is also load-bearing in the other direction:
every lookup already returns "absent" rather than failing, because a single binary
must boot on machines that differ. Adding hard rejections would need care not to
turn a machine that merely lacks a device into a machine that will not boot.

## Why it is worth recording

The threat model is stated nowhere near the code that depends on it. The parser
reads as though it were defensive — bounded string scans, explicit length checks
on caller offsets, a comment about rejecting forged offsets "rather than following
them into arbitrary memory" — and it *is* defensive, about the boundary it was
built for. A reader can easily carry that impression across to the blob itself.

The asymmetry between the two userspace-facing accessors is the concrete evidence
that this is a boundary someone can be wrong about, since both are on the same
surface and only one validates.

## Trigger

Anything that makes the tree stop coming from trusted firmware:

- A guest-supplied or user-supplied device tree, on any virtualization path.
- Device-tree overlays applied after boot from a filesystem.
- A tree assembled or patched by an earlier-stage loader that is not part of the
  trusted set.

Also, more quietly: any widening of the userspace tree-walk surface that lets a
caller reach the parser with a shape the boot lookups never produce.

## Fix, when triggered

Validate at initialization that both blocks lie wholly within the total size, and
bound each property's length and name offset against its block during the walk.
The walk already has both bounds available; the checks are cheap and would be
correct to add unconditionally.

The second half is to make the two userspace-facing accessors consistent — the
one that validates the name offset is the model.

## No task

Not reachable and not a defect under the current threat model: there is no path by
which an untrusted party supplies the blob. Recorded so that the first path that
does arrives with the gap already named, rather than discovering it afterwards.
