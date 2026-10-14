---
id: dec-2026-10-14-b3-test-bar
type: dec
title: "B-3's 'own tests' bar is each library's upstream test programs, not its interpreter-driven suites"
date: 2026-10-14
status: standing
decided-by: user-vote
affects: [sub-substrate-build]
created: 2026-10-14
---
## Fork

B-3's exit criterion reads "each library's own tests under Pouch". Several
upstream harnesses need an interpreter Thylacine does not ship: SQLite's
main suite (testfixture) needs Tcl; OpenSSL's `make test` and curl's
`runtests.pl` need Perl; HarfBuzz's API tests need glib.

## Research

- Pouch has no precedent for an upstream suite on the device: libsodium was
  proven by a hand-written KAT prover, JSC by `ls-jsc` legs, ICU was built
  `--disable-tests` (survey, main's memory note design-b3-survey).
- Every library ships compiled test programs that run without an
  interpreter: SQLite fuzzcheck, mptest, speedtest1; OpenSSL's C test
  binaries with their data files (evp_test); libpng pngtest; libjpeg-turbo
  tjunittest; libxml2 runtest, testapi, testchar; HarfBuzz hb-shape over
  test/shape; ICU cintltst and intltest.

## Options

- **The compiled test programs** on the device with their data, each named in
  a census line joey checks; each interpreter-driven suite skipped is listed
  by name in the status doc.
- **The same, plus a Tcl port** for SQLite's full suite.
- **Tcl and Perl ports**, every suite as upstream runs it.

## The call

The compiled test programs (operator vote, 2026-10-14).

## Rationale

The compiled programs exercise each library's code on the device with
upstream's own vectors, and the census makes a skipped or stale leg visible.
An interpreter port is a project of its own that B-3 does not need to reach
WebKit; the skipped suites stay listed, so the gap is recorded rather than
hidden.
