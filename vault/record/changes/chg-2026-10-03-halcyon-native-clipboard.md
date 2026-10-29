---
id: chg-2026-10-03-halcyon-native-clipboard
type: chg
title: "Qualify the native Halcyon clipboard adapter"
date: 2026-10-03
arc: arc-halcyon-interaction
commits: []
touched: [sub-halcyond, sub-halcyond-service-wire, sub-halcyond-application]
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
---
An explicit nondefault interaction-qualification feature now lets the per-session
pane service route the interaction sibling through HIN1,
the existing Interaction owner and the ordered HIA/HSC channels. Accepted peers
are sampled from the kernel at every frame and admission receipt. Eight fids use
monotone local incarnations. Final writes park by exact tag/ticket; flush, clunk
and cross-fid Unbind remain usable while admission waits. Internal application
failures never fall through into media handlers. SAK closes nonmedia connections
and their pending, cached and partial output before cancellation acknowledgement.

Normal builds retain two media connections and do not expose interaction or its
locator. Qualification builds use 32 control, 2 media and 4 unbound handshake reserves.
Build with --set HALCYON_INTERACTION_QUALIFICATION=y (or the legacy
THYLACINE_INTERACTION_QUALIFICATION=1 shim); tools/build.sh supplies the
halcyond/interaction-qualification Cargo feature. The environment shim must be 0 or 1.
The broader pressure/failure qualification is still required before making it
a default or landing it in Main. Actual input
capacity plus all record allocations share 32 KiB; negotiated 9P frames are 8 KiB.
Read responses borrow existing snapshots into the accounted output buffer.
In that build, the image residual reserves 7.375 MiB payload/protocol storage, 512 KiB metadata
and mapping allowance, and the separate 128 KiB worker stack plus 4 KiB guard.
Compile-time bounds constrain each connection to 8 KiB, Link 48 KiB and Shared 16 KiB.
This is not a claim that kernel memory equals the userspace allowance: weighted
service rings, kernel 9P clients, outstanding RPC buffers and retained fids are
separate costs. HI1-R24 stays open for the complete kernel-pressure ledger and
all-slot/multi-session native workload before general activation/Main landing.

HI1-R28: the native poll now uses the earliest handshake/admission/transfer
expiry, so a quiet compositor cannot hide the deadline. The real 9P fixture holds
a WRITE without Rwrite for 30 seconds. Production close/join abandons callbacks;
a late reply on the retained 9P session cannot corrupt a fresh ring. This is
local I/O retirement, not remote rollback or proof of a malicious mid-frame peer.
Loom's existing mid-frame join trust assumption remains unchanged.

A first controller Bind may race the ordered foreground transition. The probe
retries only Gone/Busy pre-authority registration, with new IDs and 8 bounded
attempts separated by 20 ms. It never retries CommitCopy. Retained failed runs
show the stale epoch was refused. Other review fixes: cross-fid Unbind was
initially over-serialized; internal app errors could fall back to media parsing;
and the expanded service reserve was missing from the image residual.

Measured evidence: 554 Halcyon host tests, 160 actual-protocol tests and eight intended
mutant failures pass; typed build-config and production checks pass. Fresh default-off
CI CPU1 boot passes 1830/1830; native service-wire passes in 66.06 s, including the
30-second withheld-Rwrite teardown and late-reply isolation. Opt-in graphical clipboard
passes in 49.58 s, media/manual in 71.51 s and extended physical F10 SAK in 185.86 s.
The graphical runtime source hashes match final source; only typed build-config/schema
tests were added afterward.

The original diagnostic graphical run verified 20,000 bytes across two processes,
then F10 connection retirement and post-SAK reconnection in 49.48 s. Final captures
and exact source/image manifests are in work/oct3-hi-native. Single-agent
self-review; all four unrelated drafts preserved. No Main landing, independent
audit, Pi/minimum-display, fresh SMP/sanitizer, 32-controller/multi-user pressure
or native partial-output-under-SAK qualification is claimed. Interactive shell/
Nora clipboard clients, transcript modes and the accent INS/NOR/VIS chip remain.
