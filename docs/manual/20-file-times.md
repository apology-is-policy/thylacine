# File Times

Every file and directory carries three times, each a count of whole seconds
since 1970-01-01 00:00 UTC: the access time, the modification time and the
change time. The file service that holds the file keeps them. It sets all three
when the file is created, moves the modification and change times when the
file's contents change, and moves the change time alone when the file's mode,
owner or group changes. Creating or removing an entry in a directory moves that
directory's modification and change times. The file service that holds the
system's own files does not move a file's access time when the file is read.

The access and modification times can also be set directly. `touch` sets them
to the current time or to a given time, `cp -p` and `cp -a` give a copy the
times of its source, and programs built with Pouch or Go, and Linux programs in
a container, set them through the POSIX calls `utimensat`, `futimens`, `utimes`
and their relatives. The change time cannot be set: setting either of the
other two moves it to the file service's clock.

Setting a time to a particular value, or setting only one of the two times,
requires the file's owner. Setting both times to the current time, which is
what `touch` does without a time operand, also succeeds for a user who may
write to the file. Programs that rely on file times, such as build tools that
compare modification times and font caches that record a directory's
modification time and rescan the directory when it changes, read the times the
file service records.

## In Practice

### See a file's times

```sh
stat notes
```

`stat` prints the three times on one line, as seconds since the epoch, in a
line such as:

```text
Access: 1761134400   Modify: 1761134400   Change: 1761138012
```

`date +%s` prints the current time in the same form, which allows a time to be
compared with the clock.

### Update a file to the current time

```sh
touch notes
```

`touch` sets both times of `notes` to the current time of the file service
that holds it. A `notes` that does not exist is created empty, with mode `0666`
reduced by the creation mask and by the directory's limit (described in The
Creation Mask); an existing file's contents are not changed. Write permission
on the file is sufficient. With `-c`, a missing file is not created and no
diagnostic is printed.

### Set a particular time

```sh
touch -t 202510221200 notes
touch -d 2025-10-22T12:00:00Z notes
```

Both commands set the access and modification times of `notes` to 12:00 UTC on
22 October 2025, after which `stat notes` shows `Modify: 1761134400`. `-t`
takes the form `[[CC]YY]MMDDhhmm[.SS]`; `-d` takes `YYYY-MM-DDThh:mm:SS`,
optionally followed by a fraction of a second and a `Z`. Both are read as UTC,
because the system has no timezone database. Only the file's owner, or a shell
holding elevated capabilities from Imperium, may set a particular time; any
other user receives `permission denied`, even with write permission.

`-a` changes only the access time and `-m` only the modification time. A
change to one time alone requires the owner, even when the time is the current
time.

### Give one file the times of another

```sh
touch -r old.c new.c
```

`new.c` receives the access and modification times of `old.c`, which must
exist. The times are copied in whole seconds.

### Copy files and keep their times

```sh
cp -p report.txt archive/
cp -a project project-copy
```

`cp -p` gives each copy the source's access and modification times, then its
owner and group, then its mode. A copy whose owner or group the user may not
give keeps the user's own, as POSIX permits, without a diagnostic; times and
mode that cannot be set are reported and make `cp` exit with status 1. `cp -a`
copies a directory tree with `-p` applied to every entry. Each directory
receives its times after its entries have been copied into it, because creating
those entries moves the directory's modification time.

`cp -a` also implies `-P`, which copies a symbolic link as a link. Native `cp`
cannot create symbolic links, so `cp -a` and `cp -P` refuse a symbolic link
in the source with `cannot copy symbolic link`, and copy the rest of the tree.
`cp -R -p` follows symbolic links and copies what they point to.

### Set times in a Linux container

Inside a container started with `viv run`, the Linux `touch`, `cp -p` and
`stat` behave as they do on Linux, including `touch -d @1000000000` for a time
given as seconds since the epoch. Their refusals follow POSIX: a request that
only the owner could make fails with `EPERM` (`Operation not permitted`), and a
current-time update refused for lack of write permission, or a path whose
directory cannot be searched, fails with `EACCES` (`Permission denied`).

### Command reference

```sh
touch [-acm] [-r ref | -t stamp | -d date] file...
```

| Option | Effect |
| --- | --- |
| `-a` | Change only the access time. |
| `-m` | Change only the modification time. |
| `-c` | Do not create a `file` that does not exist. |
| `-r ref` | Use the times of `ref`, in whole seconds. |
| `-t stamp` | Use `[[CC]YY]MMDDhhmm[.SS]`, in UTC. |
| `-d date` | Use `YYYY-MM-DDThh:mm:SS[.frac][Z]`, in UTC. |

