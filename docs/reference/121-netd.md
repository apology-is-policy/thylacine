# 121 — netd: the network daemon [ABSORBED INTO THE VAULT]

This document was absorbed at the netd sweep
(`chg-2026-07-31-netd-sweep`). Its content now lives, code-verified and
current, in the dossiers:

    vault/system/userspace/services/sub-netd-nic.md
    vault/system/userspace/services/sub-netd-server.md

(the warden bind + probe gate, the phy tokens, DHCP bring-up + the
poll_dhcp re-apply, the serve-loop delivery choreography + the #221
poll-cadence bands; the qid-encoded /net tree, the refcounted slot pool
+ the mint-generation guard, the five deferred-reply engines + their
four-site cancel matrix, the #293 remove-not-abort connect sweep, the
net-8a dual-stack routing, cs/dns/ndb/ipifc/summary, the weft in-place
drive, the #52 nonblock arm — plus what this file was thin on: the full
weft TX/RX drive mechanism, and the stale bring-up DHCP comment the
re-apply pass superseded). The audit history (net-2d, net-3d r1/r2,
net-4d r1/r2, net-8d, weft-7) lives as adt-/fnd- Record notes; the open
debt as seam-220-netd-listener-poll / seam-56-netd-cancelled-tag /
seam-240-lo-redial / seam-242-selftest-nonfatal / seam-netd-host-tests;
the do-not-re-report preambles are
vault/views/view-closed-sub-netd-{server,nic}.md.

The dossiers supersede this file. Do not extend this stub -- extend the
dossiers and their linked registry notes. This stub is deleted after the
vault migration's full-corpus verification pass (vault/meta/schema.md
section 10.6).
