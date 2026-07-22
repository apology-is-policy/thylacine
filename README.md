<p align="center"><img src="thylacine-logo.svg" alt="Thylacine OS logo" width="33%"></p>

# Thylacine OS

Thylacine is a non-POSIX operating system targeting ARM64/ARMv8 that revolves around one central thesis: __Plan 9 was right:__

- Everything is a file
- 9P is the universal protocol
  - Thylacine squeezes every last nanosecond of performance out of it, since it's really easy to completely botch it up, as Microsoft has shown us with its absurdly slow WSL2 interop FS bridge
- Per-process namespaces are the superior isolation primitive
  - Plan 9 had it in the 80s, and decades later the immensely popular Docker reimplements the same concept. Ken Thompson et al. were four decades ahead of their time.
- The kernel is a monolithic core with a deliberately minimal interface (one mechanism: 9P), and drivers are userspace programs
  - Driver faults don't take the entire kernel down with them

On top of that it adds its own convictions:

- SOTA kernel components, formally modeled in TLA+
  - Memory management, work scheduling, transport, security model, etc., all have their formal models
- Borrow universally loved mechanisms from mainstream OSs, but stay true to our Plan 9 heritage
  - Most notably Linux, e.g. 9P2000.L, and io_uring as an inspiration for Thylacine's _Loom_
- Expand on the "everything is a file"
  - Everything is a filesystem
    - Display, network, disk -- synthetic filesystems backed by 9P-speaking daemons (the console by an in-kernel device), all accessible via the kernel 9P device and grantable per-process
- We want to be usable -- transparent POSIX compatibility layer
  - Recompiled POSIX/Linux software just runs, its POSIX surface translated to Thylacine syscalls in userspace; static Linux binaries run best-effort
- Native Go port with a flagship TUI programming and debugging experience
  - Natively symbolized stack traces all the way to the kernel depths
  - What kind of Plan 9-heritage OS would it be without Ken Thompson's language as its primary user-facing toolchain? (Also, porting Rust's compiler on-device is nowhere near as easy -- though our native userspace is Rust.)
- A rich, media-capable, tabs-and-panes-based text terminal is the only UI
  - The concept of a desktop and windows has failed -- from all of the possible windowing workflows, only one now remains, which is people having a couple of windows docked to various parts of their desktops, aligned to one another without overlap, filling their entire display -- this is a terminal, and windows function as panes. All operating systems are moving closer to fully embracing this workflow, but Thylacine does not have any desktop or windowing concept from the beginning.
- A new made-to-measure (but portable) COW filesystem -- Stratum.
  - Compiles anywhere, 9P native
  - Runs as a userspace driver in Thylacine, executes via the POSIX compatibility layer ("Pouch"), and rides the 9P kernel device -- yet reaches performance competitive with the host (tested on a big Go build directly in Thylacine)
  - Post-quantum cryptography and Merkle validation

### Why the name "Thylacine"

The Thylacine (abstractly depicted in the logo above) is an extinct (though I believe that they still roam hidden corners of Tasmania somewhere in low numbers) marsupial, also known as the Tasmanian Tiger, that my wife introduced me to -- she has a special relationship with it that she infected me with. It is a special kind of loneliness when you're the last specimen of your entire species. Calling out into the night, waiting for an answer that can never come. It stirs an unnamed, brooding emotion in us. Naming my ultimate software project after it is my little nod. The Thylacine runs free in the great NAND plains.

## Latest

### Running Quake!

A Pouch port of TyrQuake:
- Almost effortless
- Just works

https://github.com/user-attachments/assets/6ecc696a-7140-485a-b59b-58c945c121a5






