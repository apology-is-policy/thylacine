// /pouch-hello-env -- the CL-1b-0 pouch-env boundary-line prover (Clade arc,
// docs/LLVM-DESIGN.md sub-chunk CL-1b-0). Proves that a pouch process sees its
// environment through getenv()/environ, populated by the crt from the /env
// device (patch 0025-pouch-env -> __pouch_env_init).
//
// The kernel passes NO envp (envp[0]==NULL); the environment lives in the
// per-Proc /env Dev. joey sets PGENV1 + PGENVNUM on its own /env, then spawns
// this binary, which inherits a COPY via the kernel clone (env_clone_into).
// __pouch_env_init reads /env at startup into __environ, so:
//   - getenv("PGENV1")   sees the inherited string value
//   - getenv("PGENVNUM") sees the inherited numeric value
//   - getenv(absent)     returns NULL
//   - environ iteration   enumerates both (proves the vector, not just getenv)
//
// On success: "pouch-hello-env: ... ENV OK" + exit 0. Any failure prints a
// labelled FAIL and exits non-zero so joey's reap sees it. fd 1 is a pipe
// joey relays to the boot-log UART.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

static int fail(const char *why) {
	printf("pouch-hello-env: %s FAIL\n", why);
	return 1;
}

int main(void) {
	// --- getenv the two inherited vars ---
	const char *v1 = getenv("PGENV1");
	if (!v1) return fail("getenv PGENV1 NULL");
	if (strcmp(v1, "clade-cl1b") != 0) return fail("PGENV1 value mismatch");

	const char *v2 = getenv("PGENVNUM");
	if (!v2) return fail("getenv PGENVNUM NULL");
	if (strcmp(v2, "1729") != 0) return fail("PGENVNUM value mismatch");

	// --- an absent variable must be NULL ---
	if (getenv("PGENV_ABSENT_NEVER_SET") != NULL)
		return fail("absent getenv returned non-NULL");

	// --- environ iteration must enumerate both (proves the whole vector,
	//     not only the getenv lookup path) ---
	int saw1 = 0, saw2 = 0;
	if (!environ) return fail("environ is NULL");
	for (char **e = environ; *e; e++) {
		if (!strncmp(*e, "PGENV1=", 7)) saw1 = 1;
		else if (!strncmp(*e, "PGENVNUM=", 9)) saw2 = 1;
	}
	if (!saw1) return fail("environ missing PGENV1");
	if (!saw2) return fail("environ missing PGENVNUM");

	printf("pouch-hello-env: PGENV1=%s PGENVNUM=%s -- ENV OK\n", v1, v2);
	return 0;
}
