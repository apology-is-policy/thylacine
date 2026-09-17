# DOSBox-X

DOSBox-X runs DOS programs on Thylacine's ARM64 system. The Cryptid port
provides a graphical SDL surface, keyboard and mouse input, audio through
Nocturne, and an x86-to-AArch64 dynamic core. Software Voodoo emulation supports
Glide applications without requiring a host 3dfx GPU.

## In Practice

### Start a bundled game

Images built with the game bundles contain read-only master directories.
Copy a game to your writable home before launching it:

```sh
cp -r /duke3d ~/duke3d
cd ~/duke3d
dosbox-x
```

Use `/tombraider` and `~/tombraider` in the same sequence for the Tomb Raider
bundle. The game's local `dosbox-x.conf` mounts its directory and starts the
game. The writable copy holds settings and saved games. A build without the
corresponding bundle has no master directory to copy.

For another DOS program, put its files in a writable directory and start
DOSBox-X there. At the DOS prompt, mount the directory as a drive, switch to
that drive and run the program. Startup commands can also be supplied with
repeated `-c` options.

### Select configuration and CPU speed

The system defaults are `/lib/dosbox-x/dosbox-x.conf`. User defaults under
`~/.config/dosbox-x/`, a `dosbox-x.conf` in the current directory, and explicit
launch options can override them. `-conf FILE` selects a configuration file;
`-set 'SECTION KEY=VALUE'` overrides a setting for that invocation.

```sh
dosbox-x -set 'cpu cycles=fixed 80000'
```

The default dynamic core requires JIT clearance. If acquisition fails, check
the launching context's eligibility rather than assuming a game-data error.
The `normal` core interprets instructions and is useful for diagnosis, but
is substantially slower for protected-mode games.

Use fixed cycles for the bundled games. An automatic cycle controller can
react to compositor timing and oscillate; increasing the fixed value is not
always beneficial when the emulation cannot keep up with real time.

### Return to the workspace

Exit the game and then use the DOS `exit` command to close the emulator.
Halcyon's workspace controls still manage its surface. Game output occupies
its own titled pane. Focus it and press Super+F to zoom; press Super+F again
to restore the tiled layout. These actions do not change the game's
configuration.

## Technical Details

The port uses Pouch for the native C and C++ runtime and SDL for display,
input and audio. The dynamic core requests explicit JIT authority and uses
the kernel's executable-memory interface. Executable code generation does
not make writable application memory executable by default.

A game's active surface declares dynamic frame intent so the compositor does
not throttle it as an idle still image. The audio backend creates an owned
Nocturne voice; closing the program releases its audio resources. A missing
sound device leaves the display path usable without inventing a recording
or playback device.

The system configuration is a build input. User configuration is a writable
copy and can retain older values after a system-default change. Inspect the
configuration files reported by the emulator when a launch behaves
differently from a fresh installation.
