# The Mac's files at /mnt/mac

When Thylacine runs in Thylacine for Mac, the Mac app serves each login session a small tree at `/mnt/mac`. The tree holds `snarf`, the text on the Mac's pasteboard, and `folders`, which holds one directory for each folder shared from the Mac. A program in the session reads and changes these files with the ordinary tools; a change to a shared folder is made on the Mac at once, by the Mac app, as the Mac user who runs it.

The tree is present only while the Mac app is running and connected. Under any other host there is no line to the Mac, and `/mnt/mac` stays an empty directory. A session has its own mount of the tree, made when the session started, so another user's session cannot reach it. A shared folder can be shared read-only, in which case every change to it is refused. Links that a shared folder holds are followed within `/mnt/mac`, as links in a Haul mount are within that mount (see Remote files with Haul).

## In Practice

### Check that the Mac is connected

List the tree:

```sh
ls /mnt/mac
```

A connected session lists `folders` and `snarf`. If the Mac app was not on the line when the session started, login printed `newns: /mnt/mac not mounted: I/O error` before the shell's first prompt, and `/mnt/mac` is empty for the whole session. When the Mac app is connected again, log in again to get a session with the tree. Under a host other than the Mac app, login prints nothing about `/mnt/mac`.

### Read and set the Mac's pasteboard

`cat /mnt/mac/snarf` prints the text on the Mac's pasteboard. Writing the file sets the pasteboard:

```sh
cat /mnt/mac/snarf
echo 'from the guest' > /mnt/mac/snarf
```

The pasteboard is set when the file is closed, to everything written since it was opened, so opening the file for writing and closing it without writing anything empties the pasteboard. Text that is not valid UTF-8 is set with each invalid sequence replaced by U+FFFD. A read takes the pasteboard's text when it starts at the beginning of the file, and later reads of the same open file continue that text, even if the Mac's pasteboard changes meanwhile. The file cannot be opened for reading and writing at once; such an open fails with `permission denied`. Its size reads 0 and its mode `0600`.

Only plain text is carried. A pasteboard that holds no text, such as one holding only an image, reads as empty. Text larger than 16 MiB can be neither read nor written through `snarf`: the read, or the write that passes 16 MiB, fails with `file too large`.

### Use a shared folder

Each shared folder appears under `/mnt/mac/folders` with the name it was shared under. The folder's files and directories are those on the Mac, and the usual tools work on them:

```sh
ls /mnt/mac/folders
cat /mnt/mac/folders/work/notes.txt
echo 'a line from the guest' > /mnt/mac/folders/work/new.txt
mkdir /mnt/mac/folders/work/drafts
mv /mnt/mac/folders/work/new.txt /mnt/mac/folders/work/drafts/
rm /mnt/mac/folders/work/drafts/new.txt
rmdir /mnt/mac/folders/work/drafts
```

`work` and `notes.txt` in these commands are illustrative names. Each change is made on the Mac before the command that made it returns, and programs on the Mac see it immediately. A file created from the guest belongs on the Mac to the Mac user who runs the app.

`mv` within one shared folder is a rename on the Mac. Between two shared folders, and between a shared folder and the guest's own files, `mv` copies the file or directory, keeping its modification time and the permissions the guest sees, and then removes the original, as a move between two disks does. The guest sees only a Mac file's owner bits, so a file of mode `0644` moved between two shared folders arrives as `0600`:

```sh
mv /tmp/report.txt /mnt/mac/folders/work/report.txt
mv /mnt/mac/folders/work/drafts /tmp/drafts
```

`/tmp/report.txt` and `drafts` are illustrative names. `mv` makes the copy under a new name beside the destination, beginning with `.mv-`, and writes each file to the disk before it closes it. It then renames the copy over the destination, which replaces the destination in one step: a program on the Mac sees the old destination or the whole copy, never part of one. Once that rename is on the disk, `mv` removes the original, so for a moment both exist. A destination that is a directory with something in it is refused before anything is copied.

If the copy fails, `mv` removes its copy and leaves the original and the destination as they were; if even its copy cannot be removed, `mv` names the `.mv-` name it left. `mv` removes only what it copied, and a file only as it was when copied. A file that appeared in the original while the copy was being made, or that a program changed after `mv` copied it, is left where it is, and `mv` names it and fails:

```text
mv: '/mnt/mac/folders/work/drafts/late.txt' changed or appeared after the copy; left in place
```

