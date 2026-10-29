#include <stdio.h>
#include "../../kernel/test/loom_slots_fixture.h"
int main(void) {
    const char *err = loom_slots_fixture();
    if (err) { fprintf(stderr, "FAIL %s\n", err); return 1; }
    printf("PASS actual slot ledger: table=%zu slot=%zu\n",
           sizeof(struct loom_slot_table), sizeof(struct loom_service_slot));
    return 0;
}
