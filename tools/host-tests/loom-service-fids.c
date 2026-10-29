#include <stdio.h>
#include "../../kernel/test/loom_fids_fixture.h"
int main(void) {
    const char *error = loom_fids_fixture();
    if (error) { fprintf(stderr,"FAIL %s\n",error); return 1; }
    printf("PASS walked FID reap: %zu bytes\n",sizeof(struct loom_service_node_table));
    return 0;
}
