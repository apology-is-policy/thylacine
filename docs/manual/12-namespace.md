# Namespaces

A namespace is the tree of names in which a process resolves paths. Each
Thylacine process has its own. A shell's namespace is a private copy made when
the session starts, and every command the shell runs starts with a copy of the
shell's namespace as it stands at that moment. The Utopia builtins `bind`,
`mount` and `unmount` change the shell's namespace. A change applies to the shell
and to the commands it starts afterwards; commands already running, other
sessions and other users keep their own namespaces, and the change ends when the
shell exits.

`bind` makes a file or directory that the shell can already reach appear at a
second name. `mount` does the same for a service posted under `/srv`, which it
attaches first; its use with remote file servers is described in Remote Files
with Haul. `unmount` removes a mount or a bind. When a directory is bound before
or after another directory instead of replacing it, the two form a union: a
lookup searches the members in order, and a listing shows the names of every
member.

A union is also how an ordinary user writes into a directory that the system
owns. The kernel admits a create through a file service's connection only from
the identity that opened the connection or from the service that posted it, so
a user cannot create files on the system's disk whatever the directory's mode.
A directory of the user's own, bound before such a directory with creation
allowed, receives the user's new files, and the existing files stay visible
beneath it.

## In Practice

### Add a writable layer to a directory

`/var` is a directory on the system's disk, so an ordinary user's attempt to
create a file in it, such as `touch /var/notes`, fails with a permission error.
Bind a directory from your own storage before it, allowing creation:

```sh
mkdir -p /tmp/var-layer
bind -bc /tmp/var-layer /var
touch /var/notes
```

`/tmp/var-layer` is an illustrative name. The session's `/tmp` is bound from the
user's home when the shell starts, so a directory in it is the user's own. After
the bind, a name in `/var` is looked up in `/tmp/var-layer` first and then in
the original `/var`, and a file or directory created in `/var`, by the shell or
by a program it starts, is created in `/tmp/var-layer` and owned by the user:
`ls /tmp/var-layer` lists `notes`. Files that exist only in the original
directory can still be read. Removing or rewriting one of them is checked
against that file and its directory as before, so it fails where it would have
failed without the bind.

The bind lasts until `unmount` or the end of the shell; the files created
through it remain in `/tmp/var-layer` afterwards. A program that the shell
starts after the bind sees the union, so a test suite that writes into its own
directory can run from a shell that has bound a layer over that directory.

### Remove a bind

```sh
unmount /var
```

`unmount` removes one mount or bind at the name. Where several are present, it
removes the first in search order; repeat it to remove the others. The original
directory is never removed by `unmount`, and files created in a layer are not
deleted when the layer is unbound.

### See what is bound

`ns` lists the shell's mounts and binds, one entry each, with the name that was
mounted onto, the source and the flags. `ns PID` lists those of process `PID`.

### Command reference

```sh
bind [-b|-a] [-c] new old
mount [-b|-a] [-c] service old [aname]
unmount old
```

`bind` makes `new` appear at `old`. `mount` attaches the service posted at
`service`, a name under `/srv`, and mounts the tree it serves at `old`; `aname`
selects which tree, where the service exports more than one. Both take the same
options:

| Option | Effect |
| --- | --- |
| none | The source replaces `old`: the name resolves to the source alone. |
| `-b` | The source joins a union with what is at `old` and is searched before it. A later `-b` is searched before an earlier one. |
| `-a` | The source joins the union and is searched after what is already there. |
| `-c` | A create in the union directory is made in the first member bound with `-c`. In a union with no such member, a create fails with a permission error. |

`-b` and `-a` cannot be combined. `-c` can be used alone or with either of them,
as `-bc` or `-ac`. Options precede the operands, and `--` ends them.

Both names must already exist; `bind` never creates `old`. A relative name is
resolved against the shell's current directory, where `..` removes the
preceding component, as in `cd`. A directory can be bound only over a directory
and a file only over a file, and only a replacement can stand at a file; any
other combination fails with `not a directory`. When `old` names a symbolic
link, the bind is made on the link itself; end the name with `/` to bind on the
directory the link points to.

### Failures

`bind`, `mount` and `unmount` set `$status` to 1 and `$errstr` to a reason when
they fail; run `echo $errstr` to display it. The reasons from `bind` are:

| Reason | Cause |
| --- | --- |
| `bind: cannot open NEW: no such file or namespace entry` | `new` does not exist. |
| `bind: cannot open NEW: permission denied` | A directory on the way to `new` may not be searched. |
| `bind: cannot bind at OLD: not a directory` | One name is a directory and the other is not, or `-b` or `-a` was given at a file. |
| `bind: cannot bind at OLD` | `old` does not exist, or the namespace has no room for another entry. |
| `bind: -b and -a are mutually exclusive` | Both orderings were given. |
| `bind: unknown option -X` | An option other than `-b`, `-a` and `-c`. |
| the usage line, starting `usage: bind` | Other than two operands. |

A failed `bind` leaves the namespace unchanged.

## Technical Details

### Where a namespace lives

The kernel keeps each process's namespace as a mount table, together with the
process's root and current directory. Starting a process copies the table, so a
change that a running command makes affects only that command. For this reason
`bind`, `mount` and `unmount` are shell builtins: as separate programs they
would each change their own copy and exit. Plan 9, where the design comes from,
shares a namespace between a parent and its child unless asked otherwise;
Thylacine always copies it.

`bind` opens `new` as a handle that can be walked but not read or written, and
passes the handle to the kernel's mount call with `old` and the flags. The
kernel resolves a mount point from the namespace root, which is why the shell
turns a relative `old` into an absolute name first.

### How a union resolves

A lookup in a union directory tries each member in order and stops at the first
that holds the name. A listing reads every member and shows each name once, from
the first member that holds it. A create is made in the first member bound with
`-c`. Removing a name acts on the member that holds it, so a name in a member
that the user may not change cannot be removed through a member the user owns.

### Who may create

A file service creates every file in the directories it serves, and it records
the new file's owner from the connection the request arrived on: the identity
that opened the connection, or its own identity when it relays requests for
others. The kernel therefore admits a create through a connection only from one
of the connection's two ends, the identity that opened it and the service that
posted it, and refuses any other identity's create with `EACCES`. The refusal
holds even in a directory whose mode would allow the create, and for a shell
holding elevated capabilities from Imperium as well: those capabilities
override the permission checks on existing files and leave this rule
unchanged.

The system's disk is one connection, opened and posted by the system, so every
user's create there is refused. A user's home is served by a per-user service
that runs as that user and posts the home; login opens the connection as the
system, so the home's two ends are the system and the user, and the user's
creates there are admitted and recorded as the user's. The `/tmp` that the shell
binds from the home uses the same connection, which is why a layer in `/tmp`
can receive a user's files in a directory of the system's disk.
