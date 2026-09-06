---
id: chg-2026-09-06-libdriver-caps-vocabulary
type: chg
title: "libdriver-grant de-stale: the H-4b-1 caps = [...] fork-grantable-capability vocabulary (the closed Cap enum, named-not-numbered, fail-closed parse)"
date: 2026-09-06
arc: arc-vault
commits: ["d8281e1f"]
touched:
  - sub-libdriver-grant
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-libdriver-grant]] (updated 2026-08-03) missed the H-4b-1 manifest change
`4bb6048c` (+109 on `manifest.rs`), verified in the source: the `caps = [...]`
axis. Folded:

- `Manifest.caps: Vec<Cap>` -- fork-grantable capabilities beyond the implicit
  `CAP_HW_CREATE`, empty by default. It is the ONE manifest axis that is not a
  device resource and not a pure decline: it asks the warden to CONFER authority,
  so its safety is not the intersection's "node supplies the values" property.
- `enum Cap { Csprng }` (`CAP_CSPRNG_READ`, for tapestryd's unguessable placement
  claims). NAMED not numbered because the crate is pure (no libthyla-rs) -- the
  warden resolves name->`T_CAP_*` bit at spawn and must hold the bit itself (I-2
  monotone; the [[sub-warden]] side). `Cap::parse`/`Cap::name` are an exact inverse
  pair so `to_text` round-trips; the vocabulary is CLOSED (unknown name -> `None`
  -> parse error) and a repeated name in the list or a repeated `caps` key is also
  a parse error (fail closed on both) -- a typo can neither widen nor narrow.

Test count MEASURED: 39 (manifest.rs 14, was 11 -- three caps tests added; the
dossier's "36" was current for its date), resource.rs 25 unchanged. Folded into a
new Mechanism subsection, Data structures (Manifest + Cap), Error paths, Tests,
Provenance. `updated:` -> 2026-09-06. Stale backlog 36 -> 35.
