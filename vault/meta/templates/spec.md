---
id: spec-{name}
type: spec
title: "{name}.tla"
models: []
pins: []
cfgs:
  - "{name}.cfg -- clean"
  - "{name}_buggy.cfg -- buggy: counterexample of {invariant}"
gate: "{when a re-run is mandatory}"
created: {YYYY-MM-DD}
updated: {YYYY-MM-DD}
---
## Abstraction
{What is deliberately beneath the model.}

## Action-site map
{TLA action <-> code symbol. Absorbs SPEC-TO-CODE.md for this module.}
