# Imperium and secure attention

`imperium` opens an elevated shell after the operator authorizes a specific
set of capabilities. The authorization applies to that shell and its descendants
and ends on abdication, exit, or expiry. It requires an eligible account with an
enrolled Imperium key. That key is distinct from the ordinary login password.

In Halcyon, secure attention temporarily takes over the display and keyboard.
The Lex curiata dialog shows the requesting identity, process, capabilities and
term. Returning from the dialog restores the workspace and its existing panes.

## In Practice

### Authorize a restricted shell

From an interactive Halcyon shell, request only the capabilities needed:

```sh
imperium chown dac
```

The command prints its process ID. Press Ctrl–Alt–Delete, or Ctrl–Alt–F10 on a
keyboard without a Delete key, then release the keys.
Check that the dialog shows the same process and the intended identity and
capabilities. Enter the Imperium key in that dialog and press Enter. The field
shows masked characters; it does not send the key into the terminal transcript.
Escape cancels the request.

After a successful verdict, press a key to return. The elevated shell opens in
the original terminal, in the directory where you ran `imperium`. Inspect its
authority with:

```sh
imperium --list
```

The prompt displays fasces rods while elevated. An axe cue indicates that the
scope includes authority to signal processes across identity boundaries. These
cues describe the shell's authority; the physical secure-attention gesture is
what selects the authorization channel. A copied picture of the dialog is not
proof of that channel.

The supported request operands are:

| Operand | Capability | Effect |
| --- | --- | --- |
| `dac` | `CAP_DAC_OVERRIDE` | Bypass filesystem permission checks. |
| `chown` | `CAP_CHOWN` | Change file ownership. |
| `kill` | `CAP_KILL` | Send signals across identity boundaries. |
| `post` | `CAP_POST_SERVICE` | Publish services for other processes. |

With no operands, the tool requests the full Imperium level. Naming operands
restricts the request to that subset. The dialog shows the actual grant and its
term; eligibility alone does not confer authority. Nested Imperium shells are
refused.

### Relinquish authority

Run `abdicate` or `exit` inside the elevated shell. The original shell resumes
without the elevated capabilities. Descendants in the elevated scope are torn
down as part of revocation, including background jobs. Move ordinary long-lived
work outside that scope before authorizing administrative work.

### Cancel or recover

Ctrl–Alt–Delete without a waiting request shows an informational panel. Press a
key to return. A wrong key, cancellation, expired request, changed requester, or
revoked eligibility does not confer authority. Repeated wrong keys cause a
lockout.

If the trusted display cannot complete an episode, the dialog reports that no
authority was conferred, and the workspace returns once every key has been
released. Holding the attention keys for more than five seconds is the usual
cause. The pending request is cancelled and must be issued again. The key prompt
is never moved into an ordinary terminal.

The current QEMU backend uses a neutral dark background during authorization.
The workspace is suspended from display and input while its panes remain alive.
A blurred workspace snapshot is not yet provided by this backend.

### Use a serial recovery session

Serial authorization is available only when the boot configuration includes
`thylacine.serial-sak=1`. In a QEMU serial recovery session, Ctrl–A followed by B
sends BREAK, the serial secure-attention gesture. The development launcher
`tools/run-vm.sh` enables this recovery posture by default; set
`THYLACINE_SERIAL_SAK=0` to test graphical-only operation. Boot firmware that
omits the token leaves serial authorization disabled.

## Technical Details

Corvus checks eligibility and the distinct authorization key. Lictor owns the
physical input and display transport and paints the trusted dialog using baked
fonts. The kernel binds both service instances to a numbered episode. Tapestry,
Halcyon, Beacon and application surfaces cannot submit trusted content or read
the trusted framebuffer through their normal graphics interfaces.

Before the dialog accepts key input, Lictor drains previously admitted graphics
work, excludes ordinary outputs, selects private backing and acknowledges that
the current request is visible. Release events are drained at entry and exit so
an Enter or Escape used by the dialog does not activate a workspace command.

A verified key creates a held grant. The kernel permits redemption only after
Lictor acknowledges successful display restoration. Failure before that point
cancels the grant. Redemption creates the revocable process scope; its existing
lifetime rules then control authority and inheritance.

This backend is qualified separately from future display and input hardware.
The current graphical implementation targets QEMU virtio devices. Raspberry Pi
400 and Pi 500 require their own hardware ownership, DMA and output qualification.
