/* The host tests' t_loom_client_fatal: name the site, then abort. A death test
 * names the function it expects and is answered by exit status alone: 77 at
 * that function, 99 at any other, so a test assert (SIGABRT) never passes it. */
#include <thyla/loom_client/base.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
const char *t_test_expected_fatal;
void t_loom_client_fatal(const char *func,unsigned line){
 fprintf(stderr,"loom-client fatal: %s:%u\n",func,line);
 if(t_test_expected_fatal)_exit(strcmp(func,t_test_expected_fatal)?99:77);
 abort();
}