Without `-a` or `-m`, both times are set. Without `-r`, `-t` or `-d`, the times
are the file service's current time; only one of the three may be given. In
`-t`, a two-digit year from `69` to `99` is in the 1900s and one from `00` to
`68` in the 2000s, a missing year is the current year, and `.SS` may be `60`.
In `-d`, the `T` may be a space, the fraction may follow a comma instead of a
period, and digits past nanoseconds are discarded. A time before the epoch is
refused.

`touch` exits with status 0 when every `file` succeeded and 1 when any failed
or a time operand was rejected; an invalid option, a missing option argument
or more than one time source exits with status 2:

| Message | Cause |
| --- | --- |
| `touch: cannot touch 'file': reason` | `file` could not be created or opened, and setting its times through its name failed too. |
| `touch: setting times of 'file': permission denied` | The user is not the owner and asked for more than both times set to the current time, or, with `-c`, may not write to the file either. |
| `touch: setting times of 'file': operation not supported` | The file's service keeps no times, as for a kernel device such as `/dev/null`. |
| `touch: 'value': invalid date format (want ...)` | The `-t` or `-d` operand is not in the form shown. |
| `touch: 'value': invalid date` or `invalid time of day` | The operand names a date or time that does not exist. |
| `touch: 'value': time before the epoch` | The operand names a time before 1970. |
| `touch: cannot specify times from more than one source` | More than one of `-r`, `-t` and `-d` was given. |

## Technical Details

### Where times are kept

The file service that holds a file records its times, and the kernel holds
them only in its cache of file attributes. Where a time is set to the current time, the current
time is that of the file service's clock, so a file on a remote mount takes the
remote host's time. The file service stores times with nanoseconds. The status
that the kernel returns for a file carries whole seconds, so `stat`, the
`stat` family of POSIX calls and Go's `os.Stat` report the seconds alone, and
`touch -r` and `cp -p` copy whole seconds.

### How a request is checked and sent

`touch`, `cp`, the POSIX calls and the container's Linux calls all reach the
same kernel call, the one `chmod` also uses, which changes a file's metadata
through a descriptor. A request names each of the two times as unchanged, the
service's current time, or a value, so the POSIX forms that leave one time
alone or ask for the current time map onto it directly. When a call names a
file by its path, it first opens the file for walking only, without read or
write access, as `chmod` does; each directory in the path must allow the user
to search it.

The kernel checks permission before it sends anything, on every file service
that enforces permissions. A value for either time, or a change to one time
alone, requires the file's owner or the host-owner capability; both times set
to the current time also succeed with write permission on the file. Only then
does the kernel send the request to the file service as a 9P `Tsetattr` with the
matching bits and values, and drop its cached copy of the file's attributes so
that the next `stat` reads the new times. The file service applies both times
and stamps the change time from one reading of its clock, while it holds the
file's lock against concurrent writes.

The kernel reports every permission refusal as `EACCES`. POSIX requires
`EPERM` where only ownership would have allowed the request, so the Pouch
library, the Go library and the container's Linux layer report `EPERM` for
such a refusal and keep `EACCES` for a current-time update and for a path
that could not be searched. Native programs report both as `permission
denied`.

### Limitations

A program that has written to a file through a descriptor of its own may still
hold some of that data in the kernel, which sends it to the file service later,
at the latest when the program closes the file. When that data arrives, the
file service moves the modification time to its clock, which replaces a time
set in the meantime through another descriptor or through the file's name. The
descriptor that sets the times has its own pending data sent first, so a
program that writes a file and then sets its times through the same descriptor,
as `cp -p` does, keeps the times it set.

A descriptor opened for reading only, on a regular file of at most 128 KiB
whose contents the kernel already holds, may be served from the kernel's cache
without a connection to the file service. An empty file qualifies once the
kernel has looked it up, and so does a file that a program on the system has
just written. Setting times or
the mode through such a descriptor fails with `EOPNOTSUPP` (`operation not
supported` in native programs); setting them through the file's name, or
through a descriptor opened for writing, succeeds. A file whose service
keeps no times, such as a kernel device like `/dev/null`, gives the same error
through either form.

POSIX's `AT_SYMLINK_NOFOLLOW` and `lutimes` set the times of a symbolic link
itself, and both work for programs built with Pouch and in containers. Native
`touch` follows a symbolic link and sets the times of the file it points to.
