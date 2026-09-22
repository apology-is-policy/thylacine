// The five provers' LEG CENSUS markers -- one definition, read by the prover
// that prints it and by joey, which matches it.
//
// A marker names every leg its prover runs, so a STALE binary (the bake traps
// that skip a populate) prints an older census and cannot pass for this one:
// every new leg would otherwise be green without having run. Adding a leg means
// adding its name HERE, once. Sharing the header does not weaken that: a stale
// binary was compiled against the old string, and joey against this one.
#ifndef POUCH_CENSUS_H
#define POUCH_CENSUS_H

#define POUCH_CENSUS_MALLOC \
    "pouch-hello-malloc: legs=heap,physpages,nprocs,sentinel-wrappers,ualarm,dtablesize: exit 0"
#define POUCH_CENSUS_THREADS \
    "pouch-hello-threads: legs=pthread,mutex,main-stack-maps-row: exit 0"
#define POUCH_CENSUS_SOCKETS \
    "pouch-hello-sockets: legs=refusals,paths,round-trip,peercred,stdio,ppoll,ppoll-eof,slots,fdset-guard: exit 0"
#define POUCH_CENSUS_FOPEN \
    "pouch-hello-fopen: legs=create,append-omode,truncate,excl,unlink,remove,tmpfile,scan: exit 0"
#define POUCH_CENSUS_IDENTITY \
    "pouch-hello-identity: legs=getpid,proc-status,uid-agrees,gid-agrees,not-sentinel: exit 0"

#endif
