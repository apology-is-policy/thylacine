#!/usr/bin/env python3
"""The Warp scenarios' image gate, on the host.

A warp scenario booted on the wrong image fails at the end of its budget as
something else: a Halcyon-session pool reached login and timed out at "shell
never came up", a clade-less one at "no composed-entry line within 900s". Four
guards stop that, and each runs here with its acceptance one variable away:

  - lc_require_image (tools/interactive/lib.exp) refuses, before the boot, a
    pool whose build/pool-contents record lacks what the scenario needs, from
    a byte-identical copy of lib.exp in a scratch tree;
  - lc_login fails at once on login's session-compositor line, against a fake
    console that plays login's prompts;
  - warp-host.sh lint refuses a logging-in scenario that declares no image,
    from a scratch copy of the script and the scenarios;
  - every scenario that launches through the tyr-glquake launcher fails on its
    not-reachable line, matched against the launcher's own source.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "tools/interactive/lib.exp"
WARP = ROOT / "tools/warp"
LAUNCHER = ROOT / "usr/ports/tyrquake/tyr-glquake-launcher.c"

fails = 0


def check(name, ok, detail=""):
    global fails
    print(("  ok   " if ok else "  FAIL ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        fails += 1


def expect(script, timeout=60):
    p = subprocess.run(["expect", "-c", script], capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def require_image(scratch):
    print("== lc_require_image: the pool's record against the scenario's needs ==")
    lib = scratch / "tools/interactive/lib.exp"
    lib.parent.mkdir(parents=True)
    shutil.copyfile(LIB, lib)
    (scratch / "build").mkdir()
    rec = scratch / "build/pool-contents"
    full = "clade=1\ngoroot=1\npayloads=GOROOT GOCACHE GO4C CLADE GLQUAKE STORM QUAKE\n"
    cases = [
        # (name, needs, record or None, refused, what the output must name)
        ("no record refuses", "shell CLADE QUAKE", None, True, "no bake record"),
        ("a record with no payloads line refuses", "shell", "clade=1\ngoroot=1\n", True, "no payloads= line"),
        ("a session pool refuses a shell scenario", "shell CLADE QUAKE",
         full.replace("QUAKE\n", "QUAKE SESSION\n"), True, "--config ci"),
        ("a clade-less pool refuses", "shell CLADE QUAKE", full.replace(" CLADE", ""), True,
         "THYLACINE_BAKE_CLADE=1"),
        ("a quake-less pool refuses", "shell CLADE QUAKE", full.replace(" QUAKE", ""), True,
         "build/quake/stage"),
        ("a clade pool staged without tyr-glquake refuses", "shell GLQUAKE QUAKE",
         full.replace(" GLQUAKE", ""), True, "build/clade/gl/tyr-glquake"),
        ("a label matches whole, never as a prefix", "shell CLADE", full.replace(" CLADE", " CLADEX"), True,
         "no CLADE payload"),
        ("a pool holding every need is accepted", "shell CLADE QUAKE", full, False, "IMAGE-OK"),
        ("an empty payloads line satisfies shell alone", "shell", "payloads=\n", False, "IMAGE-OK"),
        ("SESSION refuses only a shell need", "CLADE", full.replace("QUAKE\n", "QUAKE SESSION\n"), False,
         "IMAGE-OK"),
    ]
    for name, needs, body, refused, token in cases:
        if body is None:
            rec.unlink(missing_ok=True)
        else:
            rec.write_text(body)
        rc, out = expect(f'source {{{lib}}}; lc_require_image {{{needs}}}; puts IMAGE-OK')
        if refused:
            check(name, rc == 1 and "LS-CI FAIL: image refused:" in out and token in out
                  and "IMAGE-OK" not in out, f"rc={rc} out={out.strip()[-200:]}")
        else:
            check(name, rc == 0 and token in out and "refused" not in out, f"rc={rc} out={out.strip()[-200:]}")


def login_arm(scratch):
    print("== lc_login: login's session-compositor line fails at once ==")
    lib = scratch / "tools/interactive/lib.exp"
    for name, after_auth, ok_token, bad_token in [
        ("a session image fails naming the lever",
         r"\033]7770;aurora;reset;systemlogin: session compositor (halcyond) spawned\n",
         "LS-CI FAIL: this image boots the Halcyon session", "LOGIN-OK"),
        ("a shell image logs in (control)", r"ut v0.9-dev -- Thylacine textual shell\n",
         "LOGIN-OK", "LS-CI FAIL"),
    ]:
        fake = scratch / "console.sh"
        fake.write_text("printf 'Thylacine login: '; read u; printf 'password: '; read p\n"
                        f"printf '{after_auth}'; sleep 30\n")
        # The boot budget is the session failure's only other exit; a pass
        # through the arm must beat it by far.
        t = f'source {{{lib}}}; set ::lc_boot_timeout 20; spawn sh {{{fake}}}; ' \
            'set t0 [clock seconds]; lc_login michael pw; puts "LOGIN-OK [expr {[clock seconds] - $t0}]s"'
        rc, out = expect(t)
        quick = "never came up" not in out
        check(name, ok_token in out and bad_token not in out and quick, f"rc={rc} out={out.strip()[-240:]}")


def lint(scratch):
    print("== warp-host.sh lint: a logging-in scenario must declare its image ==")
    tree = scratch / "lint"
    (tree / "tools/warp").mkdir(parents=True)
    shutil.copyfile(ROOT / "tools/warp-host.sh", tree / "tools/warp-host.sh")
    exps = sorted(WARP.glob("*.exp"))
    logging_in = [e for e in exps if "lc_login" in e.read_text()]
    check("the scenarios that log in were found", len(logging_in) >= 10, f"{len(logging_in)}")
    for e in exps:
        shutil.copyfile(e, tree / "tools/warp" / e.name)

    def run():
        p = subprocess.run(["bash", str(tree / "tools/warp-host.sh"), "lint"], capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr

    rc, out = run()
    check("the tree's scenarios pass", rc == 0, out.strip())
    victim = tree / "tools/warp" / logging_in[0].name
    good = victim.read_text()
    decl = re.search(r"(?m)^lc_require_image \{.*\}\n", good).group(0)
    after_boot = re.sub(r"(?m)^(\s*lc_boot\b.*\n)", lambda m: m.group(1) + decl, good.replace(decl, ""), count=1)
    for name, mutant, refused in [
        ("a scenario with no declaration is named", good.replace(decl, ""), True),
        ("a declaration without shell is named", good.replace(decl, re.sub(r"\{shell ?", "{", decl)), True),
        ("a declaration after lc_boot is named", after_boot, True),
        ("shell anywhere in the braces is accepted", good.replace(decl, decl.replace("{shell", "{CLADE shell", 1)), False),
    ]:
        check(name + " (mutant differs)", mutant != good)
        victim.write_text(mutant)
        rc, out = run()
        if refused:
            check(name, rc == 2 and f"tools/warp/{victim.name}" in out, f"rc={rc} out={out.strip()}")
        else:
            check(name, rc == 0, f"rc={rc} out={out.strip()}")
        victim.write_text(good)


def launcher_arm():
    print("== the launcher's not-reachable line ends every scenario that launches through it ==")
    src = LAUNCHER.read_text()
    target = re.search(r'#define TARGET "([^"]+)"', src).group(1)
    m = re.search(r'"tyr-glquake: " TARGET " (not reachable[^"]*?)\\n"', src)
    check("the launcher's message was found", m is not None)
    if m is None:
        return
    message = f"tyr-glquake: {target} {m.group(1)}"
    # Through the launcher = the bare name or /bin/tyr-glquake, never /clade/bin,
    # in ANY double-quoted string outside a comment: a launch can travel through
    # a helper (glq-bench's run_demo) as well as an lc_send literal.
    launch = re.compile(r'"[^"\n]*(?<![\w/])(?:/bin/)?tyr-glquake ')

    def launches(text):
        return any(launch.search(l) for l in text.splitlines() if not l.lstrip().startswith("#"))

    users = [e for e in sorted(WARP.glob("*.exp")) if launches(e.read_text())]
    check("the launching scenarios were found", len(users) >= 3, ", ".join(u.name for u in users))

    def armed(text):
        arms = re.findall(r'-ex "([^"]*not reachable[^"]*)" \{ lc_fail', text)
        return bool(arms) and all(a in message for a in arms)

    for u in users:
        check(f"{u.name} fails on '{message}'", armed(u.read_text()))
    check("a launch through a helper's argument is seen (mutant)",
          launches('run_demo x "./rp5 /bin/tyr-glquake -window +timedemo demo1"'))
    check("a /clade/bin launch is not a launcher use (control)",
          not launches('run_demo x "./rp5 /clade/bin/tyr-glquake -window"'))
    if users:
        text = users[0].read_text()
        check("a scenario without the arm is caught (mutant)",
              not armed(re.sub(r'(?m)^ *-ex "[^"]*not reachable[^\n]*\n', "", text)))
        check("an arm matching words the launcher never prints is caught (mutant)",
              not armed(text.replace("is the clade pool mounted?", "is /clade mounted?")))


def main():
    with tempfile.TemporaryDirectory() as d:
        scratch = Path(d)
        require_image(scratch)
        login_arm(scratch)
        lint(scratch)
    launcher_arm()
    print("ALL PASS" if fails == 0 else f"{fails} FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
