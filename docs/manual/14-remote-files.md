# Remote files with Haul

Haul connects a remote 9P2000.L filesystem over IPv4 TCP. Supplying a token
selects the npxf authenticated encrypted channel; omitting it selects plain,
unencrypted 9P. The current implementation accepts dotted IPv4 addresses.

## In Practice

### Prepare a host-side server

npxf is an officially supported
host-side 9P2000.L server for Haul. It runs on Linux and macOS and uses
OpenSSL 3 for its authenticated encrypted channel. Install a C++20 compiler,
CMake and the OpenSSL development package on the host, then build it:

```sh
git clone https://github.com/apology-is-policy/npxf.git
cd npxf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On macOS, Homebrew provides `cmake` and `openssl@3`. If CMake does not find
OpenSSL, add `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"` to its configure
command. On Debian or Ubuntu, install `build-essential cmake libssl-dev`.

Create a token and export a directory. These commands run on the host:

```sh
umask 077
openssl rand -base64 32 > npxf.token
mkdir -p export
printf 'Hello from the host\n' > export/hello.txt
./build/npxf-server -r export -t npxf.token -l 127.0.0.1:5640 -R
```

`-R` makes the export read-only; omit it when the guest should write files.
The server uses its host account's permissions. Copy the token securely into
the guest and use that file with Haul's `-t` option. A token holder can access
the exported tree with the server's permissions; use a separate random token
for each separately trusted export.

Haul accepts a token shorter than 16 bytes with a warning, `haul: warning: the
token is only N bytes`. Anyone who can connect to the server can test guesses
at the token offline: one connection gives them what they need to check each
guess on their own computer, without contacting the server again, so a short
or memorable token can be found by trying candidates. The `openssl rand
-base64 32` command above writes 32 random bytes as 44 characters.

For QEMU user networking on the same host, `10.0.2.2!5640` reaches this
loopback listener. To serve another machine, bind npxf to the host's reachable
interface address and use its dotted IPv4 address in Haul. The server requires
a token; Haul's plain 9P mode cannot connect to it. Stop the host server with
Ctrl-C after unmounting its clients.

### Mount in the current shell

Provision a token file separately and restrict its permissions with
`chmod 600 /path/to/token`. For a mount in the current shell, enter
`imperium post` and complete the physical SAK and trusted key prompt.
The currently supported trusted path is serial; QEMU's serial monitor sends
BREAK with Ctrl-A, then b. In the elevated shell:

```sh
mkdir -p /tmp/remote
haul --post -t /path/to/token remote 10.0.2.2!5640 &
mount /srv/remote /tmp/remote /
cat /tmp/remote/hello.txt
unmount /tmp/remote
wait
abdicate
```

Replace the example address, token path and filename with your server's values.
`abdicate` ends the elevated scope and its background relays.

### Confirm that a directory is remote

A long listing of the directory that holds a mount point marks a Haul mount with
the realm `remote`. With the mount above in place, run:

```sh
ls -l /tmp
```

The `REALM` column of the `remote` entry reads `remote`. A directory with nothing
mounted on it reads `fs`, and a local mount point, such as `/srv` in a listing of
`/`, reads `mount`. `la`, the shell's alias for `ls -la`, shows the same column,
and `realm /tmp/remote` prints the realm of a single path. If the shell holds so
many mounts that their list is too long to read whole, the listing says
`mount list incomplete`, and a mount point missing from the list shows its
ordinary realm.

`ns` with no operand prints the mount table of the shell that runs it. The line
for `/tmp/remote` names the source `/srv/remote`, the service the mount came
over, and reads `remote` in the `REALM` column; its `FLAGS` column shows any
restriction the mount carries, such as `noexec`. A mount made by Haul's private
form, described under Separate remote sessions, names the source `#|`. `ns 0`
prints the system's root namespace instead, which does not contain mounts made
in a shell.

Both forms of Haul mark the mount: the private form marks the session it
attaches, and `haul --post` marks the posted service, so a mount made through
the service with the shell's `mount` builtin is marked as well. After `unmount`,
the directory reads `fs` again.

### Ownership and permissions

Thylacine reports every file on a Haul mount as owned by the user who mounted
it, with that user's primary group. The permission bits are the host's. A
private host tree, such as a `0700` directory of `0600` files, is therefore
readable by the user who mounted it, and writable when the server permits
writes. The private form and a mount of a posted service behave the same way,
and neither needs an option. The host's own ownership does not change. To see
the ownership Thylacine applies, run:

