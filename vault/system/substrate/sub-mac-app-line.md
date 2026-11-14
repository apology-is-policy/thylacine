---
id: sub-mac-app-line
type: sub
title: "The Mac app's end of the line -- the mac1 line and the 9P2000.L server over snarf and the shared folders"
parent: moc-substrate
code:
  - macrunner/app/src/interop/mod.rs
  - macrunner/app/src/interop/line.rs
  - macrunner/app/src/interop/tree.rs
  - macrunner/app/src/interop/tree_tests.rs
  - macrunner/app/src/interop/folder.rs
  - macrunner/app/src/interop/locks.rs
  - macrunner/app/src/interop/errno.rs
  - macrunner/app/src/interop/snarf.rs
  - macrunner/app/src/interop/log.rs
audit: hard
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: ["docs/MAC-RUNNER.md"]
created: 2026-11-14
updated: 2026-11-14
---
## Purpose

The Mac's end of the line to the Mac (MAC-RUNNER 4.4, votes M5 and M10-M13). The guest's
virtio-serial driver relays each connection to `/srv/mac` as a channel of the port
`org.thylacine.mac` ([[sub-mac-line]]). The app connects to QEMU's socket behind that port, and on
every channel it serves one 9P2000.L session over the tree version 1:

```
/            snarf, folders
snarf        the Mac pasteboard's text
folders/     one directory per folder the person shares, each that host folder's tree
```

Here the guest is the untrusted party. The server decides what a guest program can reach on the
Mac. The answer is the pasteboard's text and the shared folders, beneath their roots, as the Mac
user, and nothing else.

## Contract

- **The line.** `serve(Config, pasteboard, log)` connects to `Config.socket`, trying again every
  200 ms until QEMU listens, and again after each disconnect, as QEMU takes a new client then.
  With `once` it returns after the first connection. The framing is `macline`'s, the crate the
  guest's driver builds, with `End::App`:
  - frames before the driver's HELLO are discarded;
  - the app's HELLO (maxframe 65536) goes out before anything else;
  - a malformed frame or the driver's RESET ends the connection, and the app connects again.
- **Channels.** Each OPEN starts a session on its own thread, fed through a queue whose bytes are
  counted. A queue past 16 MiB closes its channel (the driver's rule for its own queues), so one
  channel never stalls another. A peer CLOSE ends the session once it has answered what it
  holds, and the session then sends the app's CLOSE. A stream that is not 9P (a size below 7 or
  past msize) closes that channel alone.
- **The session.** Tversion takes `9P2000.L` and an msize from 4096 to 1 MiB, and begins the
  session again. Tattach takes only the empty aname (else ENOENT) and an n_uname other than
  NONUNAME. Every file then reports that n_uname as its uid and gid.
- **snarf.**
  - A read at offset 0 takes the pasteboard's plain text, and later offsets continue that
    snapshot.
  - A fid opened O_WRONLY (with or without O_TRUNC) collects what is written. Its clunk sets the
    pasteboard: invalid UTF-8 becomes U+FFFD, and nothing written empties it.
  - O_RDWR is EACCES.
  - Past 16 MiB, a write answers EFBIG, and so does a read of a larger text.
  - Its stat is mode 0100600, size 0.
  - A Tversion or the channel's end discards an unclunked write and sets nothing.
