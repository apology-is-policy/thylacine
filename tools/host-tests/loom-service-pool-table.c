#include <stdio.h>
#include "../../kernel/test/loom_pool_table_fixture.h"
int main(void) {
    const char *error = loom_pool_table_fixture();
    if (error) { fprintf(stderr,"FAIL %s\n",error); return 1; }
    printf("PASS joint pool table: %zu bytes\n",sizeof(struct loom_service_pool_table));
    return 0;
}
