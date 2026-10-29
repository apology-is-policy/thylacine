#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* The series' own __pouch_env_init over a mocked /env: readdir lists the names, open + read give the values. */
struct var { const char *name; const char *value; size_t len; };
static const struct var *vars;
static size_t nvars, next_name, chunk;
static struct dirent dent;
static int dirs_open, fds_open, failures, checks;
static struct { const struct var *v; size_t off; } fds[8];
static DIR *mock_opendir(const char *path) {
 if (strcmp(path, "/env") != 0) { errno = ENOENT; return NULL; }
 dirs_open++; next_name = 0; return (DIR *)&dent;
}
static struct dirent *mock_readdir(DIR *d) {
 (void)d; if (next_name >= nvars) return NULL;
 snprintf(dent.d_name, sizeof dent.d_name, "%s", vars[next_name++].name); return &dent;
}
static int mock_closedir(DIR *d) { (void)d; dirs_open--; return 0; }
static int mock_open(const char *path, int flags) {
 if (strncmp(path, "/env/", 5) != 0 || flags != O_RDONLY) { errno = EINVAL; return -1; }
 for (size_t i = 0; i < nvars; i++) {
  if (strcmp(vars[i].name, path + 5) != 0) continue;
  for (int fd = 0; fd < 8; fd++)
   if (!fds[fd].v) { fds[fd].v = &vars[i]; fds[fd].off = 0; fds_open++; return fd + 3; }
  errno = EMFILE; return -1;
 }
 errno = ENOENT; return -1;
}
/* At most `chunk` bytes a read, so a NUL is also met in a later read than the first. */
static ssize_t mock_read(int fd, void *buf, size_t n) {
 const struct var *v = fds[fd - 3].v;
 size_t left = v->len - fds[fd - 3].off, k = n < left ? n : left;
 if (k > chunk) k = chunk;
 memcpy(buf, v->value + fds[fd - 3].off, k); fds[fd - 3].off += k; return (ssize_t)k;
}
static int mock_close(int fd) { fds[fd - 3].v = NULL; fds_open--; return 0; }
#define hidden
char **__environ;
#define opendir mock_opendir
#define readdir mock_readdir
#define closedir mock_closedir
#define open mock_open
#define read mock_read
#define close mock_close
#include "env-under-test.c"
#undef opendir
#undef readdir
#undef closedir
#undef open
#undef read
#undef close
/* "unchanged" stands for the envp exec projected, which __pouch_env_init keeps when it finds nothing. */
static char *exec_envp[] = { "FROM_EXEC=1", NULL };
static void run(const char *name, const struct var *v, size_t n, const char *const *want) {
 for (size_t c = 1; c <= 4096; c *= 4096) {
  vars = v; nvars = n; chunk = c; __environ = exec_envp;
  __pouch_env_init();
  int ok = dirs_open == 0 && fds_open == 0;
  if (!want) ok = ok && __environ == exec_envp;
  else {
   size_t i = 0;
   for (; want[i]; i++) ok = ok && __environ != exec_envp && __environ[i] && strcmp(__environ[i], want[i]) == 0;
   ok = ok && __environ != exec_envp && !__environ[i];
  }
  ++checks; printf("%s %s chunk=%zu\n", ok ? "PASS" : "FAIL", name, c); failures += !ok;
  if (__environ != exec_envp) { for (char **e = __environ; *e; e++) free(*e); free(__environ); }
 }
}
#define V(n, s) { n, s, sizeof s - 1 }
int main(void) {
 /* A name holding '=' cannot be a NAME=VALUE record: it is left out, not sent on as PE_EQ="B=v". */
 static const struct var eq[] = { V("PE_EQ=B", "v"), V("PE_OK", "1") };
 run("eq-name-skipped", eq, 2, (const char *const[]){ "PE_OK=1", NULL });
 static const struct var eq_ends[] = { V("=X", "v"), V("X=", "v"), V("=", "v"), V("PE_OK", "1") };
 run("eq-name-ends-skipped", eq_ends, 4, (const char *const[]){ "PE_OK=1", NULL });
 /* A NUL in a value would cut it short: left out, wherever it sits. */
 static const struct var nul[] = { V("PE_NUL", "a\0b"), V("PE_OK", "1") };
 run("nul-value-skipped", nul, 2, (const char *const[]){ "PE_OK=1", NULL });
 static const struct var nul_ends[] = { V("PE_N0", "\0ab"), V("PE_N1", "ab\0"), V("PE_N2", "\0"), V("PE_OK", "1") };
 run("nul-value-ends-skipped", nul_ends, 4, (const char *const[]){ "PE_OK=1", NULL });
 /* The controls, one byte away from each: the same entries with the '=' or the NUL replaced arrive whole. */
 static const struct var eq_ctl[] = { V("PE_EQ.B", "v"), V("PE_OK", "1") };
 run("eq-control-kept", eq_ctl, 2, (const char *const[]){ "PE_EQ.B=v", "PE_OK=1", NULL });
 static const struct var nul_ctl[] = { V("PE_NUL", "a.b"), V("PE_OK", "1") };
 run("nul-control-kept", nul_ctl, 2, (const char *const[]){ "PE_NUL=a.b", "PE_OK=1", NULL });
 static const struct var plain[] = { V("PE_EMPTY", ""), V("PE_EQV", "x=y=z") };
 run("value-equals-and-empty-kept", plain, 2, (const char *const[]){ "PE_EMPTY=", "PE_EQV=x=y=z", NULL });
 /* Nothing encodable: the exec envp stays, as with no /env at all. */
 static const struct var none[] = { V("A=B", "v"), V("PE_NUL", "\0") };
 run("both-only-keeps-exec-envp", none, 2, NULL);
 printf("tests=%d failures=%d\n", checks, failures); return !!failures;
}
