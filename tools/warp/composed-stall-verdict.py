#!/usr/bin/env python3
"""The composed-stall gate's verdict over a composed-screen transcript.

GPU-DESIGN 4.5.15: a GPU stall says nothing about the compositor's own
context, so neither a bring-up step nor a composition health step that meets
one takes a verdict: the step runs again, or the next copy decides.
`warp-host.sh composed-stall` boots the GL leg of composed-screen with
test-latch-dead armed, which aims three stall shots -- at the compositor ctx
create, at one blit-convention attempt, and at a health step once GPU
composition runs; this reads what it printed.

Why the transcript and not the scenario: the chain lands while the battery's
probe stages run, and an expect waiting for it would discard probe lines the
later stages need. The transcript has every byte the guest wrote.

Each bring-up leg asks: aimed, injected, the step's retry, its verification;
the conventions line names no CPU class, a failure even where the chain
broke first. The health-step leg follows.
Its order is only the causal one. tapestryd aims the shot (B) before
the step whose reply carries it, lictor says the injection (C) before that
reply, and tapestryd reads the failure (D) after it. lictor's revival (E)
comes from the pump that ends the same request, so it precedes D today; the
check asks each only after C, which a revival at a later pump also meets.
The verification (F) needs a fresh issue and read on a live engine, so it
follows both. Exit 0 = VERIFIED.
"""
import re
import sys

ARMED = "tapestryd: test-latch-dead armed"
BRINGUP = [
    ("the compositor ctx create",
     "tapestryd: test-latch-dead: the compositor ctx create retires past its deadline",
     "tapestryd: compositor ctx create met a GPU stall -- no verdict, retrying",
     "tapestryd: compositor ctx create verified after a GPU stall"),
    ("a blit-convention attempt",
     "tapestryd: test-latch-dead: the next blit-convention attempt retires past its deadline",
     "tapestryd: blit-convention attempt met a GPU stall -- no verdict, retrying",
     "tapestryd: blit-convention attempt verified after a GPU stall"),
]
CONVENTIONS = re.compile(r"tapestryd: composed pixels = GPU \(blit conv: ([^)]*)\)")
AIMED = "tapestryd: test-latch-dead: the next health step retires past its deadline"
INJECTED = ("lictor: gpu synchronous command retired past its deadline "
            "(INJECTED stall shot of test-latch-dead) -- engine stalled")
NO_VERDICT = "tapestryd: composition health step met a GPU stall -- no verdict, re-verifying"
REVIVED = re.compile(r"lictor: gpu synchronous chain retired (\d+) ms after its deadline -- engine revived")
VERIFIED = "tapestryd: compositor ctx verified after a GPU stall -- GPU composition stays ON"
SCENARIO = "LS-CI PASS: composed-screen: virtio-gpu-gl-pci"
FORBIDDEN = [
    ("GPU composition OFF, sticky", "the compositor latched GPU composition off"),
    ("stall REFUSED by the broker", "the broker refused a stall shot"),
    ("met a GPU stall on every try", "a bring-up step gave up under stalls"),
    ("compositor ctx create failed", "the compositor ctx was never made"),
    ("blit conventions not established", "the blit conventions were never measured"),
    ("test-latch-dead arm REFUSED", "the broker refused the lever"),
    ("stall shot LOST", "the shot left its command unfired (refused, or a real stall first)"),
]


def clean(text):
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
    return text.replace("\r", "")


def verdict(text):
    """Return (ok, lines): lines are the gate's report, failures first."""
    fails = []
    for needle, why in FORBIDDEN:
        if needle in text:
            fails.append(f"{why} ('{needle}')")
    if SCENARIO not in text:
        fails.append("the GL leg did not run to completion (no scenario PASS)")

    def at(needle, after=-1):
        return text.find(needle, after + 1)

    chain = []
    pos = at(ARMED)
    chain.append(("armed", pos))
    for what, aimed, retry, ok in BRINGUP:
        for name, needle in ((f"{what}: aimed", aimed), (f"{what}: injected", INJECTED),
                             (f"{what}: no verdict, retried", retry), (f"{what}: verified", ok)):
            pos = at(needle, pos) if pos >= 0 else -1
            chain.append((name, pos))
    # A CPU class is a failure on its own, whether or not the chain before it held.
    settled = CONVENTIONS.search(text, max(at(ARMED), 0))
    if settled and "CPU" in settled.group(1):
        fails.append(f"a blit class fell to the CPU after the stall ({settled.group(1)})")
    conv = CONVENTIONS.search(text, pos + 1) if pos >= 0 else None
    pos = conv.start() if conv else -1
    chain.append(("the conventions measured after both", pos))
    a = pos
    b = at(AIMED, a) if a >= 0 else -1
    c = at(INJECTED, b) if b >= 0 else -1
    d = at(NO_VERDICT, c) if c >= 0 else -1
    m = REVIVED.search(text, c + 1) if c >= 0 else None
    e = m.start() if m else -1
    f = at(VERIFIED, max(d, e)) if d >= 0 and e >= 0 else -1
    chain += [("health step: aimed", b), ("injected after aimed", c),
              ("no verdict after the injection", d), ("revived after the injection", e),
              ("verified after both", f)]
    # The first missing link names which half of the chain went wrong.
    for name, pos in chain:
        if pos < 0:
            note = ""
            if name == "verified after both" and VERIFIED in text:
                note = " (a verification printed, but not after both)"
            fails.append(f"chain broken at '{name}'{note}")
            break
    if fails:
        return False, ["COMPOSED-STALL GATE FAIL -- " + x for x in fails]
    revived_ms = m.group(1)
    return True, [f"COMPOSED-STALL GATE: VERIFIED (ctx create and a blit-convention attempt stalled "
                  f"by injection and retried, conventions {conv.group(1)}; health step stalled, no verdict, "
                  f"engine revived {revived_ms} ms past its deadline, compositor ctx re-verified, "
                  f"GPU composition never latched; 9/9 pixels via the 3D screen)"]


def main(argv):
    if len(argv) != 2:
        print("usage: composed-stall-verdict.py <composed-screen transcript>", file=sys.stderr)
        return 2
    with open(argv[1], "rb") as fh:
        text = clean(fh.read().decode("utf-8", "replace"))
    ok, lines = verdict(text)
    for line in lines:
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
