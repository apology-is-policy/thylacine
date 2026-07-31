---
id: fnd-16c-r1-f8
type: fnd
title: "Teardown-migration path untested"
round: adt-16c-r1
severity: P3
status: fixed
surface: [sub-kernel-ninep-transport]
threatens: []
fixed-by: chg-2026-05-26-16c-attach-srv
regression: 9p_srvconn_transport.kernel_attached_skips_teardown_on_handle_close
created: 2026-07-31
---
## Prosecution

Nothing exercised the adapter's transport.close actually running the
migrated srvconn_teardown -- a regression dropping it would pass.

## Disposition

Fixed: `kernel_attached_skips_teardown_on_handle_close` extended with a
Part 3 driving the adapter's close and verifying the teardown fires.
