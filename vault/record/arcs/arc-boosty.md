---
id: arc-boosty
type: arc
title: "Boosty: a web browser for Thylacine (WebKit first, then Servo)"
status: active
design: ["docs/BROWSER-DESIGN.md", "docs/browser-status.md"]
chunks:
  - chg-2026-09-21-boosty-b0-jsc
  - chg-2026-09-21-pouch-b0-libc
  - chg-2026-09-21-mount-shed
  - chg-2026-09-23-b1a-permission-ceiling
  - chg-2026-09-23-b1a-prime-capacity
follow-ons: []
exit-criteria:
  - "[x] B-0: JavaScriptCore (JIT off) builds through tools/build.sh and runs on the device"
  - "[ ] B-1: the kernel tranche (browser findings F3-F9: a prot-mutation surface, anonymous shared memory, fd passing, dlopen) -- DESIGNED WITH THE OPERATOR FIRST"
  - "[ ] B-2: WebCore + a caller-owned-buffer paint into a Tapestry surface"
  - "[ ] B-3: networking (curl over Pouch sockets) + the first page over the wire"
  - "[ ] Servo second, on the Rust std port (the aux track R)"
  - "RENUMBERED 2026-09-23 (BROWSER-DESIGN 9): the B-1..B-3 lines above are the first plan; the phases below are the arc's phases now, and docs/browser-status.md carries their state"
  - "[x] B-1 (renumbered): the kernel tranche -- permissions, capacity, dlmalloc, dlopen (B-1a..B-1d)"
  - "[x] B-2 (renumbered): the JIT on the separated-WX code region, JSC Baseline through FTL and Wasm (B-2a..B-2e3)"
  - "[ ] B-3 (renumbered): the C libraries under Pouch, each library's compiled upstream tests on the device (B-3a..B-3l, votes 2026-10-14)"
  - "[ ] B-4: the IPC primitive and WebKit's Platform/IPC + SharedMemory backend"
  - "[ ] B-5: WebCore + WebKit2 headless renders a local page to a PNG on the device"
  - "[ ] B-6: the chrome on Tapestry; a TLS page in a tile; content Proc unable to open /net"
  - "[ ] B-7: hardening, fuzz posture, the invariant ENFORCED, the manual section"
created: 2026-09-21
---
## Goal

A real web browser on Thylacine, named Boosty. The operator's ratified
order ([[dec-2026-09-21-browser-engine-order]]): WebKit first, Rust `std`
in parallel on the aux track, Servo second, no stage 0. Nothing Blink, V8
or Chromium.

## Planned chunks

B-0 is the starting line: JavaScriptCore alone, JIT off, as a Pouch port.
It needed no kernel change and found five libc lies on the way
([[chg-2026-09-21-pouch-b0-libc]]), and the gate fleet it ran on turned
out to be red on `main` for an unrelated reason the arc then fixed
([[chg-2026-09-21-mount-shed]]). B-1 is kernel design and is a
conversation with the operator before it is a chunk.

## Close summary
(written at status flip to complete)
