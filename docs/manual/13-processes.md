# Processes and memory

Thylacine reports its processes and their memory through three text files and
two commands. `/ctl/procs` lists every process in one snapshot, one line each.
`/proc/<pid>/status` describes one process, including its memory footprint.
`/ctl/memory` reports the machine's pages and the user pool. `ps` prints the
process list as a table, and `prowl` shows the processes, the CPUs and the pool
live and lets the operator act on a process.

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
milliseconds under ten seconds and in seconds above. `STATE` is `ALIVE`,
`STOPPED`, `ZOMBIE` or `INVALID`. To pass the kernel's own text through for a
script, with `CPU_NS` in nanoseconds, run `ps --color=never --beacon=never`.

### Watch the system live

Run `prowl`. The first row names the process count and the CPU count; the second
row shows one bar per CPU; the third row is the memory meter, filled to the
pool's charge, with the charged and pool page counts and the free physical pages
beside it. The table below lists the processes with their CPU share, their
charge under `MEM(pg)`, their page tables under `TBL`, their threads and their
state.

The arrow keys, Page Up, Page Down, Home and End move the cursor. `d` opens and
closes the detail pane. `t` switches between the flat list and the process tree.
`s` cycles the sort through CPU, process ID, memory and name. `r` or the space
bar refreshes. `z` stops the selected process and `c` continues it. `k` kills it
after a confirmation, and Escape cancels that confirmation. Otherwise `q` or
Escape quits.

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

It reports `name`, `pid`, `state`, `threads`, `cpu_ns`, `ppid`, the owning
`principal` and `gid`, then the memory lines: `pages` (the charge), `tables`,
`file`, `children`, `peak` and `budget`. A zombie reports its final figures and
an `exit` line with its exit status. Every process's status file is readable by
everyone.

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
