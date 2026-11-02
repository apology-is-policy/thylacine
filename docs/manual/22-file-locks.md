# File Locks

A file lock marks a range of bytes in a file as held for reading or for writing, so that cooperating programs can take turns with a file they share. Any number of programs can hold read locks on the same bytes at once, while a write lock on those bytes excludes every other lock. Locks are advisory: a lock stops another program's request for a conflicting lock and has no effect on that program's reads and writes, so every program that shares the file must ask for a lock before it touches the data. Database libraries depend on this behaviour, and so does any program that updates a file in place while other programs may read it.

Programs take locks through a call; the system provides no shell command for it. Programs built with Pouch use the POSIX `fcntl` lock commands and `flock`, and native programs use the kernel call `SYS_FILE_LOCK` directly. Linux programs in a container cannot lock files yet, and receive `ENOSYS`. Only a file held by a 9P file service can be locked, which includes the system's own files; a file on a network mount can be locked if its server serves locks. Other files, such as kernel devices and pipes, refuse a lock with `ENOLCK`, and so does a server that does not serve them.

Each lock belongs to an owner, and the choice of owner decides who shares the lock and what releases it. The owner is either the process or the open file. The POSIX commands `F_SETLK` and `F_SETLKW` make the process the owner, while `F_OFD_SETLK`, `F_OFD_SETLKW` and `flock` make the open file the owner. A native program chooses with a flag, and uses the open file by default. New programs should use the open file as the owner, because the process owner gives up all of its locks on a file whenever the process closes any descriptor of that file, including a descriptor that a library opened for its own purposes.

## In Practice

### Hold a lock while updating a file

A program built with Pouch can update a file under a write lock as follows. The descriptor must be open for writing to take a write lock, and open for reading to take a read lock; otherwise the request fails with `EBADF`.

```c
struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
int fd = open("counter", O_RDWR);
if (fd < 0 || fcntl(fd, F_OFD_SETLKW, &fl) != 0) {
    perror("counter");
    exit(1);
}
/* read, change and write the file */
close(fd);
```

`l_start` and `l_len` are both zero here, which selects every byte of the file, including bytes added later. `F_OFD_SETLKW` waits until the lock is granted. Closing the descriptor releases the lock before `close` returns, because this descriptor is the only one that refers to the open file; a descriptor copied with `dup` or inherited by a child refers to the same open file and keeps the lock held until it is closed as well. The data the program wrote reaches the file service before the lock is released, so the next holder reads it.

To release a lock while keeping the file open, repeat the call with `l_type` set to `F_UNLCK` and the same range. Unlocking part of a locked range leaves the rest of it locked.

### Try for a lock without waiting

`F_SETLK` and `F_OFD_SETLK` answer at once. A conflict makes the call fail with `EAGAIN`, and the program can do other work and try again later. The same request with `flock` is `flock(fd, LOCK_EX | LOCK_NB)`, which fails with `EWOULDBLOCK`, the same value as `EAGAIN`.

### Find out whether a range is free

`F_GETLK` and `F_OFD_GETLK` report whether a lock could be taken, without taking it. The program fills in a `struct flock` describing the lock it would ask for, and the descriptor needs no particular access mode. When nothing conflicts, the call sets `l_type` to `F_UNLCK` and leaves every other field unchanged. When a lock conflicts, the call stores its type and range, with `l_whence` set to `SEEK_SET` and `l_len` zero if the range runs to the end of the file. `l_pid` is always -1, because the file service does not record a process for a lock. Another program can change the answer before the caller acts on it, so the answer is useful for reporting and for diagnosis, and a program that needs the lock should ask for the lock itself.

### Lock a whole file with flock

`flock(fd, LOCK_SH)` takes a read lock on the whole file, `flock(fd, LOCK_EX)` takes a write lock, and `flock(fd, LOCK_UN)` releases the lock. The open file owns the lock, and the call waits unless `LOCK_NB` is added. `LOCK_EX` requires a descriptor open for writing, as it does on a Linux NFS client; on a descriptor opened read-only it fails with `EBADF`. Locks taken with `flock` conflict with locks taken with `fcntl`, which also differs from local filesystems on Linux, where the two kinds do not interact.

### Choose between the two owners

