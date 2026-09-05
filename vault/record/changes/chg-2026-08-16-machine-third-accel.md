---
id: chg-2026-08-16-machine-third-accel
type: chg
title: "A default tested by a real event, and a defence written before its collision"
date: 2026-08-16
arc: arc-vault
commits: ["37e5e96d"]
touched: [sub-substrate-machine]
established: []
closed: []
opened: []
mirrors-checked: []
depth: rich
created: 2026-08-16
---
Four commits since the dossier: a third accelerator, the graphics-host
substrate, the real-silicon bring-up, and a second boot token. Two of them are
worth keeping for reasons that are not about virtual machines at all.

## The absence-default was tested, not argued

The watchpoint exemption is advertised by a token whose **absence** means
"watchpoints work". The dossier already recorded the polarity as deliberate: a
new substrate cannot silently inherit the exemption, because inheriting it
requires someone to add it.

A third accelerator then arrived — hardware virtualization on ARM silicon — and
**did not inherit the exemption**, without anyone revisiting the decision. The
exemption is keyed to the one substrate that needs it rather than to a list of
substrates that do not, so the correct behaviour for a new arrival is to do
nothing, and doing nothing is what happened.

**That is a safe default demonstrated rather than reasoned about.** The
distinction matters because a default's whole claim is about cases nobody
considered, and until an unconsidered case arrives the claim is untested. This
one arrived and the default held.

## A defence written before the collision it prevents

The token array is **appended to, never assigned**, and the comment says why: a
rebuild would silently drop whichever token was added first.

A second token did later arrive. Had the array been rebuilt, that arrival would
have dropped the watchpoint token and re-wedged the original defect under
emulation — and **nothing would have failed loudly.** The guest would simply have
resumed arming watchpoints on the substrate that cannot deliver them.

**A second thing arriving is how the first thing gets silently voided.** This
vault has that pattern in several forms — a new gate hollowing out an old
negative test, a lifted constant voiding the proofs that named it — and they are
almost always recorded *after* the collision.

Here the defence predates it. Worth naming as the rarer and better case: the
author of the first token asked what the second one would do to it, before there
was a second one.

## Coverage is the claim being exercised somewhere, not everywhere

The second token opts a boot out of a compile-heavy pre-login gate, and the
reasoning is about **matched budgets** rather than about the gate.

That gate costs most of five minutes per boot under emulation, against an
interactive harness's five-minute login budget. So a disk image minted for the
compiler gate made every interactive scenario fail by timeout **with a completely
healthy guest** — the harness measuring one thing and being defeated by an
unrelated thing sharing its budget.

Opting out *removes* the mismatch rather than detecting it, which is what lets
one image serve both gates.

The part that makes this a good decision rather than a convenient one: the gate
still runs unconditionally on the ordinary boot, which is where the charter it
proves is actually tested. **It merely stops being repeated dozens of times by a
harness that is not testing it** — and a gate that re-runs an unrelated proof on
every scenario pays for it out of the budget of the thing it *is* testing.

## Two smaller checks, one of which I nearly skipped

The hardware arm's GIC selection is a **different kind of choice** from the other
two: both emulated arms name a version, the hardware arm names the host's and
delegates to the silicon. Fine, because the guest autodetects from the device
tree either way.

The consequence is that the announce line reads `gic=vhost` — a literal `v`
prefix in front of a non-numeric value. The dossier says harnesses read that line
back, so I checked whether anything parses the field numerically. **Nothing
does**; every consumer extracts the accelerator token alone. Cosmetic.

That check was worth running rather than reasoning about, and the dossier's own
sentence is what prompted it — a claim stated at the level of "harnesses read
this line" rather than "harnesses read this field" is exactly imprecise enough to
hide a real break. It did not hide one here.

The opt-out is compared against the string `1` rather than tested for
non-emptiness, because the emptiness test is true for the string `0` — so the
documented way to turn the feature off would have been inert on arrival. Small,
and in the family this project keeps hitting: **a control that cannot express its
own off state.**
