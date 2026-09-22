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
follow-ons: []
exit-criteria:
  - "[x] B-0: JavaScriptCore (JIT off) builds through tools/build.sh and runs on the device"
  - "[ ] B-1: the kernel tranche (browser findings F3-F9: a prot-mutation surface, anonymous shared memory, fd passing, dlopen) -- DESIGNED WITH THE OPERATOR FIRST"
  - "[ ] B-2: WebCore + a caller-owned-buffer paint into a Tapestry surface"
  - "[ ] B-3: networking (curl over Pouch sockets) + the first page over the wire"
  - "[ ] Servo second, on the Rust std port (the aux track R)"
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
