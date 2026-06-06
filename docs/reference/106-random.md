# 106 — Kernel CSPRNG (`random.c` + `chacha20.c`)

As-built reference for the kernel cryptographically-secure RNG. Landed in
Lazarus W3 (`PORTABILITY.md §6`); replaces the RNDR-only v1.0 baseline.

## Purpose

Supply the kernel and userspace with cryptographic-quality random bytes on
*every* ARM64 substrate — TCG, HVF (Apple Silicon, no FEAT_RNG), and bare metal
— through one code path. Consumers: `SYS_GETRANDOM` (corvus, libsodium via
musl's `getentropy`), the ELF loader's `AT_RANDOM` (stack-canary / ASLR seed),
the Plan 9 `/dev/random` Dev surface. Before W3, `random.c` was RNDR-only and
returned `-1` on RNDR-less targets, breaking userspace crypto under HVF.

## Public API

```c
// kernel/include/thylacine/random.h
long  kern_random_bytes(void *out, long len);  // len on success, 0 if len==0, -1 unseeded/bad-arg
bool  kern_random_seeded(void);                // true once a strong source has seeded the pool (monotonic)
size_t random_seed_from_virtio(void);          // strong reseed from the virtio-rng device; returns bytes pulled
bool  kern_random_virtio_contributed(void);    // true iff a virtio-rng pull has ever contributed

// kernel/include/thylacine/chacha20.h
void chacha_keysetup(struct chacha_ctx *x, const u8 key[32]);
void chacha_ivsetup(struct chacha_ctx *x, const u8 iv[8]);
void chacha_keystream(struct chacha_ctx *x, u8 *out, u32 bytes);
```

`kern_random_bytes` contract is unchanged from the RNDR baseline: full count on
success, `-1` while unseeded (fail closed) or on a bad argument. `SYS_GETRANDOM`
(`kernel/syscall.c::sys_getrandom_handler`) gates on `kern_random_seeded()` and
treats a short read as failure.

## Implementation

The construction is OpenBSD `arc4random` (`kernel/random.c`):

- **State**: a `struct chacha_ctx` (`g_rng`), a 1024-byte keystream buffer
  (`g_rng_buf`), `g_rng_have` (bytes left in the buffer tail), `g_rng_count`
  (bytes until the next strong stir), `g_rng_seeded`, `g_rng_virtio_ok`. All
  guarded by `g_random_lock`; `g_rng_seeded`/`g_rng_virtio_ok` are also atomic
  for lock-free readers.
- **`rng_rekey_locked(dat, len)`**: generate a fresh `g_rng_buf` of keystream;
  XOR up to 40 bytes of `dat` into its head; re-key the context from the first
  `KEYSZ+IVSZ` (= 40) bytes (**fast key erasure** — the old key is gone, giving
  backtracking resistance); zero those 40 bytes; expose the remaining 984 bytes
  at the tail. The rekey material is never served.
- **`rng_buf_consume_locked(out, n)`**: serve from `g_rng_buf + (1024 - have)`
  (i.e. bytes `[40..1024)`), zeroing each served byte; rekey to refill on drain.
- **`rng_collect_cheap(out, cap, &strong)`**: non-blocking entropy — DTB
  `kaslr-seed`/`rng-seed` (each sets `strong`), one RNDR word if present (sets
  `strong`), then CNTPCT samples (with a short variable-latency spin between
  samples) to fill the remainder. CNTPCT alone is never `strong`.
- **`rng_stir_locked`**: collect cheap entropy + rekey + reset
  `g_rng_count = 1 MiB`; set `g_rng_seeded` if a strong source contributed.
- **`random_virtio_pull(out, want)`**: the virtio-rng driver (below).
- **`random_reseed_strong`**: pull virtio-rng entropy (OUTSIDE `g_random_lock`)
  + a cheap collection, then take `g_random_lock` and rekey from both. A
  successful virtio pull marks the pool seeded even with no DTB seed and no RNDR.

`kern_random_bytes`: if unseeded, `-1`. Decide the strong-reseed under the lock
(`g_rng_count <= n`), run the device pull *outside* the lock, re-check + serve
under the lock (a cheap stir is the fallback if the device was unavailable, so
the pool never serves unboundedly without re-keying).

### The virtio-rng driver (`random_virtio_pull`)

A thin consumer of the P4-F substrate (`kernel/virtio.c`); the first kernel
caller to do a real virtqueue data transfer. Steps (VIRTIO 1.2 §3.1.1):

1. `virtio_mmio_find_by_device_id(VIRTIO_DEVICE_ID_RNG)`; bail if absent.
2. `virtio_negotiate_features(dev, 0)` (reset → ACK → DRIVER → FEATURES_OK;
   virtio-rng needs no features).
3. `virtio_virtqueue_create(dev, 0)`; then `virtio_add_status(DRIVER_OK)`.
4. Alloc one `KP_ZERO` page; descriptor 0 = `{addr=pa, len=want,
   flags=VRING_DESC_F_WRITE}`; publish into the avail ring (`ring[0]=0`, `dsb`,
   `idx++`, `dsb`); `virtio_vq_notify`.
5. Bounded-poll `used->idx != 0` (cap `RNG_VIRTIO_POLL_MAX = 1<<22`); on
   completion `dsb`, copy `min(used.len, want)` bytes out.
6. **All-zero guard**: if the copied bytes are all zero, treat as failure
   (`got = 0`) — a coherency miss or a dead device must not pass as entropy.
7. Stop the device (`virtio_virtqueue_destroy` → `virtio_reset`) **before**
   scrubbing + freeing the page.

Serialized by `g_rng_dev_lock` (released before `g_random_lock` is taken in the
caller — no nesting). Lock order: `g_rng_dev_lock → buddy` (alloc/free);
`g_random_lock` is leaf.

## Data structures

- `struct chacha_ctx { u32 input[16]; }` — 4 sigma + 8 key + 2 counter + 2 nonce.
- Split virtqueue rings (`struct vring_desc/avail/used`, `virtio.h`) — borrowed
  per pull, freed at teardown.

## State machine (seeding)

```
BSS-zero --devrandom_init--> seeded-from-DTB+cntpct(+rndr)  (g_rng_seeded=true on QEMU)
         --main.c after virtio_init--> strong-reseed-from-virtio (g_rng_virtio_ok=true)
         --every ~1 MiB served--> strong-reseed (virtio + cheap)
         --every buffer drain (984 B)--> rekey (fast key erasure)
```

The DTB seed shared with KASLR carries the pre-virtio window only; the virtio
reseed (independent host entropy) lands before any userspace `SYS_GETRANDOM`,
and 16 of the 32 key bytes in that window are RNDR/CNTPCT-derived.

## Tests

`kernel/test/test_random.c`:
- `chacha20.block_vector` — the cipher matches the canonical RFC 8439 / DJB
  zero-key/nonce/counter keystream (proves the primitive).
- `chacha20.keystream_continuity` — block 1 differs from block 0 (counter).
- `kern_random.two_reads_differ` / `.large_read_nonzero` — the stream advances;
  a 2048-byte read crosses the buffer boundary (rekey-on-drain).
- `kern_random.virtio_reseed` — a full device bring-up/pull/teardown returns
  entropy (`> 0` — with the all-zero guard, this also asserts non-zero), marks
  the pool virtio-seeded, and the CSPRNG still serves.
- Pre-existing `kern_random.{seeded_returns_true_on_qemu,bytes_produces_nonzero}`
  stay green; `pouch-hello-sodium` xchacha20-poly1305 still round-trips.

## Error paths

- `kern_random_bytes`: `-1` on NULL/`len<0`/unseeded; `0` on `len==0`.
- `random_virtio_pull`: `0` on no-device / negotiate-fail / vq-create-fail /
  alloc-fail / poll-timeout / all-zero — every path resets the device + unlocks
  + leaks nothing. A `0` return leaves the CSPRNG on its prior seed.
- `SYS_GETRANDOM`: `-1` if `!kern_random_seeded()`, on a short read, or on a
  uaccess fault (partial output scrubbed).

## Status

Implemented W3a (`f19e985`, cipher + sources) + W3b (`c76f4ac`, virtio driver +
cadence). 721/721 kernel tests; 0 EXTINCTION. Audit: see
`memory/audit_w3_closed_list.md`.

## Known caveats / footguns

- **Non-coherent DMA** (bare-metal W4): the all-zero guard makes a coherency
  miss fail *safe* (the pool keeps its DTB seed), but a real bare-metal
  virtio/SD transport should add a cache-invalidate before the buffer read.
- **Reseed latency**: a single consumer pulling > 1 MiB triggers a virtio device
  round-trip (a few µs, bounded, non-blocking) from syscall context every 1 MiB.
  corvus/login draw far less and never hit it.
- **`g_random_lock` is process-context only** — no IRQ-context caller takes it
  (it would self-deadlock against a preempted holder; none exists today).
