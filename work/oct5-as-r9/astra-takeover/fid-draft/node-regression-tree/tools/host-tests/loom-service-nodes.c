#include <stdio.h>
#include "../../kernel/test/loom_nodes_fixture.h"
int main(void) {
    const char *error = loom_nodes_fixture();
    if (error) { fprintf(stderr,"FAIL %s\n",error); return 1; }
    printf("PASS scope/fid reservations: %zu bytes\n",sizeof(struct loom_service_node_table));
    return 0;
}