- **folders/** lists the shares sorted by name. Each is that host folder's tree:
  - Walks, opens, creates and every name operation resolve beneath the share's root, one name
    per openat, with O_NOFOLLOW_ANY. No host link is followed, and no `..` or `/` is ever sent
    to the host.
  - A link is served as a link. Its qid type is QTSYMLINK, readlink reads it, a walk stops at it,
    and an open of it is ELOOP. The guest's kernel resolves it beneath the session's mount, since
    the driver posts `/srv/mac` remote (DISTRO 4.6, [[sub-mac-line]]).
  - Only a regular file or a directory opens. A fifo, socket or device answers EPERM: opening one
    would hand the guest the host's.
  - Tlcreate is exclusive: EEXIST proves the name exists, which the kernel relies on.
  - Tmkdir and Tsymlink are as Linux's.
  - Trename and Trenameat stay within one share (EXDEV across), and a share's root cannot move
    (EBUSY).
  - Tunlinkat of a directory without AT_REMOVEDIR is EISDIR. Tmknod and Tlink are EPERM.
  - A read-only share answers EROFS to every change: an open for writing or with O_TRUNC, a
    create, mkdir, symlink, rename, unlink, a setattr of mode, size or times, mknod and link.
- **Ownership** (4.4): uid and gid are the attacher's. The mode's owner bits are the host file's,
  without write in a read-only share, and group and other have none. A setattr that changes uid
  or gid is EPERM, and one that names the attacher is a no-op. Mode, size and times go to the
  host file.
- **Locks.** POSIX record locks are arbitrated among the guest's owners, across every session
  that locks the file. An owner is the session with the kernel's client_id and proc_id.
  - A conflict answers P9_LOCK_BLOCKED, and the kernel tries again for a waiting lock.
  - A fid's clunk lets go of the locks taken through it, and a session's end lets go of all its
    own.
- **Errors** are Linux errno values (errno.rs):
  - EPERM to ERANGE pass through, except that EDEADLK's 11 becomes 35. Other values are
    translated: ENOTEMPTY's 66 becomes 39, and so on.
  - A macOS value with no Linux counterpart reads EIO.
  - An operation this server does not have answers EOPNOTSUPP. That covers Txattrwalk, and the
    kernel's fused walk Twalkgetattr, on which the kernel falls back to a plain walk
    (kernel/dev9p.c).
- **The record** (`--line-log`): one line per event, in tools/mac-app-fake.py's words
  (`hello maxframe=N`, `chan N attach n_uname=U uname='' aname=''`,
  `chan N snarf set N bytes '...'`, `close chan N` and so on). `ls-mac-line` reads the app's
  record as it reads the fake's. Each walk is one line naming the node it began at, its names
  and its outcome: `chan N walk 'folders/work/d' ['secret.txt'] -> errno 40`, `-> 1 of 1`, or
  `walk no fid [] -> errno 9`. `ls-mac-folders` reads it to show a walk from a held directory
  began there.
- **Running it.**
  `thylacine-mac --mac-line <socket> [--share <name>=<path>[:ro]]... [--line-log <path>] [--snarf-memory <text>] [--line-once]`.
  - With `--attach`, the line runs beside the window, on its own thread. Without it, the line
    runs alone.
  - `--snarf-memory` gives a test a pasteboard in memory.

## Mechanism

`line.rs` reads the socket and parses frames (`macline::Parser`). It checks each frame against
the line's rules (`macline::Line`, behind the `wire` lock) and hands DATA to its channel's queue.
- Writers take the stream's own lock (`out`) first, and the `wire` lock briefly inside it, to
  check that the connection stands and that the channel may still carry DATA. A session's
  replies go out as whole DATA frames, never interleaved with another channel's.
- The reader takes only the `wire` lock.
- At a connection's end, `live` is cleared and the queues are dropped. A session still in a host
  call finds its writes refused, and its descriptors and locks go when it ends.

`tree.rs`'s `Session` holds the fid table and answers requests in order.
- A fid names a node: `Root`, `Snarf`, `Folders`, or `Host { share, rel }`. `rel` is the path
  below the share's root, each element checked by `folder::name_ok`.
- An opened fid holds a descriptor (a file or a directory), or snarf's snapshot or write buffer.
- A directory's listing is taken at the read from offset 0, and its cookies are indices into that
  listing. They stay true however the directory changes meanwhile.
- A rename in the session rewrites the paths of the session's fids below the old name.
- An open fid's getattr goes through its descriptor, so a rename since does not lose it.

`folder.rs` does every host call.
- `Root::dir` opens each path element in turn from the share's root, with
  `O_DIRECTORY | O_NOFOLLOW_ANY`. macOS refuses O_NOFOLLOW_ANY beside O_NOFOLLOW (EINVAL). The
  first build's walks below one level failed on it, and the tests caught it.
- Each name operation then acts at the parent's descriptor, with AT_SYMLINK_NOFOLLOW where the
  call takes it.
- The share's own path is opened once, following links, since the person chose it.

Qids:
- A host file's qid path is the share's index + 1 in bits 56-62 over the inode, when the file is
  on the share root's device and its inode fits. Otherwise it is a number from a remap table
  whose top byte is 0x7F. So at most 126 folders can be shared.
- The synthetic nodes' paths have bit 63 set.
- The qid version is QEMU's, mtime ^ (size << 8).

## Data structures

- `Shared` holds:
  - the shares sorted by name, each root opened at the first walk that finds the folder and then
    kept;
  - the pasteboard, the record, the lock table and the remap table;
  - the start time, which is the synthetic nodes' times.
- `Fid { node, uid, open }`. `Open` is `No`, `Dir { fd, list }`, `SnarfRead { snap }`,
  `SnarfWrite { buf }` or `File { fd, read, write, append }`.
- `Locks` maps (dev, ino), or (u64::MAX, a synthetic qid path), to a list of locks. Each lock
  records its owner, range and kind, and the fid it came through.

## Concurrency

- One thread reads the connection, and one thread per channel runs its session.
- The `out` lock is always taken before the `wire` lock, and the reader never holds `wire` while
  it writes.
- The lock table, the remap table and each share's root slot have their own mutexes. Each is held
  for a lookup and never across a host call.
- A session answers its requests in order, so a Tflush always comes after the request it names
  has been answered.

## Invariants enforced

No ARCHITECTURE 28 invariant applies: this code runs on the Mac. Its rules are MAC-RUNNER 4.4's:
- the guest reaches nothing on the Mac outside the shares' roots and the pasteboard's text;
- a read-only share is never changed;
- every file is the attacher's, and ownership never changes.

## Error paths

- Every handler returns an errno or a reply length. An errno becomes Rlerror with the request's
  tag.
- A parse failure is EPROTO.
- A pasteboard that refuses a set is recorded, and the clunk still succeeds.
- A failed write to the socket ends the connection, and the reader then sees the end.

## Performance

- A session answers its requests one at a time, so one slow host call (a stalled network volume)
  holds up the rest of that session's requests. Other sessions are not held up.
- Every path element is its own openat. Each step of a walk at depth d costs about d + 1 opens.

## Prosecution

Host tests (`cargo test interop` in macrunner/app), 30 of them:
- errno translation, the share argument, the record's quoting, and the lock table;
- the folder calls against real directories: a link inside or outside the share is a link and is
  never passed through, a share root that is a link is followed once, entries come without `.`
  and `..`, create is exclusive, and append writes at the end;
- the session, request by request, in the kernel's layouts:
  - the root and ownership;
  - snarf: the snapshot, O_RDWR, the set at the clunk, U+FFFD, EFBIG;
  - a folder read and written, with ENOTEMPTY in Linux's numbering and EISDIR;
  - a rename, and the fids that follow it;
  - a read-only share's EROFS;
  - a link served and not walked;
  - a walk from a directory the Mac replaced with a link: ELOOP, and the record of where each
    walk began;
  - chown and mknod refused;
  - EOPNOTSUPP for Txattrwalk and Twalkgetattr;
  - readdir cookies;
  - locks across owners.

Mutants (aux-gate resid/ic-mut.py, `IC-MUT DONE bad=0`): 17 of the server's rules each turn a
test red. They cover the read-only checks, ownership, snarf's three rules, partial walks, the
rename's fid rewrite, unlink's EISDIR, the clunk's lock release, the errno map, the no-follow
flag, `.` and `..` in listings, and the lock table's owner and carve.

`ls-mac-line` with `THYLACINE_MAC_APP` set runs the app in the fake's place. Against the scripted
guest (aux-gate `scendry-mac`), the app gave the fake's verdicts: 2 runs green, and 11 faults
red, each at its own check. In a guest under HVF (run 8, 2026-11-14, fce70866d) it was green,
all five legs, with the app in the fake's place: the app's first guest run.

`ls-mac-folders` drives the shared folders through the guest's kernel against the app and
checks each change in the Mac's folder. The app's O_NOFOLLOW_ANY is reached by a walk from a
fid the kernel holds below a directory the Mac replaced with a link, which `mac-line-probe
beneath` makes with an O_PATH handle; a cwd is a name, re-walked from the root, and the kernel
meets the link itself; the app's record names where each walk began. Against a scripted guest
that runs each command as the kernel does (9P against the real app, one name per walk, open
before create, a served link resolved beneath the mount, the cwd a name, a console child's
stdin a closed pipe, the probe released by the `go` the Mac writes; aux-gate
`scendry-folders`): the clean app is green, all seven legs, and fourteen faults are red, each at its own leg and with its own words: nine of the guest (no `docs` listed, no write, no rename, no copy, links resolved from the guest's root, a cwd walked from a held fid, a held handle re-walked by name -- the app saw no walk of `secret.txt` -- a probe that walks before the swap, and one that never reads the Mac's `go`) and five of the app's rules, built into its binary (resid/mutbin.py: a read-only create, a read-only open for writing, no O_NOFOLLOW_ANY -- the outside file's words reached the guest through the held handle -- the host uid reported, group bits kept); against the 9f18c7b68 scenario it gives run 11's words; resid/scendry-folders-8.out, SCENDRY-FOLDERS DONE bad=0. In a guest under HVF (runs 9, 10 and 11, 2026-11-14) legs 1 to 4
were green with the app. Leg 5 has not been green in a guest: run 9's assumed a cwd holds a fid,
and in runs 10 and 11 the probe's release came through stdin, which a console child never
reads. Leg 5 as it now stands is NOT YET RUN IN A GUEST, and none of it is audited (round R5).

## Seams

- [[sub-mac-line]]: the guest's end, the framing (`macline`, built by both ends), and
  `ls-mac-line`.
- [[sub-libthyla-rs]]: `ninep`, the codec the server speaks. The creating and naming half of
  9P2000.L landed in it for this server.
- [[sub-macrunner-app]]: the window. `main.rs` parses the line's arguments and starts the line.
- The settings that choose the shares (the app's preferences) are not built. Until they are,
  the shares are the `--share` arguments.

## Caveats

- Guest and host locks do not meet. The table arbitrates the guest's owners, and it does not see
  a host process locking the same file through fcntl.
- A link the guest creates is stored as written. The app never follows one, but a program on the
  Mac that follows it reaches wherever it points.
- After a rename, other sessions' fids keep the old path, as they do after a rename on the Mac.
  They read ENOENT until walked again.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