`late.txt` is an illustrative name. If the original cannot be removed, the copy remains, and so does whatever of the original could not be removed; `mv` says what and fails. If the rename cannot be written to the disk, `mv` keeps the original too, and says so.

Two shared folders can overlap, one inside the other, and a file reached through both is one file: moving or copying it from one of its names to the other fails with `are the same file`, and the file is unchanged. A device, socket or other special file is not moved this way: `mv` says so. A symbolic link cannot be moved across a boundary, because Thylacine's own programs cannot create one yet: moving a link, or a directory that holds one, fails with `creating a link is not supported`, and the original is left whole.

Only regular files and directories can be opened. A Mac device, socket or named pipe in a shared folder is listed, but opening it fails with `permission denied`. A Mac file that the Mac user cannot read cannot be read from the guest either.

### Read-only folders

A folder shared read-only refuses every change. Its files and directories show no write permission (see the next section), so the guest itself refuses creating, writing, truncating, renaming or removing a file and making a directory, before the Mac is asked. A tool reports `permission denied`:

```sh
rm /mnt/mac/folders/docs/readme.txt
```

```text
rm: /mnt/mac/folders/docs/readme.txt: permission denied
```

A refused redirection prints nothing; it sets `$status` to 1 and `$errstr` to a message ending in `PermissionDenied`.

A file's owner may change its mode, so `chmod` is passed to the Mac, which refuses it:

```text
chmod: /mnt/mac/folders/docs/readme.txt: read-only file system
```

The folder on the Mac is unchanged.

Moving a file out of a read-only folder into the guest's own files copies it and then fails to remove the original:

```text
mv: cannot remove '/mnt/mac/folders/docs/readme.txt': permission denied
```

The copy stays where `mv` put it, and the folder on the Mac is unchanged. Moving it into another shared folder is refused before anything is copied, with `permission denied`, because the two folders are one session and the guest checks the read-only folder's modes first.

### Ownership and permissions

Every file under `/mnt/mac` is reported as owned by the session's user, whatever its owner on the Mac, and its group number is the user's own number:

```sh
stat /mnt/mac/folders/work/notes.txt
id
```

The `Uid` that `stat` prints is the `uid` that `id` prints, and the `Gid` is that same number, whatever the user's primary group. The owner bits of the mode are the Mac file's owner bits, with write removed in a read-only folder, and the group and other bits are always zero: a Mac file of mode `0644` reads `0600`, and in a read-only folder `0400`. `chmod` sets the Mac file's mode as given, and the guest goes on reading only its owner bits. A change of owner to anyone other than the session's user, or of group to any number other than the user's, fails with `permission denied`.

### Links in a shared folder

A symbolic link in a shared folder is followed within `/mnt/mac`. A relative target is resolved from the directory that holds the link. An absolute target is resolved from `/mnt/mac`, so a link whose target is `/folders/work/notes.txt` names `/mnt/mac/folders/work/notes.txt`. A link that the Mac uses to reach a file outside the shared folder, such as `config -> /Users/me/.config/app`, names `/mnt/mac/Users/me/.config/app` in the guest, and a command that follows it fails as for a missing file. To make such a file available, move it into a shared folder or share its folder.

`rm` removes a link itself, never what it points to, and a link to a directory needs no `-r`. Written with a trailing slash, as in `link/`, the name means the directory the link points to, as POSIX has it: `rm -r link/` removes what that directory holds and then fails, because the link is not a directory.

The Mac app never follows a link on the Mac. If the Mac replaces a directory that a program is using with a link, nothing outside the shared folder is read through it. A working directory is kept as a name, so the next lookup beside it walks down from the root again, meets the link, and follows it within `/mnt/mac` as any link there is followed. A link to a Mac folder outside the share then names nothing, and the lookup fails with `no such file or namespace entry`.

### When the Mac app goes away

If the Mac app quits or loses its connection, every open file and directory under `/mnt/mac` fails with `I/O error`, and the session's mount stays unusable until the session ends. Programs that do not use `/mnt/mac` are unaffected. When the Mac app connects again, a new session has the tree.

### Limits

