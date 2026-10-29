#ifndef THYLA_LOOM_CLIENT_OWNED_MAPPING_H
#define THYLA_LOOM_CLIENT_OWNED_MAPPING_H
#include <thyla/loom_client/paired_cq.h>
/* Existing loom_params88 byte ABI, private receipt output interpretation. */
struct t_owned_setup{uint32_t flags,sq_entries,cq_entries,ring_size;uint64_t ring_va;uint32_t hdr_off,sq_array_off,sqe_off,cqe_off,sq_array_size,sqe_size,cqe_size,reserved;uint64_t receipt[4];};
_Static_assert(sizeof(struct t_owned_setup)==88,"setup ABI");
_Static_assert(offsetof(struct t_owned_setup,ring_va)==16,"setup VA");
_Static_assert(offsetof(struct t_owned_setup,receipt)==56,"setup receipt outputs");
/* Consumed backend owns descriptor and stable fresh mapping. No aliases/raw reap
 * escape. Truthful immutable base/len. Bounded non-reentrant close, destroy unmaps
 * only originally owned region and preserves independent kernel pins/charges. */
struct t_mapping_backend{void *context;void *(*base)(void *);size_t(*length)(void *);void(*close)(void *);void(*destroy)(void *);};
/* The caller's storage: zero it before its first use (static storage is), and
 * reuse it only after t_mapping_destroy. Its fields are the core's own. */
struct t_owned_mapping{struct t_paired_cq reader;struct t_mapping_backend backend;unsigned live,closed;};
enum{T_MAP_GEOMETRY=-1301,T_MAP_HEADER=-1302,T_MAP_CLOSED=-1303,T_MAP_CORRUPT=-1304};
_Static_assert(T_LOOM_CLIENT_LOCAL(T_MAP_GEOMETRY,1300)&&T_LOOM_CLIENT_LOCAL(T_MAP_HEADER,1300)&&
    T_LOOM_CLIENT_LOCAL(T_MAP_CLOSED,1300)&&T_LOOM_CLIENT_LOCAL(T_MAP_CORRUPT,1300),"mapping codes lie in their block");
/* Consumes backend on all paths and is live only on zero; the storage cannot
 * alias backend or mapped storage. */
int t_mapping_create(struct t_mapping_backend,struct t_owned_setup,struct t_owned_mapping *);
int t_mapping_reap(struct t_owned_mapping *,struct t_paired_record *);
void t_mapping_close(struct t_owned_mapping *);
void t_mapping_destroy(struct t_owned_mapping *);
#endif
