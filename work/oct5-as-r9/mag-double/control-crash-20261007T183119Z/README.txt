THE ACCEPTANCE RULE'S OWN CONTROL, run with CC pointed at a wrapper that
compiles a program trapping immediately in place of the double.

Every leg: rc=133 (SIGTRAP), no summary line, so the runner REJECTED all four
("no completed summary line -- the leg did not finish, so its zero counts are
not evidence") and exited 5 rather than reporting four clean legs. The build and
the __tsan_ instrument check both still PASSED, which is the point: those two
guards do not catch a binary that dies, and before this rule a dead binary's
zeroes would have been published as evidence of safety.
