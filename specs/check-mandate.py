#!/usr/bin/env python3
"""Run the finite mandate model and named mutants, retaining every TLC log.

Requires the host's resource lease. One worker and a bounded heap are deliberate.
Parse errors, timeouts and a violation of the wrong invariant are all failures.
Copies inputs into the evidence directory so TLC trace files stay out of specs/.
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jar", default=os.environ.get("TLA_JAR", "/tmp/tla2tools.jar"))
    parser.add_argument("--java", default=shutil.which("java") or "java")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = args.output or source.parent / "work" / "ua-model" / stamp
    output.mkdir(parents=True, exist_ok=False)
    for path in [source / "mandate.tla", *source.glob("mandate*.cfg")]:
        shutil.copy2(path, output / path.name)
    cases = [
        ("mandate", None),
        ("mandate_buggy_stale_support", "Supported"),
        ("mandate_buggy_fork_admission", "RevocationComplete"),
        ("mandate_buggy_no_cascade", "Supported"),
        ("mandate_buggy_preview_change", "Bounded"),
        ("mandate_buggy_no_restore", "RestorationBeforeCommit"),
        ("mandate_buggy_replay", "NoReplayReopen"),
        ("mandate_buggy_no_audit", "AtomicAudit"),
    ]
    failed = False
    verdicts = []
    for name, invariant in cases:
        command = [args.java, "-Xmx512m", "-cp", str(Path(args.jar).resolve()),
                   "tlc2.TLC", "-workers", "1", "-metadir", name + ".states",
                   "-config", name + ".cfg", "mandate.tla"]
        with (output / (name + ".log")).open("w") as log:
            try:
                result = subprocess.run(command, cwd=output, stdout=log,
                                        stderr=subprocess.STDOUT, timeout=args.timeout)
                code = result.returncode
            except (subprocess.TimeoutExpired, OSError) as error:
                log.write("\nRUNNER ERROR: " + str(error) + "\n")
                code = -1
        content = (output / (name + ".log")).read_text()
        if invariant is None:
            good = code == 0 and "Model checking completed. No error has been found." in content
        else:
            good = code == 12 and f"Invariant {invariant} is violated." in content
        states = re.findall(r"([\d,]+) distinct states found", content)
        verdict = f"{'PASS' if good else 'FAIL'} {name}: rc={code}, states={states[-1] if states else '?'}"
        if invariant:
            verdict += f", expected={invariant}"
        print(verdict, flush=True)
        verdicts.append(verdict)
        failed |= not good
    (output / "verdicts.txt").write_text("\n".join(verdicts) + "\n")
    print(f"Evidence: {output}")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
