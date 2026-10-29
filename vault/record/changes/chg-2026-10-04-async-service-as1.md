---
id: chg-2026-10-04-async-service-as1
type: chg
title: "Resumable native service framing and handshake"
date: 2026-10-04
arc: arc-halcyon-interaction
commits: []
touched: [sub-kernel-ninep-transport, sub-kernel-ninep-session, sub-kernel-loom, sub-kernel-ninep-client, sub-kernel-srvconn]
established: []
closed: []
opened: []
depth: skeletal
---
AS-1 under approved scripture4722f34e8: bounded nonblocking framing and native
handshake reuse the transport/session engines and SrvConn role-aware try-I/O.
Absolute deadline and terminal abort do not require a peer response. Shared
version dispatcher now refuses unsupported dialect/zero framing size (AS-R1).
Actual-source byte-boundary tests,12 intended mutants, LLVM ASan+UBSan with a
positive fault canary, ARM64 compilation, existing9p_client197states/five buggy
configs and fresh CPU1 boot1830/1830 pass. Apple ASan startup deadlock isolated
before main in minimal and full-fixture samples (AS-R2); scoped LLVM invocation
works. Single-agent review; broad matrix follows. No private userspace activation,
completed clipboard, fresh graphics/Pi or Main landing.
