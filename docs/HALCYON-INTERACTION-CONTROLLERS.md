# Controller lifecycle adapter

Implementation detail of the approved HI-1 ownership contract; no new wire,
kernel authority or admission point. The service executor owns a fixed table of
32 terminal controller entries. A pane token locates an already authenticated
host route and is never a credential. Each route includes its local incarnation,
leaf, binding and foreground epoch. Direct graphical ownership is a later adapter
to the existing surface contract, not a terminal-route shortcut.

Preparation requires a live kernel peer snapshot (connection incarnation,
process stripes and principal), the session principal, a host-supplied context
and epoch, and the current normal seat generation. Application-supplied values
must be compared with that host context before this API is called. An occupied
leaf or binding is refused until explicitly retired. Preparation burns a fresh
monotone controller generation even if its eventual publication fails.

The entry starts pending. Its HIA Publish request is sent through the existing
serialized admission channel using that channel owner's increasing request ID.
Only the exact successful Publish receipt, matching foreground and seat, plus
a fresh matching live kernel peer snapshot, makes it active. Wrong receipts do
not consume another pending publication; an exact failed publication retires
its entry. This table does not supply kernel peer snapshots or perform HIA I/O.
The caller must drain in-flight HIA work before channel reuse, even after a
publication has been cancelled locally. One request sequencer must cover all
Bind/Publish/Check/Unbind operations when the channel adapter is connected.

Mode reports require the complete scope and exact peer incarnation, a strictly
increasing nonzero sequence and at most 64 bytes of control-free UTF-8 context.
Labels are stored inline. Invalid/stale reports leave the last record intact.
Unknown or pending controllers have no reported mode; presentation derives APP
or its own transcript state rather than guessing INS/NOR. Presentation chooses
an exact host route; peer reports never choose the focused tile.

Disconnect, route removal/replacement, terminal foreground/subject retirement and seat
loss remove pending and active entries. Retirement yields the exact old Owner
to the broker cancellation callback before a replacement can register. A stale
route-removal event cannot retire a replacement incarnation. Terminal snapshots
must include both epoch and subject: ACK can change nomination at the same epoch.
An unacknowledged/dead nomination supplies subject zero. Seat restoration
starts empty, never revives old scopes, and preserves the generation counter.
Focus-only loss is different: preserve the registration/mode and use the broker's
ordered focus-loss epoch handling; do not treat a return to focus as registration.

The table is allocation-free with a compile-time 16 KiB metadata ceiling.
That bound is only this table, not a claim about the total session allocation.
Public dispatch remains disabled until authenticated route/context delivery,
ordered focus and terminal retirement, request serialization, pending/partial
reply cancellation, clients and the complete activation ledger are connected
and exercised together.
