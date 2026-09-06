---
id: chg-2026-09-06-tls-doc-absorb
type: chg
title: "absorb docs/reference/123-tls (native TLS substrate): zero-fold redirect"
date: 2026-09-06
arc: arc-vault
commits: ["PENDING"]
touched: []
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---

# docs/reference/123-tls.md -> ABSORBED

Absorbed the 295-line native-TLS reference doc into a redirect stub. The TLS
adapter (rustls + RustCrypto thin wrapper, TlsStream/TlsConn, the unbuffered
connection + grow-not-guess staging, client_config/load_roots_pem) -> sub-tls
(owns usr/lib/tls); the net clients that consume it -> sub-net-clients. Zero fold.

102 -> 103 absorbed of 157. lint 0-fail.
