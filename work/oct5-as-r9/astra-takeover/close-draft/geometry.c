#include <thylacine/loom.h>
#include <thylacine/loom_service_engine.h>
unsigned long loom_bytes=sizeof(struct Loom);
unsigned long engine_bytes=sizeof(struct loom_service_engine);
_Static_assert(sizeof(struct Loom)>2048 && sizeof(struct Loom)<=4096,"private queue retains one-page Loom");
