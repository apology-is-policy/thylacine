---
id: fnd-noexec-r2-f1
type: fnd
round: adt-noexec-r2
severity: P2
status: fixed
title: "A well-formed image the kernel refuses by policy answered ENOEXEC"
surface: [sub-kernel-exec]
threatens: []
fixed-by: chg-2026-11-05-noexec-r2
regression: "execve.policy_refusal_says_eacces: a W+X segment: EACCES"
created: 2026-11-05
---
## Prosecution

`exec_load_body` and `exec_image_check` map every `elf_load` refusal but `HAS_INTERP` to the image's fault, and those include `RWX_REJECTED` and `EXEC_STACK`, valid aarch64 ELFs that Linux runs. busybox ash and dash read `-8` as a script and re-run the binary. They read EINVAL before this chunk, which no shell re-runs.

## Disposition

Operator vote 33: they read `T_E_ACCES` (`dec-2026-11-05-policy-refusal`), through `exec_refused_by_policy` in both places. Other well-formed images the loader cannot take stay `ENOEXEC`, and the row lists them. The report's POSIX reading was checked: POSIX's EACCES text there is about a file that is not regular, so it does not settle this, and the vote rests on what the call already means by EACCES.