| Question | The process owns the lock | The open file owns the lock |
| --- | --- | --- |
| Which calls take it | `F_SETLK`, `F_SETLKW` | `F_OFD_SETLK`, `F_OFD_SETLKW`, `flock` |
| Does it conflict with a second open of the file in the same process | No | Yes |
| Is it shared by the process's threads | Yes | Yes, through the descriptor |
| Does a child started with `fork` share it | No; the child holds none of the parent's locks | Yes, through the inherited descriptor |
| Does it survive `exec` | Yes, unless close-on-exec closes a descriptor of the file | Yes, while the descriptor stays open |
| What releases it | An unlock, the process's closing any descriptor of the file, or the process's exit | An unlock, or the last close of the open file |

A descriptor opened with `O_PATH` does not count as a descriptor of the file here, so closing one releases nothing. `chmod` and `utimensat` given a path use such a descriptor internally, and also release nothing.

A process can lock at most 128 different files as a process owner. A file keeps its place in that count after an unlock, until the process closes a descriptor of the file or exits. Stratum, the file service that holds the system's own files, limits every owner to 256 separate locked ranges and a file system to 65,536 ranges in all, and refuses further requests with `ENOLCK`. Adjacent ranges of the same type count as one.

### Lock a file from a native program

A native program written in Rust calls `t_file_lock` in `libthyla-rs`, and a native program written in C calls the same function in `libt`. The call takes a descriptor, an operation word, a starting offset and a length (zero to run to the end of the file), and the address of a `t_file_lock` structure for a test. The operation word holds a type, `T_FILE_LOCK_RD`, `T_FILE_LOCK_WR` or `T_FILE_LOCK_UN`, combined with any of three flags. `T_FILE_LOCK_WAIT` waits for the lock, `T_FILE_LOCK_TEST` asks the question that `F_GETLK` asks, and `T_FILE_LOCK_PROC` makes the process the owner.

### Errors

| Error | Native message | Cause and response |
| --- | --- | --- |
| `EAGAIN` | `resource temporarily unavailable` | Another owner holds a conflicting lock and the call was not asked to wait. Try again later. |
| `EBADF` | `bad handle` | The descriptor is closed, opened for walking only, or opened without the access the lock type requires. A process-owner lock also receives this error when another thread closed the descriptor while the call was waiting; the lock has been released again. |
| `EINVAL` | `invalid argument` | `l_whence`, `l_type` or a `flock` operation is not valid, the range starts before the beginning of the file, or an `F_OFD_` request has a non-zero `l_pid`. A native `SYS_FILE_LOCK` call whose range would reach 2^64 also receives it. |
| `EFAULT` | `bad address` | A native `SYS_FILE_LOCK` test names an answer address that cannot be written. The service is not asked if the address is outside the process's half of memory. |
| `EOVERFLOW` | not applicable | The range ends past the largest offset a signed 64-bit number can hold, or `F_GETLK` cannot represent the conflicting lock. |
| `ENOLCK` | `no locks available` | The file's service does not support locks, a limit described above has been reached, or the service refused a request. A process-owner lock on a 9P server that does not support cloning an open file's connection also receives it. |
| `ESTALE` | `stale file handle` | The file has been removed. On the system's own files, a read-only descriptor of a small file can also receive this error after the file has been renamed or replaced, as described in Technical Details. |
| `ENOSYS` | not applicable | The descriptor is a socket. |

## Technical Details

### Where locks are kept

The file service that holds a file keeps its locks, and the kernel keeps no table of them. A lock request travels to the service as the 9P2000.L message `Tlock`, and a test as `Tgetlock`. Stratum, the file service that holds the system's own files, records each lock against the fid, its name for one open connection to the file. A request from an owner that already holds part of the range replaces that part atomically, so a write lock can be converted to a read lock with no moment at which the owner holds nothing. An unlock trims or splits the owner's locks. When a fid is closed, the service drops every lock recorded against it, including when a process dies or its connection to the service ends.

An open file owner's locks are recorded against the open file's own fid. A process owner needs an identity that outlives any one descriptor, so the kernel creates one additional fid for each file the process locks, cloned from the descriptor's fid and stored with the process's descriptor table. Threads of the process share those fids, a child created by `fork` starts with none, and `exec` keeps them. When the process closes a descriptor of a file with such a fid, other than an `O_PATH` descriptor, the kernel sends the descriptor's staged writes to the service, unlocks the whole file, waits for the answer and closes the fid. Two descriptors count as the same file when they use the same 9P connection and the same file number and generation. A file reached through two separate mounts therefore counts as two files for this rule, although the service still applies its conflict rule to the locks of both.

