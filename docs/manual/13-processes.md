# Processes and memory

Thylacine reports its processes and their memory through three text files and
two commands. `/ctl/procs` lists every process in one snapshot, one line each.
`/proc/<pid>/status` describes one process, including its memory footprint.
`/ctl/memory` reports the machine's pages and the user pool. `ps` prints the
process list as a table, and `prowl` shows the processes, the CPUs and the pool
live and lets the operator act on a process. `/proc` can also be listed like any
directory: it holds one directory per process, and each process's directory
holds a `thread` directory with one directory per thread, where a single thread
of a program that runs many can be examined.

Memory is counted in pages of 4 KiB. A process's charge is the sum of the
anonymous pages it holds, the file pages it maps, the index pages of its
page map, and its page tables. The user pool bounds the charge of every user
process together. A process whose charge would take the pool past its size
receives an allocation failure, and the kernel's own memory is never at risk.

## In Practice

### List the processes

Run `ps`. It prints one row per process:

```text
PID  PPID  NAME  STATE  THR  PAGES  TBL  KIDS  CPU
```

`PAGES` is the process's charge and `TBL` the page tables inside that charge.
`KIDS` counts its live children. `CPU` is the time it has spent on a CPU, in
milliseconds under ten seconds and in seconds above. Another user's process
shows `-` under `CPU`, unless the operator holds `CAP_HOSTOWNER`. `STATE` is `ALIVE`,
`STOPPED`, `ZOMBIE` or `INVALID`. To pass the kernel's own text through for a
script, with `CPU_NS` in nanoseconds, run `ps --color=never --beacon=never`.

### Watch the system live

Run `prowl`. The first row names the process count and the CPU count; the second
row shows one bar per CPU; the third row is the memory meter, filled to the
pool's charge, with the charged and pool page counts and the free physical pages
beside it. The table below lists the processes with their CPU share, their
charge under `MEM(pg)`, their page tables under `TBL`, their threads and their
state. An operator who holds neither `CAP_HOSTOWNER` nor the system's own
identity sees each CPU bar drawn in dashes and another user's CPU share as `-`;
the operator's own processes show their share as usual.

The arrow keys, Page Up, Page Down, Home and End move the cursor. `d` opens and
closes the detail pane. `t` switches between the flat list and the process tree.
`s` cycles the sort through CPU, process ID, memory and name. `r` or the space
bar refreshes. `z` stops the selected process and `c` continues it. `k` kills it
after a confirmation, and Escape cancels that confirmation. Otherwise `q` or
Escape quits, and anything typed after it goes to the shell.

The detail pane's first line is the selected process's footprint: its charge,
the page tables and file pages inside it, its peak charge and its budget. Below
it, one row per thread shows the scheduler's view of that process. The thread
rows appear for the operator's own processes, and for every process when the
operator holds `CAP_HOSTOWNER`; otherwise the pane reports that view as
unavailable. The footprint line is shown for every process.

### Read one process's footprint

Read the process's status file:

```sh
cat /proc/<pid>/status
```

It reports `name`, `pid`, `state`, `threads`, `threadcap` (the most threads the
process may run at once), `cpu_ns`, `ppid`, the owning `principal` and `gid`,
then the memory lines: `pages` (the charge), `tables`,
`file`, `children`, `peak` and `budget`. A zombie reports its final figures and
an `exit` line with its exit status, and a process that died of a fault or a
note adds a `signal` line with the number of the signal it died of. Every
process's status file is readable by everyone, but `cpu_ns` reads `-` to anyone
other than the process's owner and the holders of `CAP_HOSTOWNER`.

### Browse /proc

```sh
ls /proc
ls /proc/<pid>
```

`ls /proc` lists one directory per process, named by its process ID, in
increasing order. An operator running as `none` sees only its own process.
`ls /proc/<pid>` lists the process's files, `status`, `ctl`, `maps` and the
rest, and its `thread` directory.

### Look at one thread

To find out where a thread of a program is waiting, list the program's threads
and read that thread's kernel stack:

```sh
ls /proc/<pid>/thread
cat /proc/<pid>/thread/<tid>/kstack
```

