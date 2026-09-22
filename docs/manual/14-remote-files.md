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

### Failure and cleanup

`mount` and `unmount` set `$status` and `$errstr`. Run `echo $errstr` to display
a failure reason. A second mount of one post is refused; a missing post, busy
name or exhausted quota also fails. Confirm the service announcement before
mounting, and use `haul -v` for connection and handshake progress.

A remote disconnect fails pending filesystem operations. The relay ends when
its connection or elevated scope ends. Token retrieval through corvus is not
implemented; continue to use the explicit file or environment-source interface.