Stratum can reuse the number of a removed file. The generation distinguishes the new file from the old one, so a descriptor of a removed file neither locks nor releases anything belonging to the file that took its number; a lock request through that descriptor fails with `ESTALE`.

### Waiting

The file service never holds a request open. It answers a conflict at once, and the kernel retries a waiting request after a delay that starts at 1 millisecond and doubles to a limit of 64 milliseconds. Waiters do not form a queue, so a process that asks at the moment a lock is released can be granted it ahead of one that has waited longer, and no order of grant is guaranteed. Killing a process ends its wait. A caught signal does not interrupt the wait for a program built with Pouch or a native program, so `alarm` cannot put a time limit on `F_SETLKW`; a program that needs a limit can use `F_SETLK` in a loop with its own delay.

Neither the service nor the kernel detects deadlock. Two owners that each wait for a lock the other holds wait until one of them is killed, and `EDEADLK` is never returned. Programs that take more than one lock should take them in a fixed order.

### Cached data and locks

The kernel caches file contents and attributes, and a program's recent writes can wait in the kernel before they reach the service. Taking or releasing a lock changes what the cache may assume, so that a lock holder always reads what the previous holder wrote. Taking a lock discards the kernel's cached pages and attributes for the file, so the next read asks the service. Every request that sets a lock or changes one, an unlock included, sends the owner's staged writes to the service first, and the request follows after they have been answered, so a reader admitted when a write lock is converted to a read lock sees what the holder wrote. A test sends nothing. If those writes fail, the request is still sent, and the error is reported by the next write, `fsync` or close of that open file. A close or an exit that releases a lock waits for the service's answer, so the next request finds the lock gone. When a process is killed while holding a lock, the session's closing thread sends the staged writes and then the release, in that order, and a parent's `wait` can return before the release is answered.

The staged writes sent are those of every descriptor of the file in the process, for either kind of owner. Taking either kind of lock also has every descriptor of the file in the process read from the service. A read-only descriptor another thread opens while the request is on its way can keep its copy. A descriptor elsewhere that has not asked for a lock keeps seeing what it would otherwise see, which close-to-open consistency describes. A private file map also keeps its view; POSIX leaves unspecified whether such a map shows later changes to the file, and shared file maps are not supported for programs built with Pouch.

### Read-only descriptors served from the cache

A descriptor opened for reading only, on a regular file of at most 128 KiB whose contents the kernel already holds, can be served from a copy the kernel takes when the file is opened. Taking a lock on the file through any descriptor in the process stops the copy from being used: from then on the descriptor reads from the service, and `fstat` reports the service's size. A test or an unlock leaves the copy in use.

On a mount that revalidates at each open, the descriptor keeps a fid from the moment it is opened, and locks and metadata changes go straight to that fid. The system's own files use a looser mount, on which such an open sends nothing to the service and binds no fid for the file. The first lock request, `chmod` or `futimens` on the descriptor walks the names the open used and accepts the result only when it reaches the same file number and generation. If the file was renamed away, removed or replaced since the open, the request fails with `ESTALE` and nothing is locked or changed, while reads through the descriptor keep returning the copy. Opening the file again gives a descriptor that can lock it. If the service fails the walk for any other reason, a lock request fails with `ENOLCK`, and `chmod` or `futimens` with the service's error. `chmod` and `futimens` check permission against the file's owner as the first such request finds it, not the owner when the descriptor was opened. A later request reads the kernel's cached attributes, as any descriptor's does, and sees a change made through another mount only when those are refreshed.

A second read-only descriptor of the same file in the process that can no longer reach the file in this way keeps serving its copy, and a lock taken through the first descriptor stands. If the second descriptor fails for any other reason, the lock request fails with `ENOLCK`.

### Limitations

Linux programs in a container receive `ENOSYS` from the `fcntl` lock commands, because Vivarium does not yet forward them. The `go` command's internal lock on its build cache and module cache has no effect on Thylacine, so two `go` commands running together are not excluded from each other by it. A lock waiter cannot be interrupted by a caught signal, as described under Waiting.