The listing names each live thread by its thread ID, in increasing order. The
main thread's ID is the process ID. `kstack` prints where the thread is inside
the kernel, one line per frame with the innermost first: each line names a
function and the offset into it, such as `#0  sched+0x1c4`, so a thread blocked
in a system call shows the path from that call down to the wait. A thread that
is on a CPU at the moment of the read prints `#0  <running>`; read it again to
catch it waiting. The process need not be stopped, and the read does not
disturb it. A holder of `CAP_DEBUG` or `CAP_HOSTOWNER` also sees each frame's
address.

The thread listing is open to the process's owner, to holders of
`CAP_HOSTOWNER`, and to a holder of `CAP_DEBUG` for any process it could debug,
which excludes a process that has made itself untraceable. Anyone else is
refused with `permission denied`, and a thread directory they name answers
`no such file or namespace entry`, as a thread that does not exist answers
everyone. Reading `kstack` needs the authority to debug the process: its owner,
while holding every capability the process holds, or a holder of `CAP_DEBUG` or
`CAP_HOSTOWNER`. No one may read the stack of a process that has made itself
untraceable, such as the login session. A thread that exits leaves the listing,
and a `kstack` opened before it exited then fails with `I/O error`.

Each thread directory also holds `regs`, `fpregs` and `kregs`, the thread's
registers. They answer only while a debugger holds the process stopped, and are
described under Technical Details.

### Read the pool

```sh
cat /ctl/memory
```

`total` is the machine's pages. `free` is the pages the physical allocator holds
now. `reserved` is the pages taken before the allocator opened, for the kernel's
image and boot structures. `reserve` is the share kept for the kernel and its
services. `pool` is the rest, the pages every user process may hold together.
`charged` is what they hold now. The headroom is `pool` minus `charged`.

### Judge the headroom

A process's `budget` defaults to the pool, so the pool's headroom is the limit
for every process at once. When a process's charge would take the pool past its size, the
kernel first reclaims the pages of cached file images that no process maps and
then refuses: a program that asked for memory receives `ENOMEM`, and a program
that touched a page it had reserved is terminated with a fault note. The kernel
and its services are charged but never refused.

A native program reserves its heap as address space and is charged for each page
when it first touches that page, so at exhaustion a native program is usually
terminated at a page rather than refused. The process terminated is the one
whose touch found the pool full, which need not be the process that filled it,
because the kernel does not choose which process to end. Its parent sees exit
status 1, and the console line that reports the fault gives its process ID.

## Technical Details

The pool is the machine's memory less the reserve. The reserve is an eighth of
the memory, never less than 256 MiB and never more than half of it. A machine
with 2 GiB has a pool of 458752 pages and a reserve of 65536 pages.

Every process's budget defaults to the whole pool. Processes of the kernel's own
principal are charged but never refused. Before refusing an allocation the pool
reclaims the pages of cached file images that no process maps.

Each thread costs the pool 9 pages (36 KiB) for its kernel stack and its record,
charged when the thread is created and returned when it is freed. A process may
run 4096 threads at once (its `threadcap`), or as many as the pool can hold on a
machine where that is fewer, and a child inherits its parent's limit. A thread
past the limit is refused with `EAGAIN`, and one the pool cannot hold with
`ENOMEM`. Processes of the kernel's own principal have no thread limit.

Page tables are charged to the address space when they are installed and
reclaimed when they empty. A file page is charged to each process that maps it.
The page map's index pages are charged with the pages they index. `peak` only
rises, and a zombie reports the peak of its whole life.

A program's main stack is 8 MiB, reserved when the program starts and committed
a page at a time as it is touched; the `stack` row of `/proc/<pid>/maps` shows
it. A thread's stack carries a guard below it that no write can reach, so an
overflow ends the process instead of corrupting memory. Memory a program gives
back -- an allocation the C library returns to the system, a range the program
says it no longer needs -- leaves its `pages` figure at once. A native
program's heap gives memory back in two ways: a block of 256 KiB or more has a
reservation of its own and leaves when it is freed, and smaller freed blocks are
returned once the free space at the top of the heap passes 2 MiB. Freed small
blocks that lie below a block still in use stay charged, and the program's later
allocations reuse them.

