---
id: seam-larder-cacheable-proxy
type: seam
title: "The cacheability latch is a POUNCE-success proxy, not the true property"
status: open
surface: [sub-kernel-larder, sub-kernel-ninep-dev9p]
opened-by: chg-2026-07-09-larder-l1e
tracker: ""
created: 2026-07-31
updated: 2026-07-31
---
**Owed**: an explicit attach-time cacheability capability. The Larder
engages only for a `cacheable` client — a per-`p9_client` flag latched
true by the first successful `Twalkgetattr` (POUNCE), the v1.0 proxy for
"a content-versioned, offset-stable FS". Stratum speaks POUNCE and
latches; netd answers ENOSYS and never does (its `/net` reads are
CONSUMING — the same offset returns different bytes — and `qid.version`
is always 0, so caching it would serve stale stream bytes).

**What closes it**: a declared per-mount capability (the Plan 9
interpose-cfs-per-mount idiom) carried at attach time, replacing the
behavioral proxy.

**Risk while open**: the proxy is FAIL-SAFE in the direction that
matters (unproven → never cached — a perf loss for a hypothetical
POUNCE-less content-versioned FS, never a stale read). It breaks only
for a future server that speaks POUNCE but streams — no such server
exists in the v1.0 set (Stratum, netd; corvus is byte-mode, no Larder;
`/proc` `/ctl` `/env` `/dev` are native Devs). Any NEW dev9p-served
server must be checked against this proxy before deployment — that check
is what this seam exists to force.
