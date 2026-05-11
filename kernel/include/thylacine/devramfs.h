// Public helpers for the in-kernel ramfs reader.
//
// devramfs (kernel/devramfs.c) parses the initrd cpio at boot and exposes
// files through the bestiary Dev vtable. For internal callers that need
// to slurp a file by name without going through the full Spoor /
// Walkqid / open / read path (e.g., test harnesses, boot-time program
// loaders), this header exposes a direct lookup.
//
// The returned pointer aliases storage owned by the initrd blob — which
// the kernel keeps mapped for the lifetime of the boot — so callers
// MUST NOT free it. The pointer is valid until kernel shutdown.

#ifndef THYLACINE_DEVRAMFS_H
#define THYLACINE_DEVRAMFS_H

#include <thylacine/types.h>

// devramfs_lookup — find file `name` in the parsed cpio. On success
// returns 0 and fills *out_data + *out_size with pointers into the
// initrd blob (caller MUST NOT modify or free).
//
// Returns:
//   0   on success
//  -1   not found, or devramfs not initialized, or NULL args.
int devramfs_lookup(const char *name, const void **out_data, size_t *out_size);

#endif // THYLACINE_DEVRAMFS_H
