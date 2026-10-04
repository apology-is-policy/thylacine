#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define THYLACINE_SPINLOCK_H
#include <thylacine/types.h>
typedef struct { int held; } spin_lock_t;
#define SPIN_LOCK_INIT ((spin_lock_t){0})
static void spin_lock_init(spin_lock_t *l) { l->held=0; }
static void spin_lock(spin_lock_t *l) { if(l->held)abort();l->held=1; }
static void spin_unlock(spin_lock_t *l) { if(!l->held)abort();l->held=0; }
#include <thylacine/9p_client.h>
#define CLIENT_UNLOCK_RET(c, rc) do {spin_unlock(&(c)->lock);return(rc);}while(0)
static void *kmalloc(size_t n,unsigned flags) { (void)flags;return malloc(n); }
static void kfree(void *p) { free(p); }
void larder_init(struct larder *l) { memset(l,0,sizeof(*l)); }
void larder_destroy(struct larder *l) { (void)l; }
void poll_waiter_list_init(struct poll_waiter_list *l) { memset(l,0,sizeof(*l)); }
void poll_waiter_list_wake(struct poll_waiter_list *l) { (void)l; }
int wakeup(struct Rendez *r) { (void)r; return 0; }
static void uart_puts(const char *s) { (void)s; }
static void uart_putdec(u64 n) { (void)n; }
static int client_run(struct p9_client *c,size_t n,struct p9_dispatch_result *r) {
    (void)c;(void)n;(void)r;fputs("blocking engine reached\n",stderr);exit(1);
}
/* ACTUAL_CLIENT */
#include "private_client_fixture.h"
int main(void) {
    const char *err=private_client_fixture_run();
    if(err){fprintf(stderr,"FAIL: %s\n",err);return 1;}
    puts("PASS actual private client partial TX/RX, duplex, cancellation and malformed peer");return 0;
}