| Limit | Value |
|---|---|
| Text through `snarf` | 16 MiB |
| Connections to the Mac at once | 32 (each session's mount is one) |
| Shared folders | 126 |
| Name of a file or directory | 255 bytes |

## Technical Details

### The line to the Mac

The Mac app runs QEMU with a virtio console device whose port is named `org.thylacine.mac`. In the guest, the `virtio-serial` driver binds that device and posts the service `/srv/mac`. Each guest connection to `/srv/mac` becomes one channel of the port's byte stream, and the driver relays the channel's bytes to the Mac app and back without reading them. The Mac app serves each channel as its own 9P2000.L session. The channel framing, `mac1`, is built from one source by both the driver and the app.

A connection is made by opening `/srv/mac`. The kernel then attaches a 9P session with the opener's user id, and that id is the owner every file of the session reports. While the Mac app is not on the line, the driver refuses a connection at once, so the opener's attach fails with `I/O error` without waiting for the kernel's handshake deadline.

### The session's mount

Login starts every session program through `newns`, which opens `/srv/mac` as the session's user and mounts the result at `/mnt/mac` in the session's own namespace before it runs the program in its place. Only processes of that session see the mount. If `/srv/mac` does not exist, `newns` mounts nothing and prints nothing; if the connection fails, it prints one line and the session starts without the tree.

### Ownership and the Mac user

The Mac app performs every operation on the Mac as the user who runs it, and reports each file to the guest as owned by the session's user, with that user's number as its group as well. The session's user therefore has the owner's access in Thylacine to any file the Mac user can reach in a shared folder, and the Mac's own permissions still apply to what the Mac app can do. Group and other bits are reported as zero because the Mac's groups have no meaning in the guest.

### How links resolve

`/srv/mac` is posted as a remote service, the declaration Haul's posts carry, because the line's data leaves the guest. A link served by a remote session is resolved by the kernel within the mount it was reached through, which for `/mnt/mac` is the session's mount. The Mac app reports a link as a link and never resolves one on the Mac: each lookup and open on the Mac takes one name at a time from the shared folder's root and refuses to pass through a link. A lookup the kernel makes from a directory it already holds, such as one a program took as a path handle before the Mac replaced it with a link, is therefore refused by the Mac app itself, with `too many levels of symbolic links`. A working directory holds no such reference: it is a name, resolved again from the root for each lookup.

### Moves across devices

A rename is one operation within one 9P session. The guest's own files are served by other sessions than `/mnt/mac`, so the kernel refuses a rename between them and `/mnt/mac` with `invalid cross-device link` before it asks either side. The Mac app refuses a rename between two shared folders with the same error, because they are separate folders on the Mac. `mv` answers that error by copying and then removing, as POSIX `mv` does.

`mv` walks the original by directory handles. It opens each entry from the handle of the directory it was listed in, without following a link, and removes it by name through that same handle, so a link that a Mac program puts in place of a directory while `mv` works cannot lead the removal outside the original. `rm -r` removes a tree the same way, and names each entry it cannot remove and goes on with the rest. The copy opens each file and directory without following a link and checks that it is the entry it found, so a link put in place during the copy is refused, not followed. `mv` checks the original's identity when the copy begins and again when it removes it. If the original was replaced before the copy began, `mv` removes its copy, says the original changed, and leaves both names as they were; if it was replaced later, the copy is in place, and `mv` leaves the new original, names it, and fails. Before the rename, `mv` checks that the `.mv-` name still holds its copy. A file's removal also compares the file's version, which the Mac app moves on every change to its content, size, mode or owner, reading the file's change time to the nanosecond.

`/srv/mac` and every other posted service read as regular files until they are opened, so `cp` and `mv` can tell that one is special only by opening it: copying or moving a service connects to it once, and the connection is closed at once. A write to the disk that `mv` waits for is a sync, and the Mac app commits each sync past the drive's own cache, so a copy in a shared folder that `mv` has synced survives the Mac losing power. On a disk that cannot commit past its cache, such as a network share, the Mac app makes an ordinary sync instead, and the drive may still hold the data when the power goes.

### Errors

The Mac app reports errors in the Linux numbering that 9P2000.L uses, translated from the Mac's (for example, the Mac's code for a non-empty directory is 66, and the guest receives 39, `directory not empty`). A Mac error with no Linux counterpart reaches the guest as `I/O error`. Thylacine's tools name the common codes, among them 30, `read-only file system`, 27, `file too large`, and 18, `invalid cross-device link`; others are printed as `kernel error (errno N)`.

### Failure and cleanup

When the Mac app goes away, the driver closes every channel: each connection's next operation fails, and the mounts made over them stay failed. When the Mac app returns, the driver starts a fresh line, and new connections work; mounts made before the departure do not recover. A lock that a guest program holds on a file in a shared folder is released when the program closes the file or its connection ends; Mac programs do not see these locks, and the Mac app does not see locks held by Mac programs.
