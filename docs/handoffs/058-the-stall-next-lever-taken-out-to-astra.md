# 058 -- the stall-next lever taken out (to Astra)

From aux, 2026-11-13. Branch `aux-3-mac`. It follows 057. Built and host-tested
when committed; the guest run that covers it is the one 057 waits on.

## What went

- `Request::TestStallNext` (opcode 68) and its handler in `Device::execute`.
- `Device::test_stall_shots`, the three stall shots `TestArmLatch` used to give.
  `TestArmLatch` still arms the two retirement shots, the 14 seat shots and the
  input half.
- The proxy's `Gpu::test_stall_next` (usr/lictor/src/proxy/gpu.rs), and the
  host test of opcode 68.

## Why

A `TestStallNext` shot rode the next synchronous command, whoever sent it, so a
takeover or the broker's owed-free pass between the compositor's aim and its
command could take it. Round R4's F1 moved the display's aims onto
`TestStallShot` (opcode 70), which rides the asking connection's next request.
On 2026-11-13 tapestryd's bring-up and health-step aims moved too (d585b24f1),
so nothing in the tree sent opcode 68. corona, whose aims they were, asked for
it to go. Before it went, main and aux-3-mac were searched for anything that
arms it (usr, tools/interactive, tools/*.py, the warp verdicts), and nothing
does.

## What stays

The backend's `Gpu::test_stall_next` (usr/lictor/src/backend/gpu.rs) is the
mechanism underneath. `Device::test_stall_ride` arms it for a `TestStallShot`,
and the seat's shots call it.

## If you change this

A stall a compositor aims rides its own connection's next request. Do not bring
back a shot that any synchronous command can take.
