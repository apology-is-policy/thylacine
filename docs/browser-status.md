# Boosty -- browser arc status (the B-arc; `docs/BROWSER-DESIGN.md`)

The authoritative pickup guide for the web-browser arc. The design document is
the plan and the research; this tracks what has LANDED and what is next. The
arc sits outside ROADMAP's eight phases and must never put the v1.0 release
candidate at risk (ROADMAP section 11; BROWSER-DESIGN section 10, risk 7).

## TL;DR

**Ratified 2026-09-21; nothing built yet. The browser is named Boosty (the operator's cat).** The operator voted: **WebKit
first, then Servo; no stage 0 (no NetSurf, no `webfs`); the Rust `std` port
runs in parallel, owned by the aux track; effort `xhigh` throughout.** The
first implementation chunk is **B-0: JavaScriptCore alone** (`JSCOnly`, no
JIT) cross-built for `aarch64-thylacine` -- WebKit's own first step for a new
OS, and the cheap way to *measure* the anonymous-memory gaps (P1) and answer
the JIT question (B-2) instead of guessing.

## Landed chunks

| Commit | What | Witness |
|---|---|---|
| `c09141da` | `docs/BROWSER-DESIGN.md` PROPOSED: six research lanes, the tree's measured starting line, the platform tranche, the JIT mapping, the capability-graph design | docs only |
| *(pending)* | RATIFIED: the vote recorded (`dec-2026-09-21-browser-engine-order`), this status doc, the NOVEL.md candidate, the track-R brief for aux (`docs/handoffs/041`) | docs only |

## Remaining work (in order; BROWSER-DESIGN section 9)

| Phase | What | Exit |
|---|---|---|
| **B-0** | `JSCOnly` cross-build, JIT off (asm LLInt + IPInt). Source lives OUTSIDE this repo (a `webkit-thylacine` fork beside `llvm-thylacine`); the repo carries the build wiring and patches. | `jsc` runs on the device; the measured list of P1 gaps |
| **B-1** | P1, the anonymous-memory surface, scoped by B-0's measurements. **Scripture first** -- O-1 (reservation holes vs an I-12 wording amendment) needs the operator's signature. Audit-bearing. | allocators run; SMP gate; audit closed |
| **B-2** | The JIT: JSC's separated WX heap on `SYS_JIT_CREATE` + `SYS_ICACHE_SYNC`; `CAP_JIT` clearance. Audit-bearing (I-42/I-12). | benchmark with JIT tiers; deny-path probe (no `CAP_JIT` -> interpreter, never RWX) |
| **B-3** | P3: ICU, FreeType, HarfBuzz, sqlite, libxml2, png/jpeg/webp, libpsl, curl, OpenSSL; fontconfig decision (O-4). | each library's tests under Pouch |
| **B-4** | P2: its own design document (O-3: generalise the Weft share gate vs Mycelium), the primitive, then WebKit's `Platform/IPC` + `SharedMemory` backend. Audit-bearing (I-4). | two-process message + shared-bitmap witness |
| **B-5** | WebCore + WebKit2 headless, `PORT=Thylacine` modelled on PlayStation; P5 (EGL) resolved here; O-2 (which WebKit line to track) measured here. | `WKPagePaint` renders a local page to a PNG on the device |
| **B-6** | The chrome on Tapestry; P6; the constructed namespaces with deny-path probes. | a TLS page in a tile; content Proc proven unable to open `/net` |
| **B-7** | Hardening, fuzz posture, the owed invariant ENFORCED, the Operator's Manual section. | arc close |
| **R** | **aux**: Rust `std` for Thylacine, the crate tail, then Servo. Brief: `docs/handoffs/041-rust-std-track-to-aux.md`. | a `std` hello built by cargo and run on the device |

## Exit criteria status

- [ ] A page loads over TLS from the network and renders in a Halcyon tile.
- [ ] JavaScript runs with a JIT under strict W^X (no RWX page ever exists).
- [ ] A content Proc cannot name `/net`, `/srv`, `/proc` or `/dev` (deny-path probe).
- [ ] The owed section-28 invariant is allocated, then ENFORCED.
- [ ] The Operator's Manual has a browser section.

## Trip hazards

- **Effort is `xhigh`, by the operator's vote, for the whole arc.** Say so in
  every audit-bearing commit body; do not re-ask.
- **No permission-mutation syscall exists (I-12).** Do not add `mprotect` to
  make an allocator happy. B-1 is a scripture commit with a signature first.
- **`SYS_WEFT_SHARE` is gated to the driver tier on purpose** (Weft-7 F1,
  `kernel/syscall.c`). Read that audit before proposing to lift it.
- **Thylacine links statically and has no `dlopen`.** WebKit is LGPLv2: keep
  the build reproducible from published source so LGPL section 6 is met.
- **Swift is entering WebKit** (`ENABLE_BACK_FORWARD_LIST_SWIFT`; OFF when
  cross-compiling). Pin it OFF explicitly and watch for it becoming mandatory.
- **WebKit cannot be built on thyla-pi** (1.5-2 GB per unified job). Host only.
- **Never claim a research fact from memory.** The design's section 13 marks
  what was verified in source; five items are flagged unverified, and the
  first of them (every JSC writer uses `performJITMemcpy`) is B-2's first job.

## References

`docs/BROWSER-DESIGN.md`; `vault/record/decisions/dec-2026-09-21-browser-engine-order.md`;
`docs/NOVEL.md` ("The browser as a capability graph", "Mycelium", "JIT-as-a-capability");
`docs/JIT-ON-WX-DESIGN.md`; `docs/LLVM-DESIGN.md` (the C++ runtime, CL-2/CL-3);
`docs/POUCH-DESIGN.md` section 8 (memory); `usr/lib/thylajit/thyla_jit.h`.
