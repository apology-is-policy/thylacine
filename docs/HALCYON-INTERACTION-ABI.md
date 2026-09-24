# Halcyon interaction wire reservations

HI-1 envelope foundation for the approved HALCYON-INTERACTION design. This
reserves identifiers; it does not enable a clipboard endpoint. Operation body
layouts, ownership/focus integration, aggregate allocation accounting and the
asynchronous C/Rust client API must land before any client consumes the service.

Rust definitions: `usr/lib/libhalcyon/src/interaction_wire.rs`. C mirror:
`usr/lib/libhalcyon/include/halcyon_interaction.h`. All integers are explicitly
little-endian. No pointer-sized fields, native struct casts or implicit padding.

| Byte offset | Field | Width |
|---:|---|---:|
| 0 | Magic `HIN1` | 4 |
| 4 | Version, exactly 1 | 2 |
| 6 | Operation | 2 |
| 8 | Exact total record length, including this header | 4 |
| 12 | Flags: bit 0 response; every other bit zero | 4 |
| 16 | Nonzero request ID, echoed unchanged in response | 8 |
| 24 | Operation body | variable |

Maximum record length is 32768 bytes; minimum 24. Unknown version, operation,
flags, direction mismatch or any trailing bytes are refused. An envelope parser
does not accept an operation body implicitly: the typed operation decoder must
require exact exhaustion before dispatch. Transport fragments have contiguous
offsets starting at zero and produce no operation before complete validation.
Request IDs increase on a fid; only an exact immediate duplicate may return its
cached result. Changed bytes with the same ID are a protocol error. A client
opens a fresh fid before exhaustion; reconnect is not a mutation retry.

Operation numbers 1..10, respectively: Hello, BindController, ReportMode,
GetClipboard, ReadClipboard, BeginCopy, WriteCopy, CommitCopy, Cancel,
UnbindController. These are local to HIN1, not 9P message IDs or Lictor opcodes.
Modes are INS=1, NOR=2, VIS=3, CMD=4, APP=5; readonly remains a separate flag.
The matching Hello envelope fixture carries request ID 0x0102030405060708.

Limits: 1 MiB canonical clipboard UTF-8, 16 KiB transfer chunks, 64-byte context
labels, 4096-byte search queries. Clipboard validation rejects Unicode control
characters except TAB/LF; export adapters normalize CRLF/CR before submission.
Unicode format characters and combining sequences are preserved. No terminal
escape sequence or NUL may enter the clipboard through a canonical upload.

The existing pane service uses qid class low bits 0=root, 1=directory, 2=place.
Class 3 is reserved for interaction in that service. Routing still uses the full
pane token and live ownership records; the folded qid is never authority. No new
srv slot, 9P message type, kernel syscall or kernel capability is reserved here.

The operation-body/error mapping and C/Rust fixture gate remain prerequisites
for service integration. The current source tests exercise only envelope bounds,
canonical text and malformed input; they are not end-to-end clipboard evidence.
