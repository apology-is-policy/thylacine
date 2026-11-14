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

The pasteboard is set when the file is closed, from everything written since it was opened; nothing written empties it. Text that is not valid UTF-8 is set with each invalid sequence replaced by U+FFFD. A read takes the pasteboard's text when it starts at the beginning of the file, and later reads of the same open file continue that text, even if the Mac's pasteboard changes meanwhile. The file cannot be opened for reading and writing at once; such an open fails with `permission denied`. Its size reads 0 and its mode `0600`.

Only plain text is carried. A pasteboard that holds no text, such as one holding only an image, reads as empty. Text larger than 16 MiB can be neither read nor written through `snarf`.

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

`work` and `notes.txt` in these commands are illustrative names. Each change is made on the Mac before the command that made it returns, and programs on the Mac see it immediately. A file created from the guest belongs on the Mac to the Mac user who runs the app. `mv` within one shared folder is a rename on the Mac; between two shared folders, it fails (`kernel error (errno 18)`).

Only regular files and directories can be opened. A Mac device, socket or named pipe in a shared folder is listed, but opening it fails. A Mac file that the Mac user cannot read cannot be read from the guest either.

### Read-only folders

A folder shared read-only refuses every change: creating, writing, truncating, renaming or removing a file, making a directory, and changing a mode or time. A tool reports the refusal as `kernel error (errno 30)`, the Linux code for a read-only file system:

```sh
rm /mnt/mac/folders/docs/readme.txt
```

```text
rm: /mnt/mac/folders/docs/readme.txt: kernel error (errno 30)
```

A refused redirection prints nothing; it sets `$status` to 1 and `$errstr` to a message ending in `Other(30)`. The folder on the Mac is unchanged.

### Ownership and permissions

Every file under `/mnt/mac` is reported as owned by the session's user and that user's primary group, whatever its owner on the Mac:

```sh
stat /mnt/mac/folders/work/notes.txt
id
```

The `Uid` and `Gid` that `stat` prints are the `uid` and `gid` that `id` prints. The owner bits of the mode are the Mac file's owner bits, with write removed in a read-only folder, and the group and other bits are always zero: a Mac file of mode `0644` reads `0600`, and in a read-only folder `0400`. `chmod` changes the Mac file's mode. A change of owner or group to anyone other than the session's user is refused.

### Links in a shared folder

A symbolic link in a shared folder is followed within `/mnt/mac`. A relative target is resolved from the directory that holds the link. An absolute target is resolved from `/mnt/mac`, so a link whose target is `/folders/work/notes.txt` names `/mnt/mac/folders/work/notes.txt`. A link that the Mac uses to reach a file outside the shared folder, such as `config -> /Users/me/.config/app`, names `/mnt/mac/Users/me/.config/app` in the guest, and a command that follows it fails as for a missing file. To make such a file available, move it into a shared folder or share its folder.

The Mac app never follows a link on the Mac. If a directory that a program has entered is replaced on the Mac by a link, the next lookup in that directory fails with `too many levels of symbolic links`, and nothing outside the shared folder is read.

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

The Mac app performs every operation on the Mac as the user who runs it, and reports each file to the guest as owned by the session's user. The session's user therefore has the owner's access in Thylacine to any file the Mac user can reach in a shared folder, and the Mac's own permissions still apply to what the Mac app can do. Group and other bits are reported as zero because the Mac's groups have no meaning in the guest.

### How links resolve

`/srv/mac` is posted as a remote service, the declaration Haul's posts carry, because the line's data leaves the guest. A link served by a remote session is resolved by the kernel within the mount it was reached through, which for `/mnt/mac` is the session's mount. The Mac app reports a link as a link and never resolves one on the Mac: each lookup and open on the Mac takes one name at a time from the shared folder's root and refuses to pass through a link. A link that appears on the Mac under a directory the guest has entered is therefore refused by the Mac app itself.

### Errors

The Mac app reports errors in the Linux numbering that 9P2000.L uses, translated from the Mac's (for example, the Mac's code for a non-empty directory is 66, and the guest receives 39, `directory not empty`). A Mac error with no Linux counterpart reaches the guest as `I/O error`. Thylacine's tools name the common codes; others, such as 30 for a read-only file system and 18 for a move across folders, are printed by number.

### Failure and cleanup

When the Mac app goes away, the driver closes every channel: each connection's next operation fails, and the mounts made over them stay failed. When the Mac app returns, the driver starts a fresh line, and new connections work; mounts made before the departure do not recover. A lock that a guest program holds on a file in a shared folder is released when the program closes the file or its connection ends; Mac programs do not see these locks, and the Mac app does not see locks held by Mac programs.
