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

### Failure and cleanup

`mount` and `unmount` set `$status` and `$errstr`. Run `echo $errstr` to display
a failure reason. A second mount of one post is refused; a missing post, busy
name or exhausted quota also fails. Confirm the service announcement before
mounting, and use `haul -v` for connection and handshake progress.

A remote disconnect fails pending filesystem operations. The relay ends when
its connection or elevated scope ends. Token retrieval through corvus is not
implemented; continue to use the explicit file or environment-source interface.
