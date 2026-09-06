---
id: chg-2026-09-06-presenters-pl5-ls-pre-box
type: chg
title: "sub-coreutils-presenters delta: ls -l/la now frame RICH as a Beacon `pre` code-fence box (PL-5), not a `table` -- ps stays the genuine table; the ls-halcyon witness token is `1936;v1;pre`"
date: 2026-09-06
arc: arc-vault
commits: []
touched:
  - sub-coreutils-presenters
established: []
closed: []
opened: []
mirrors-checked: []
depth: skeletal
created: 2026-09-06
---
[[sub-coreutils-presenters]] (de-staled earlier this run @b28a2154), flagged by
main on yip 0059: PL-5 (`ea731dd8`, pushed `38693ab3`) changed `ls -l`/`la`'s
Rich-tier rendering from the semantic `table` it carried since H-1c-2 to a Beacon
`pre` code-fence BOX -- the box furniture as the `pre` payload, name cells `obj
type=path`, SGR off. Operator-ratified (HALCYON.md 14.13: a box-drawing emitter
wraps its output in `pre`, because a `table` renders proportional and would break
the mono box). Two lines were made imprecise; both corrected:

- The four-tools-frame paragraph said "`ls` and `stat` frame their listings
  likewise [as ps's table]" -- "likewise" over-generalized. Now distinguished:
  `ps` is the genuine `table`; short `ls` tags each name `obj type=path`; `stat`
  frames its listing; **`ls -l`/`la` are the exception -- a mono `pre` island
  since PL-5**, with the box furniture as the `pre` payload.
- The witness caveat cited ls-halcyon.exp's "ps framed, ls never did" as if
  current -- that was the PRE-FIX operand-vanished bug hunt. The current
  assertion is that `ls -l` DOES frame, as `1936;v1;pre` (was `table`). Added the
  new every-boot `coreutil-smoke` producer witness (`ls -l --beacon=always
  /version` -> `1936;v1;pre` + `obj`, strips to `┌`/`│`, 56 checks) and noted
  `usr/coreutil-smoke` is UNOWNED (sweep filed, like the yip-0029 Warp paths).

Verified: ls.rs's header documents the `-l` rich arm as the `pre`-box (table ->
pre-box restyle); PL-5 in-tree after FF to 38693ab3. This is the upkeep model in
action -- main ran owner (exit 0), rang the vault, vault folds; the third such
flag this run (0058 halcyond, 0059 here), which is why the code->dossier
commit-time reminder the operator just greenlit matters.
