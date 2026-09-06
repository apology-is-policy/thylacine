# 75 — devcap: the hostowner-elevation `cap` device [ABSORBED INTO THE VAULT]

Absorbed at the docs/reference retirement (`chg-2026-09-06-devcap-doc-absorb`).
The kernel-side factotum-pattern hostowner elevation: a two-phase, file-mediated
capability grant through the `/cap` device (`/cap/grant` + `/cap/use`), binding
the grant to the unforgeable per-Proc `stripes` identity. Its content lives,
code-verified and current, in:

- the **whole cap device** — the two-phase grant gated on *different* authorities
  (`/grant` on `CAP_GRANT_HOSTOWNER` or clearance; `/use` on
  `PROC_FLAG_CONSOLE_ATTACHED` + a matching non-expired pending grant), the
  pending-grant table + 30 s expiry + lazy sweep + re-register-in-place, the
  equality-not-subset redeem rule, the fail-closed no-consume-on-mismatch, the
  one-shot consume, the `stripes` binding, and `cap_proc_exit_notify` cleanup:

      vault/system/kernel/security/sub-kernel-caps.md   (audit: hard, inv-i2/inv-i25)
      "Capabilities — the fork-grantable ceiling, the cap device, and the legate"

- the **enforcement axis** the elevated `CAP_HOSTOWNER` feeds:

      vault/system/kernel/security/sub-kernel-perm.md

**What this file got WRONG or MISSED by the time it was absorbed:**

- **Nothing load-bearing — a clean zero-fold.** The cap-device mechanism, the
  two-trust-domain defense-in-depth (a compromised corvus can register grants for
  arbitrary stripes but only a *console-attached* writer can redeem, so a corvus
  compromise is bounded to the local physical console — `corvus.tla`'s
  `HostownerRequiresConsole`), and every subtle rule (equality-not-subset, no
  consume on a failed gate, one-shot) are all carried by `sub-kernel-caps`, which
  laps this doc (it also carries the legate and the clearance grant this device
  generalizes to). The `seam-devcap-plain-caps-read` (the register gates still read
  the plain caps word) is tracked there.
- **The doc's own "pending fixup commit" / P5-hostowner status framing is stale**
  — the userspace consumer (corvus `ADMIN_ELEVATE` + joey redemption) landed; the
  live picture is in `sub-corvus` and `sub-kernel-caps`.
