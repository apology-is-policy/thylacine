# 124 — net-utils: the native network CLI tools [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-net-utils-absorb`).
The native network command-line tools (`nslookup`, `ping`, `curl`, `wget`) —
libthyla-rs programs that are all clients of netd's `/net` tree (they touch no
hardware; I-5). Their content lives, code-verified and current, in:

- the **fetchers** — `curl`/`wget` (two binaries over one engine), HTTP/1.0 with
  explicit-close, the TLS client, the baked-CA self-test, and the
  randomness-capability dependency for the https handshake (the plain paths need
  no capability):

      vault/system/userspace/tools/sub-net-clients.md

- the **presenter tools** — `nslookup`/`ping` as the coreutils binaries they are
  (`ping.rs`, `nslookup.rs`), the colour/output tier they share with the other
  presenters:

      vault/system/userspace/tools/sub-coreutils-presenters.md

- the **`/net` protocol they consume** — the cs/dns resolver (numeric -> static
  ndb -> DNS), the ICMP echo path (the rotating Echo ident), the resident
  loopback; **and the two client-facing atoms folded here at this absorption**:
  the `/net/cs` 0-service resolve footgun, and `ping`'s seam #256:

      vault/system/userspace/services/sub-netd-server.md   (audit: hard)

- the **shared client primitives** — `net::resolve` (the one front door) and
  `net::IcmpSocket`, in libthyla-rs's `net.rs`:

      vault/system/userspace/runtime/sub-libthyla-rs.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Two client-facing gotchas were uncovered — folded at absorption.** The tools
  and the `/net` server were both owned, but two consequences of the protocol
  lived only here: (1) the **0-service footgun** — a resolve for the IP only must
  still pass a non-zero service, because `/net/cs` falls through to an ndb lookup
  of the literal `"0"` and misses (folded into the netd-server cs resolver
  mechanism); and (2) **seam #256** — `ping`'s ICMP-error recv can be fooled into
  waiting out its 1 s poll when only a Destination-Unreachable (quoting our ident)
  arrives, the v1.x fix being a per-`recv` deadline (named on netd-server's
  existing ICMP caveat, whose server-side WouldBlock mechanism was already there).
- **The rest is current and home** — the curl/wget engine (HTTP/1.0, TLS, the
  CAP_CSPRNG-for-https dependency) in sub-net-clients; the ping/nslookup binaries
  in sub-coreutils-presenters; the resolver + ICMP protocol in sub-netd-server.
  The deterministic boot proofs (`nslookup localhost`, `ping 127.0.0.1`,
  `curl/wget --selftest`) are covered by the tools' self-test discipline. No
  traceroute (slirp is a single NAT hop) remains a correct v1.0 non-goal.
