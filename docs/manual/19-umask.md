# The Creation Mask

Every new file and directory receives a mode: the nine permission bits that
decide who may read it, write it, and execute or search it. The program that
creates the file asks for a mode, and the kernel reduces that mode in two steps
before the file service records it, first by the creating process's creation
mask and then by the mode of the directory that holds the new entry. The
creation mask, or umask, is the set of permissions that a process withholds from
everything it creates. The Utopia builtin `umask` displays and changes the
shell's mask, and every command the shell starts afterwards inherits it.

A process starts with the mask of the process that started it, and the mask in
force from boot is `0022`, which withholds write permission from the group and
from other users. Thylacine's own programs ordinarily ask for `0644` for a file
and `0755` for a directory, so under the default mask their new entries keep those modes.
Programs written for POSIX systems, including Linux programs in containers and
programs built with Pouch or Go, commonly ask for `0666` and `0777` and rely on
the mask; under `0022`, in a directory that grants them, they produce `0644` and
`0755` as well.

The directory's limit follows Plan 9. A new file receives read or write
permission for its owner, its group or other users only where its directory
grants the same permission, and a new directory receives each of its nine
permissions only where its parent grants it; a new file's execute permissions
are not limited by its directory. The limit applies when the entry is created.
Its owner can change the mode afterwards with `chmod`. Which users may create in
a directory at all is a separate rule, described in Namespaces.

## In Practice

### See the mask

```sh
umask
```

`umask` with no operand prints the shell's mask as four octal digits, such as
`0022` in a new session. The last three digits list the permissions withheld
from the owner, the group and other users, each the sum of 4 for read, 2 for
write and 1 for execute or search; the first digit is always 0.

### Keep new files private

```sh
umask 077
touch /tmp/private
mkdir /tmp/private-dir
```

After `umask 077`, the files and directories that the shell and its commands
create grant nothing to the group or to other users. `ls -l` shows
`/tmp/private` as `-rw-------`, because `touch` asks for `0666`, and
`/tmp/private-dir` as `drwx------`, because `mkdir` asks for `0755`. The mask
applies to commands the shell starts after the change. Commands that are
already running, other sessions and existing files are unaffected, and the mask
lasts until it is changed again or the shell exits. `umask 022` returns to the
default.

### Set the mask for a Linux container

A container started with `viv run` begins with the mask of the shell that ran
`viv`, so `umask 077` before `viv run` makes the container's new files private
as well. Inside the container, the Linux shell's own `umask` builtin displays
and changes the mask of the container's processes, as it does on Linux, and a
change made there ends with the container.

### Widen a mode after creation

The directory's limit applies only at creation. The owner of a file can grant
more permission afterwards with `chmod`, such as `chmod 644 notes` for a file
created in a directory that grants no group or other read permission. Granting
it has an effect only for users who can also search the directories above the
file.

### Command reference

```sh
umask [mode]
```

With no operand, `umask` prints the mask as four octal digits. With one operand,
it sets the mask to `mode`, which must be one to four octal digits with a value
no greater than `0777`; `22`, `022` and `0022` are the same mask. Symbolic
modes, such as `u=rwx`, are not supported. `umask` is a shell builtin, so it
cannot be used in a pipeline, and its output cannot be captured with `$(...)`.

`umask` sets `$status` to 0 when it succeeds. When it fails, it sets `$status`
to 1 and `$errstr` to the reason, and the mask is unchanged:

| Reason | Cause |
| --- | --- |
| `umask: bad mask MODE` | `mode` is not one to four octal digits, or its value is greater than `0777`. |
| `usage: umask [mode]` | More than one operand was given. |

## Technical Details

### Where the mask is kept

The kernel keeps the mask with each process and copies it to a new process when
the process starts, whether a command is spawned by a shell or a Linux process
forks inside a container; the threads of one process share one mask. A process
changes its mask through a single call that sets a new mask and returns the old
one, and the kernel refuses a mask with any bit above `0777`. The `umask`
functions that programs built with Pouch and Go call, and the Linux `umask`
call inside a container, discard such bits first, as POSIX requires, so a
POSIX program's `umask` call never fails.

Because the call always sets a mask, reading one takes two calls: Utopia's
`umask` sets `0777`, the mask that withholds every permission, and restores the
mask it read. An entry created between the two calls, by another thread of the
shell, would receive fewer permissions than intended and never more.

### How a new entry's mode is decided

The kernel applies both steps on each path that sends a create to a file
service: the create call, and the directory and node creates of the
asynchronous I/O ring. On each, it does so after it has checked that the
creating process may create in the directory and before it sends the request
to the file service. It first removes
the bits of the creator's mask from the requested mode, then applies the
directory's limit, using the directory mode it read for the permission check.
The file service records the mode it receives. A service posted in `/srv` is
not a file, and the mode it is posted with is used as given.

POSIX systems apply the mask alone, and Plan 9 applies the directory's limit
alone. With both, each user decides how open the entries that the user's own
programs create are, and the directory's owner decides the most that a new
entry in that directory can grant: a process whose mask is 0 still cannot create
a file that the group or other users can write in a directory that withholds
write permission from them.

For a program written for Linux, the directory's limit is the observable
difference. In a directory of mode `0700`, a compiler that asks for `0777` for a new
executable produces `0711` (`rwx--x--x`) under the default mask, where Linux
produces `0755`, and a program that asks for `0666` produces `0600` instead of
`0644`.
