#include <stdio.h>
#include "../../kernel/test/private_pool_fixture.h"
int main(void) {
    const char *err=private_pool_fixture_run();
    if(err) { fprintf(stderr,"FAIL %s\n",err); return 1; }
    printf("PASS actual pool state: bank=%zu pool=%zu cell=%zu result=%zu\n",
        sizeof(struct loom_pool_bank),sizeof(struct loom_pool),
        sizeof(struct loom_pool_cell),sizeof(struct loom_pool_result));
    return 0;
}
