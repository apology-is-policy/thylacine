# The Environment

The environment is a set of named values that a program receives from the
program that started it, such as `HOME`, the user's home directory, and `PATH`,
the directories searched for a command. Every process has its own environment.
A process that starts another gives it a copy of its environment as it stands
at that moment, so a change made later by either process does not reach the
other. A process that replaces its program, as `env` does to run a command,
keeps its environment.

A login session starts with the variables that login sets, which include
`HOME`, `USER` and `PATH`, and `ut` adds `BEACON`, which tells programs what
kind of output the terminal displays. Programs read the environment in their
usual way: a program built with Pouch through `getenv`, a Go program through
`os.Getenv`, and a native program through the file that holds the variable.

`env` prints the environment, and runs a command with variables added to its
environment or removed from it. `ut` has no command that sets a variable in its
own environment, so a variable that a command needs is given to it through
`env`.

## In Practice

### See the environment

```sh
env
cat /env/HOME
```

`env` prints each variable on a line of its own, as `NAME=VALUE`. Each variable
is also a file in the directory `/env`, which holds the environment of the
process that reads it: `cat /env/HOME` prints the value of `HOME` without a
newline after it, and `ls /env` lists the names.

### Run a command with a variable set

```sh
env FC_DEBUG=16 fc-match serif
```

`fc-match` runs with `FC_DEBUG` set to `16`, which makes it report each font
cache it checks, and so does every program it starts. The shell's environment
is unchanged: `cat /env/FC_DEBUG` afterwards fails, unless the shell already
had the variable. Several assignments may be given, and a
later assignment to the same name replaces an earlier one. The command's exit
status becomes `env`'s, so `$status` in `ut` reads the command's own status.

A command without a `/` is searched for in the directories that `PATH` names,
after the assignments are made, and in `/bin` when `PATH` is not set.

### Run a command with an empty environment

```sh
env -i PATH=/bin env
```

`-i` removes every variable before the assignments are made, so the command
receives only the variables given after it. This command prints a single line,
`PATH=/bin`. A program started with an empty environment has no `HOME`, `USER`
or `BEACON`, and it falls back to whatever its own defaults are.

### Setting a variable from ut

Redirecting output into a file under `/env` does not set the variable. The
command `echo 1 > /env/V` leaves `V` set to an empty value in the shell's
environment and discards the `1`: `ut` opens `/env/V` in its own environment,
emptying it, and `echo` writes the value into its own copy of the environment,
which ends when `echo` exits. Every command the shell starts afterwards
receives `V` with an empty value. Use `env V=1 command` to give a command a
variable.

### Command reference

```sh
env [-i] [NAME=VALUE]... [COMMAND [ARG]...]
```

| Option | Effect |
| --- | --- |
| `-i` | Start from an empty environment. `-` is accepted as the same option. |
| `--` | End the options; the next operand is an assignment or the command. |
| `--help` | Print the usage and exit. It is an option only among the options; after them it is an operand like any other. |

Every operand after the options that contains `=` is an assignment, and the
first operand without one is the command; the operands after the command are
its arguments, even when they contain `=`. Without a command, `env` prints the
resulting environment and exits with status 0.

`env` exits with the command's status when the command runs, 125 when `env`
itself fails, 126 when the command was found but could not be run, and 127 when
it was not found:

| Message | Cause |
| --- | --- |
| `env: invalid option 'opt'` | An option other than `-i`, `-` or `--` was given before the first operand. |
| `env: cannot set 'NAME=VALUE': invalid variable name` | The name is empty or contains a `/`. |
| `env: cannot set 'NAME': reason` | The environment refused the variable: the name is longer than 63 bytes, the value is longer than 4096 bytes, or the process already holds 64 variables. |
| `env: cannot unset 'NAME': reason` | `-i` could not remove a variable. |
| `env: cannot read path: reason` | `env` could not list its environment: `/env` for `-i`, or its `/proc/<pid>/environ` when printing. |
| `env: 'COMMAND': reason` | The command could not be run. A missing command exits with status 127; any other cause, such as a file that is not a program, exits with 126. |

## Technical Details

### Where the environment is kept

The kernel holds each process's environment and presents it as the device
mounted at `/env`. A variable's name is a file name in that directory, and its
value is the file's contents, as raw bytes. Every operation on `/env` acts on
the environment of the process that performs it. When a process starts
another, the kernel copies the starting process's variables into the new one,
and the copy is independent from then on. A name holds at most 63 bytes and no
`/`, a value at most 4096 bytes, and a process at most 64 variables.

A process that replaces its program keeps its `/env`. The kernel also writes
the variables into the new program's startup frame as `NAME=VALUE` strings,
which is where the C library and Go find them. A variable whose name contains
`=` or whose value contains a NUL byte cannot be written in that form, so the
kernel leaves it out of the startup frame and out of `/proc/<pid>/environ`; it
remains in `/env`. A program reading the startup frame sees the environment as
it was when the program started, while a program reading `/env` sees its
current state.

### How env runs a command

`env` makes its changes in its own environment: with `-i` it removes every file
in its `/env`, and for each assignment it writes the value into
`/env/NAME`. It then replaces its own program with the command, keeping its
process, its open descriptors and its environment, so the command starts with
the result. Because the replacement happens in the same process, the shell
waits for the command as it waited for `env`, and the command's exit status is
the one the shell reads. Without a command, `env` reads its own
`/proc/<pid>/environ`, the kernel's `NAME=VALUE` rendering of its environment,
and prints each entry on a line.

The kernel runs only programs in its executable format. `ut` reads the `#!`
line of a script and runs the interpreter it names; `env` does not, so a script
given to `env` as the command fails with status 126.

### Why a redirect into /env sets nothing

`ut` opens the target of a redirect itself and passes the open file to the
command as its output. Opening `/env/V` for writing empties `V` in the
environment of `ut`, the process that opened it. The command then writes
through the file it was given, and the kernel applies the write to the
environment of the writing process, which is the command's copy. The value
therefore never reaches the shell, and the shell's `V` stays empty. Plan 9
shares one environment between a process and the processes it starts unless a
process asks for a copy, so the same redirect sets the variable there.
