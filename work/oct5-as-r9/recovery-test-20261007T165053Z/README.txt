Retained evidence for the recovery harness, written because a tally in prose is
a claim and a RESULTS.txt is evidence (astra, yip 0161 t57: she read the arms
read-only but could not verify the reported 24/0 and the three mutant tallies).

  MANIFEST.txt                  the driver's run -- control plus three
                                one-variable mutants, with each mutant's
                                EXPECTED ARM SET asserted rather than only its
                                count, and a VERDICT line at the end.
  control/RESULTS.txt           every check of the unmutated runner: 24/0.
  m1,m2,m3/RESULTS.txt          the mutants: a rebuild mismatch that only
                                prints; the disk floor never re-measured before
                                the recovery rebuild; a restore failure that
                                only prints.
  *.stdout, *.reds              each run's full output, and the red lines the
                                driver matched against its expectation.
  DRIVER-CONTROL-MANIFEST.txt   THE CHECKER'S OWN CONTROL. The driver was re-run
                                with m1's expectation deliberately mis-specified
                                as R7; it names the unexpected red (R3) and
                                fails with VERDICT: FAILED. Without this,
                                "each mutant reddens only its named arm" would
                                rest on a checker that had never been wrong.
                                Its first attempt was itself invalid -- a copy
                                of the driver run from outside the tree died
                                before testing anything and still exited 1,
                                which is why the log is read and not the status.

NOT RETAINED, deliberately: the per-scenario scratch trees (the stub build.sh
and yip, the fake build/ and kernel/ dirs) and the generated mutant scripts.
Every run regenerates them, and each RESULTS.txt records the sha256 of the exact
script it tested, so a reader reproduces rather than trusts:

    sh work/oct5-as-r9/recovery-test-mutants.sh

WHAT THIS EVIDENCE DOES NOT COVER: it is the RECOVERY half only, against a stub
tree, which is honest for control flow and worthless for layout -- the layout
premise is asked of the real build tree in the harness's L1 arm. The experiment
half (control + mutant boots) remains UNRUN and needs the mac.