`/ctl/procs` is one snapshot taken under the process-table lock, and it stops
when its buffer of 4 KiB fills, at some fifty to sixty processes; `ps` and
`prowl` show what they received. `/ctl/procs`, `/ctl/memory` and `/proc/<pid>/status` are readable by
every process. `/proc/<pid>/sched` is readable by the process's owner and by
holders of `CAP_HOSTOWNER`.

A listing of `/proc` is generated from the process table as it is read, in
increasing ID order, with no snapshot kept between reads: each read continues
from the last ID it returned. A process or thread created during a long listing
appears if its ID is above that point, and one that ends is not listed again.
Process and thread IDs are drawn from one sequence and never reused, so a name,
once listed, never comes to name a different process or thread. The one
exception keeps the name with its process: when a thread other than the main
thread runs a new program, it becomes the main thread, takes the process ID as
its thread ID, and its old thread ID is retired.

The process-level debugging files `regs`, `fpregs` and `kregs` describe one
thread: the thread whose breakpoint or step stopped the process, else the main
thread. The files of the same names under `/proc/<pid>/thread/<tid>/` have the
same formats and name the thread outright, so a debugger can read every thread
of a stopped program, such as each thread a Go program runs its goroutines on.
`regs` holds the thread's saved user registers, `fpregs` its floating-point and
vector registers, and `kregs` its kernel-side registers. They answer only while
every thread of the process is stopped by its debugger, `regs` and `fpregs`
accept writes then, and each needs the authority to debug the process. `kstack`
needs no stop because it reads only a thread that is off the CPUs, waiting, so
its stack is not being changed under the read.

A debugger steps one thread by writing `step <tid>` to the process's `ctl`;
`step` alone steps the thread that stopped the process, else the main thread.
The stepped thread runs one instruction, the process stops again, and the
process-level files then describe that thread. The process's other threads run
as well until it stops; only the named thread is held to one instruction. A
breakpoint at the stepped thread's position is lifted for that thread alone, so
no other thread can run past it unseen. There is no per-thread `ctl`. The open `ctl` file of the
debugger that attached is what owns the debugging session, and a second control
file could not show that it belongs to the same debugger, so stopping and
starting apply to the whole process and a single thread is chosen only by the
`step` operand.

The thread listing and the walk to a thread directory share a gate because the
set of thread IDs is what the gate protects: a walk that answered for any ID
would disclose the set one guess at a time. The gate is the one on
`/proc/<pid>/sched`, which already lists every thread, widened to debuggers, so
a holder of `CAP_DEBUG` can reach the files it is entitled to read. Passing the
gate grants only the names; each file keeps its own check.

A process's CPU time is shown only to its owner and to holders of
`CAP_HOSTOWNER`; other readers see `-` in `/ctl/procs` and in the status file.
The counters in `/ctl/cpu` and `/ctl/sched` that move whenever any process runs
(each CPU's idle time, context switches and interrupts, the runnable count and
the park counts) are shown only to the system's own processes and to holders of
`CAP_HOSTOWNER`. The CPU count, the capacity, cache-line and processor-ID
columns and the count of processes created are shown to everyone. The counters
are withheld because each one moves with every key typed at a password prompt:
a reader that watched them could learn a password's length and the rhythm of
its keys. A Linux program in a vivarium that reads `/proc/stat` finds every
CPU time as zero and no interrupt or context-switch totals.

A program built with the C library can load a shared library while it runs.
The library loader is `/lib/libc.so`, and a library named without a directory
is looked for in the directories the program names and then in `/lib`. The
loader does not read `LD_LIBRARY_PATH` or `LD_PRELOAD`, so neither variable
changes which library a program loads. On Linux the loader ignores these two
variables only when a program gains privilege as it starts. A Thylacine
process gains authority after it starts, when it asks for elevation, and by
then a library chosen through the environment would already be running inside
it. A program linked statically has no loader, and it does read the locale,
message-catalogue and time-zone variables (`MUSL_LOCPATH`, `NLSPATH`, a file
named by `TZ`). A dynamically loaded program ignores those as well, because
the C library applies one setting to all of them.
