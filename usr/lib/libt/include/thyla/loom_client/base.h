// The native C Loom client cores (docs/ASYNC-SERVICE-ABI.md, "Native C client
// storage"). They never allocate: every table lives in storage the caller
// provides, so a full table refuses before the kernel is touched, and a broken
// internal invariant ends the process.
#ifndef THYLA_LOOM_CLIENT_BASE_H
#define THYLA_LOOM_CLIENT_BASE_H
#include <stddef.h>
#include <stdint.h>
#include <thyla/bytes.h>
#include <thyla/loom_service.h>

// -ENOSPC, the errno the kernel's pool bank and request table answer when full.
#define T_LOOM_CLIENT_ENOSPC (-28)

// libt's definition writes the site to fd 2 and ends every thread of the
// process; a host test links its own.
__attribute__((noreturn)) void t_loom_client_fatal(const char *func, unsigned line);

#define T_LOOM_CHECK(cond) \
    do { if (!(cond)) t_loom_client_fatal(__func__, __LINE__); } while (0)

#endif
