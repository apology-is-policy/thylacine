#include <stdio.h>
#include "../../kernel/test/loom_requests_fixture.h"
int main(void) {
    const char *error=loom_requests_fixture();
    if (error) { fprintf(stderr,"FAIL %s\n",error);return 1; }
    puts("PASS request terminal obligations");return 0;
}