```sh
stat /tmp/remote/hello.txt
id
```

The `Uid` and `Gid` fields that `stat` prints match the `uid` and `gid` that
`id` prints, whatever ids the file has on the host. The `Mode` field is the
host file's mode.

`chmod` on a Haul mount changes the host file's mode. A change of owner or
group is refused. A file or directory created through the mount belongs on the
host to the server's account, with the group the host assigns by default.
The host server still applies its own account's permissions. If a read fails
with a permission error, check the file's mode on the host: its owner bits are
the ones that apply to the user who mounted it.

### Command reference

- `haul --post [-t FILE | --token-env VAR] [-v] NAME HOST!PORT`: publish a
  single-session byte service at `/srv/NAME`; requires scoped post authority.
- `mount /srv/NAME PATH [ANAME]`: attach the byte service and mount it in this
  shell's namespace. `/` is the usual attach name for npxf exports.
- `unmount PATH`: remove the mount. When its transport closes, Haul exits.
- `haul [-a ANAME] [-t FILE | --token-env VAR] [-v] HOST!PORT PATH [COMMAND ...]`:
  mount privately, then run a child command or park. Use an absolute command
  path, such as `/bin/ut`.
- `imperium --list`: inspect the current elevated scope. `abdicate`: leave it.
- `ls -l DIR` or `la DIR`: the `REALM` column reads `remote` for a Haul mount
  point in DIR and `mount` for a local one. `ns [PID]`: the mount table of the
  calling shell, or of process PID, with each mount's realm and flags.

Haul options must precede the two operands. Post mode does not accept `-a` or a
child command. Names are single printable ASCII components, at most 32 bytes,
excluding slash, whitespace, `.` and `..`. `HOST:PORT` also works; when composing
an address from shell variables, use the quoted form `"$host:$port"`.

### Separate remote sessions

One post serves one mount. Use another name and Haul process for another remote
session. A scope may hold two active posts; the shared registry has four
cap-owned slots. Exited posts can be replaced without permanently consuming
new slots. A TCB service name cannot be taken over by a posted user service.

For a one-command private mount, use:

```sh
haul -t /path/to/token 10.0.2.2!5640 /tmp/remote /bin/cat /tmp/remote/hello.txt
```

## Technical Details

### Namespace and identity

Namespaces are per-process. Backgrounding the private mount form cannot add a
mount to its parent shell. The posted-service form and shell `mount` builtin
exist to perform that mount in the calling shell itself. The service accepts
only a client with the poster's kernel-stamped principal identity.

Not every name in `/srv` is yours to open. A service that belongs to the
system, such as the storage coordinator, refuses a connection from an ordinary
program: the open fails and `mount` reports a permission error. Services you
posted yourself, Haul's included, are unaffected — you may always mount a
service you posted. The refusal depends on the authority of the program doing
the opening, not on the permission bits of the name, so `ls -l /srv` does not
predict it.

### File ownership

A 9P server reports each file's owner as a numeric user and group id from the
host. On macOS these are typically user 501 and group 20. No Thylacine user
holds the host's ids, and the kernel checks file permissions itself, against
the owner the server reports. Without an adjustment, every Thylacine user would
be subject to the host file's "other" permission bits, and a private host
directory would be unreadable to the user who mounted it.

The kernel therefore marks a Haul session when it is attached. The private form
requests the mark on its attach. `haul --post` marks the posted service, and
every attach through that service carries the mark, which is why a plain
`mount /srv/NAME` needs no option. On a marked session the kernel reports every
file as owned by the attaching user and that user's primary group, and keeps the
server's permission bits. The adjustment is made where the kernel converts the
server's attributes, so `stat`, directory listings and the kernel's own
permission checks all see the same owner. The mark is fixed before the mount's
root becomes usable and does not change for the life of the session.

The mark gives the mounting user no access to the server that the connection
did not already give. The kernel accepts the mark only where the process that
attaches holds the raw connection: the private form's own pipes to its relay,
or a byte service. That process could send the same requests to the server
directly. Nothing identity-related is sent to the server. The attach names no
user, a create asks the server to keep its default group, and a change of owner
or group is refused before it reaches the server.

