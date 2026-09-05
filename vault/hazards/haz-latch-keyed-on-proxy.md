---
id: haz-latch-keyed-on-proxy
type: haz
title: "A latch keyed on a proxy property fires on every class the proxy covers and the property does not"
applies-to: [global]
instances: [fnd-zoom-r1-f1]
created: 2026-09-05
updated: 2026-09-05
---
## The failure shape

A one-way classification (a latch, a mode flip, a tier assignment) is keyed on an OBSERVABLE that stands in for the property the classification exists for, because the observable is what the code has in hand. The proxy and the property coincide on the class that motivated the mechanism, so the mechanism is correct there and its tests pass. Then a second class arrives on which the proxy fires and the property does not, and the mechanism classifies it wrongly, permanently (the latch is one-way by design). tapestryd's #56 patchwork latch keyed on "a present's damage did not cover the surface" as a proxy for "the client rotates weave slots, so a slot is stale outside its damage". Aurora satisfies both. DOSBox-X (an SDL client: one slot, the app's own framebuffer, complete by construction) satisfies only the proxy -- it presents its menu bar and changed scanline bands as partial rects -- and was cropped at the content origin instead of letterboxed: in its tile, and zoomed, native at the display's corner on black.

## The tell

The mechanism's own doc names the property ("aurora's cell-diff over ROTATING weave slots ... each slot is patchwork") and then asserts a class exemption the code never checks ("the SDL class never latches"). A class named in prose but absent from the predicate is a proxy at work. A second tell: the proxy is a fact about ONE EVENT (this present's damage) while the property is a fact about the CLIENT'S DISCIPLINE (how it uses its slots over time) -- a per-event observation cannot witness a per-client property without history.

## The countermeasure

Key the classification on the property, observed directly (here: a bitmask of the slots ever presented; the latch needs partial damage AND two distinct slots), or on a declaration the client makes (the single-slot discipline as an API, `Surface::set_single_slot`), and write the negative control into the gate: the class the proxy would have caught and the property must not (a single-slot partial presenter stays letterboxed) beside the class the property must catch (the rotating presenter still latches) -- one variable apart, on the same surface.
