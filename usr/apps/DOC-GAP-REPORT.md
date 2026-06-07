# Documentation-Gap Report (native userspace authoring)

The auxiliary agent's top deliverable: a documentation-completeness audit of
`docs/reference/*` + the `libthyla-rs` public API, produced by writing native
programs against them using ONLY the documentation. Each gap is a place the docs
were missing, ambiguous, or wrong -- the main agent folds verified gaps into the
real docs later.

Append one entry per gap. Keep them specific (cite the file + section). Severity:
**P1** blocks authoring (no way to do the task from docs), **P2** costly
(had to read the API / guess), **P3** polish (unclear / missing example).

## Format

```
### G<NN> [P<sev>] <one-line title>
- App / task: <which app, what you were trying to do>
- Doc consulted: <docs/reference/NN-*.md section, or "USER-MANUAL stub", or
  "libthyla-rs API only">
- Gap: <missing / ambiguous / wrong -- what exactly was not derivable from docs>
- Workaround: <what you did instead -- read the pub signature, cribbed a sibling
  app, hand-rolled, or skipped>
- Suggested doc fix: <1 line: where + what to add>
```

## Findings

(none yet -- start with A0: "how do I build a native libthyla-rs app" is almost
certainly the first P1.)