### How a mount is marked remote

The kernel cannot observe where a Haul session's data goes. Haul relays the 9P
messages between the host connection and two local pipes, and the kernel uses
those pipes as the session's transport; the TCP connection belongs to Haul and
the network service. Haul therefore declares the session remote when the session
is created. The private form sets a flag on its attach, and `haul --post` sets a
flag on the posted service, which marks every session attached through that
service. The kernel records the declaration with the session before the mount's
root becomes usable and does not change it for the life of the session.

The mark is displayed and has no other effect. The kernel's list of a process's
mounts ends a mount's line with the word `remote` when the mount's source belongs
to a marked session, and `ls`, `stat`, `realm` and `ns` read that list. The same
list names a mount's source by the file its session came over: the service in
`/srv` that the shell's `mount` opened, or `#|`, the name of the pipe device, for
Haul's private form, whose session runs over pipes that have no names. Name
resolution, permission checks and caching behave identically on marked and
unmarked sessions. Any program that attaches a session can declare it remote, so
the mark reports what the attaching program stated; Haul states it because Haul
holds the network connection. A union's own directory, which the kernel keeps as
a member of the union, is never marked, because no program mounted it.

`ls` identifies a mount point by name. It reads its own mount list, which is a
copy of its shell's, and compares the absolute path of each entry it lists with
the mount-point names in that list. The name of a mount point is recorded when
the mount is made, so a mount point that is listed under a different name, for
example through a bind, shows the realm the directory has without the mount. The
list does not quote names, and a mount point whose name contains whitespace is
not recognized reliably.

### Failure and cleanup

`mount` and `unmount` set `$status` and `$errstr`. Run `echo $errstr` to display
a failure reason. A second mount of one post is refused; a missing post, busy
name or exhausted quota also fails. PATH must be a directory: a mounted service
is a directory tree, and mounting one over a file fails with
`mount: cannot mount at PATH: not a directory`. A symbolic link counts as a file
here, because the mount point is the link itself. End PATH with `/`, as in
`/tmp/ln/`, to mount on the directory the link points to. That fails while a
file is mounted on the link itself; unmount it first. Confirm the service
announcement before mounting, and use `haul -v` for connection and handshake
progress.

Haul writes its messages to standard error, so they appear in the terminal or
Halcyon tile that ran it. When the server cannot be reached, Haul names the
address and the reason, and exits with status 1 before it mounts anything:

- `haul: cannot reach 10.0.2.2!5640: connection refused`: the host answered, but
  nothing is listening on that port. Start the server, or check the port.
- `haul: no answer from 10.0.2.2!5640 yet -- still trying`, after two seconds,
  and then `haul: cannot reach 10.0.2.2!5640: no answer (timed out)` after about
  15 seconds: the host never replied. Check the address and that the host is up.

A Haul started with its standard error closed writes these messages to the
system console instead. A launcher that connects standard error to `/dev/null`
discards them. In the command form, lines
that begin with `haul:` come from Haul and the command's own output does not
carry that prefix; when the command exits with a non-zero status, Haul prints
`haul: the command exited non-zero` and exits with status 1.

A server that sends a reply larger than the message size (msize) agreed for
the session breaks the session. Haul refuses the reply, reports it in two lines
such as `haul: 10.0.2.2!5640 sent a 8203-byte reply, over the session's
4096-byte msize -- refusing it` and `haul: the 9P session with 10.0.2.2!5640
is broken -- the mount is dead`, and exits with status 1. Pending filesystem
operations on the mount fail. With the encrypted channel the reply came from
the server, and the two sizes in the first line identify the fault for its
maintainer; on a plain connection anything on the network path could have
sent it.

When the server closes the connection, Haul prints `haul: 10.0.2.2!5640 closed
the connection -- the mount is dead` and exits with status 1. When Thylacine
itself ends the session -- it refused a reply that answers no request it made,
or the mount was taken down -- Haul prints `haul: Thylacine ended the 9P
session with 10.0.2.2!5640 -- the mount is dead` instead, and exits with status
1: the fault lies in the session, not in the server's connection.

A remote disconnect fails pending filesystem operations. The relay ends when
its connection or elevated scope ends. Token retrieval through corvus is not
implemented; continue to use the explicit file or environment-source interface.
