#include "retirement_executor.h"
#include <stdlib.h>
#include <assert.h>
struct cell{struct t_fixed_driver *driver;unsigned retiring;};
struct t_retirement_executor{struct cell cells[64];size_t capacity,cursor,refs;unsigned open;};
struct t_managed_client{struct t_retirement_executor *executor;size_t slot;};
static void release(struct t_retirement_executor *e){assert(e->refs);if(--e->refs==0){for(size_t i=0;i<e->capacity;++i)if(e->cells[i].driver)t_fixed_driver_drop(e->cells[i].driver);free(e);}}
int t_retirement_create(size_t capacity,struct t_retirement_executor **out){
 assert(out);*out=NULL;if(!capacity||capacity>64)return T_RETIREMENT_CAPACITY;
 struct t_retirement_executor *e=calloc(1,sizeof(*e));if(!e)return -12;
 e->capacity=capacity;e->refs=1;e->open=1;*out=e;return 0;
}
int t_retirement_admit(struct t_retirement_executor *e,t_driver_factory make,void *context,struct t_managed_client **out){
 assert(e->open&&out&&make);*out=NULL;size_t slot=0;while(slot<e->capacity&&e->cells[slot].driver)++slot;
 if(slot==e->capacity)return T_RETIREMENT_FULL;
 struct t_managed_client *c=calloc(1,sizeof(*c));if(!c)return -12;
 struct t_fixed_driver *d=NULL;int rc=make(context,&d);
 if(rc||!d){if(d)t_fixed_driver_drop(d);free(c);return rc?rc:T_FIXED_CLOSING;}
 e->cells[slot]=(struct cell){d,0};++e->refs;*c=(struct t_managed_client){e,slot};*out=c;return 0;
}
size_t t_retirement_occupied(const struct t_retirement_executor *e){assert(e->open);size_t n=0;for(size_t i=0;i<e->capacity;++i)n+=e->cells[i].driver!=NULL;return n;}
struct t_retirement_progress t_retirement_progress(struct t_retirement_executor *e,size_t budget){
 assert(e->open);size_t visited=budget<e->capacity?budget:e->capacity,reclaimed=0;
 for(size_t n=0;n<visited;++n){size_t i=e->cursor;e->cursor=(i+1)%e->capacity;struct cell *c=&e->cells[i];
  if(c->driver&&c->retiring&&t_fixed_progress(c->driver)){t_fixed_driver_drop(c->driver);*c=(struct cell){0};++reclaimed;}
 }
 return (struct t_retirement_progress){visited,reclaimed,t_retirement_occupied(e)};
}
void t_retirement_drop(struct t_retirement_executor *e){assert(e->open);e->open=0;for(size_t i=0;i<e->capacity;++i)if(e->cells[i].driver)t_fixed_close(e->cells[i].driver);release(e);}
int t_managed_read(struct t_managed_client *c,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,struct t_fixed_request **out){if(!c->executor->open){*out=NULL;return T_FIXED_CLOSING;}return t_fixed_read(c->executor->cells[c->slot].driver,scope,range,out);}
int t_managed_write(struct t_managed_client *c,struct t_loom_service_ref scope,struct t_loom_service_pool_member range,const void *data,size_t len,struct t_fixed_request **out){if(!c->executor->open){*out=NULL;return T_FIXED_CLOSING;}return t_fixed_write(c->executor->cells[c->slot].driver,scope,range,data,len,out);}
int t_managed_receive(struct t_managed_client *c,const struct t_paired_record *record){if(!c->executor->open)return T_FIXED_CLOSING;return t_fixed_receive(c->executor->cells[c->slot].driver,record);}
void t_managed_drop(struct t_managed_client *c){struct t_retirement_executor *e=c->executor;struct cell *cell=&e->cells[c->slot];assert(cell->driver&&!cell->retiring);cell->retiring=1;free(c);release(e);}
