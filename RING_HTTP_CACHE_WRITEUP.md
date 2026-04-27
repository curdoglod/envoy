# Envoy Ring Cache Write-up

## 1) Scope and status

### Implemented

- RAM-only ring cache backend (`ring_http_cache`) with configurable capacity (`ring_size`).
- Cache key includes host and URL via Envoy cache filter key construction.
- Request coalescing on cache miss (leader/follower model).
- Existing `Vary` behavior compatible with `simple_http_cache` patterns.
- Unit tests for ring cache behavior.

### Not fully implemented in this iteration

- Streaming response chunks to coalesced followers while leader is still receiving upstream data.
- Watermark-aware coalesced fanout implementation.
- Byte-based memory budgeting and strict in-flight admission controls.

## 2) Repository map

- `api/envoy/extensions/http/cache/ring_http_cache/v3/config.proto`:
  `RingHttpCacheConfig` with `ring_size`.
- `source/extensions/http/cache/ring_http_cache/`:
  ring cache backend implementation.
- `source/extensions/extensions_build_config.bzl`:
  extension registration under `envoy.extensions.http.cache.ring`.
- `configs/cache_demo/envoy-ring-cache-test.yaml`:
  local demo Envoy config.
- `configs/cache_demo/server.py`:
  small upstream server for local end-to-end checks.
- `configs/cache_demo/server1.py`:
  slow upstream variant for manual coalescing checks.

## 3) Build and demo

Build debug Envoy:

```bash
ENVOY_DOCKER_BUILD_DIR=$HOME/envoy-build ./ci/run_envoy_docker.sh './ci/do_ci.sh debug.server_only'
```

Start demo upstream:

```bash
python3 configs/cache_demo/server.py
```

Start Envoy (second terminal):

```bash
./linux/amd64/build_envoy_debug/envoy --config-path configs/cache_demo/envoy-ring-cache-test.yaml
```

Quick validation (`ring_size: 1` in demo config):

```bash
curl -si http://127.0.0.1:10000/a   # MISS, body has timestamp T1
curl -si http://127.0.0.1:10000/a   # HIT, same body, age > 0
curl -si http://127.0.0.1:10000/b   # MISS, evicts /a
curl -si http://127.0.0.1:10000/a   # MISS again, fresh timestamp
```

Manual coalescing test:

1. Start slow upstream in one terminal:

```bash
python3 configs/cache_demo/server1.py
```

2. In another terminal, run 10 parallel requests to the same URL:

```bash
URL="http://127.0.0.1:10000/slow?run=$(date +%s)"
seq 1 10 | xargs -I{} -P10 curl -s "$URL"
```

Expected: `server1.py` prints one `UPSTREAM request` line for the whole batch; all clients get
responses; repeating the same URL hits cache until `max-age` expires.

## 4) Design decisions

### Why cache v1 API

Using `source/extensions/filters/http/cache/` (v1): smaller API surface, behavior parity with
`simple_http_cache`, and existing test patterns were directly reusable.

### Cache key

The cache filter builds `Key` from:

- `:scheme`
- `:authority` (Host)
- `:path`
- `cluster_name`

This satisfies the requirement that Host and URL are part of the key.

Configurable custom key composition is possible as an extension, but was intentionally deferred to
keep this iteration small and testable.

### Data structures

```cpp
struct Slot { Key key; Entry entry; bool occupied; };
struct Subscriber { Event::Dispatcher* dispatcher; std::function<void()> wake;
                    std::shared_ptr<bool> cancelled; };
struct Inflight { std::vector<Subscriber> subscribers; };

std::vector<Slot> slots_;                          // ring buffer, size = ring_size
uint32_t next_slot_;                               // next overwrite slot
absl::flat_hash_map<Key, uint32_t> index_;         // key -> slot index
absl::flat_hash_map<Key, Inflight> inflight_;      // coalescing state per key
absl::Mutex mutex_;
```

### Insert and eviction behavior

Under writer lock:

1. Read path uses `find`, never accidental `operator[]` insertion.
2. Re-insert for existing key updates the same slot in place.
3. On insert into `next_slot_`, old key is removed from index first (if occupied).
4. Slot is overwritten, marked occupied, and index updated.
5. `next_slot_` advances only on a new-slot insert.

Result: deterministic FIFO eviction by ring slot order.

### Vary handling

Same model as `simple_http_cache`:

- marker entry at base request key stores `Vary` metadata;
- payload entry is stored under varied key (`request_key + vary_identifier`).

`lookup`, `updateHeaders`, and `varyLookup` resolve the effective varied key before returning or
updating payload data.

### Ring topology: one shared ring vs N rings

One shared singleton ring keeps `inflight_` coalescing state visible to all worker threads, so
concurrent misses on different workers still converge on one upstream fetch. In production the
natural next step is `N` rings by `hash(key) % N` — removes the global write-lock, same-shard
correctness for coalescing is preserved, `HttpCache` API stays unchanged.

## 5) Request coalescing

First miss becomes leader and fetches upstream; concurrent misses subscribe as followers in
`inflight_`. On `completeInflight(key)` each follower is woken on its dispatcher and retries
the lookup.

`inflight_[key]` is always completed by exactly one owner — `commit()`, `InsertContext::onDestroy()`,
or `LookupContext::onDestroy()`. `transferLeadership()` is called before lookup destruction so
the insert context takes over ownership and followers aren't woken prematurely.

Followers wake only after the full response is cached, not while leader is still receiving chunks
— that's the main trade-off of this implementation.

## 6) Limitations, watermarking, and production considerations

### Current limitations

- Single global mutex: correctness-focused, not maximum concurrency.
- Capacity is entry count (`ring_size`), not byte budget.
- No dedicated ring backend stats yet (evictions, occupancy, hit ratio, in-flight depth).
- `inflight_` has no strict capacity limit yet (possible abuse surface).
- Coalesced followers do not stream partial data; they wait for leader completion.

### Watermark buffers and streaming coalescing

Watermark buffers are Envoy's backpressure mechanism: high-watermark pauses reads/writes,
low-watermark resumes them. Since followers currently wait for full leader completion, the fanout
path doesn't exercise this.

To properly stream chunks to followers the design would need a shared body buffer with per-follower
read cursors and pause/resume tied to each follower's encoder watermark callbacks. The key
invariant: a slow follower pauses only itself, never the leader — otherwise the slowest client
defines upstream fetch latency for everyone.

Protocol specifics matter here. In HTTP/1.x each follower is on its own connection so it's
straightforward. In HTTP/2 multiple followers can share one TCP connection across streams —
watermark callbacks must be per-stream or pausing one follower stalls the whole connection
(head-of-line). HTTP/3 is the same logic as HTTP/2; transport-level independence of streams makes
per-stream pause/resume even cleaner in practice.

### What I would do differently in production

- `N` sharded rings by `hash(key) % N` instead of one global mutex.
- Byte-based capacity budget, not entry count.
- Streaming coalescing sessions with bounded shared buffer and per-follower cursors.
- Hard cap on `inflight_` size to prevent unique-key floods.
- Proper metrics: evictions, hit ratio, in-flight depth.

## 7) Testing

Ring cache tests:

- `test/extensions/http/cache/ring_http_cache/ring_http_cache_test.cc`
- `test/extensions/http/cache/ring_http_cache/BUILD`

Run tests via CI wrapper:

```bash
ENVOY_DOCKER_BUILD_DIR=$HOME/envoy-build ./ci/run_envoy_docker.sh './ci/do_ci.sh dev //test/extensions/http/cache/ring_http_cache:ring_http_cache_test'
```

Run directly with Bazel:

```bash
bazel test --config=clang //test/extensions/http/cache/ring_http_cache:ring_http_cache_test
```

## 8) Time spent

- Envoy setup, build environment, and understanding existing cache implementations: 10–12 hours
- Ring cache implementation and integration: 8–10 hours
- Request coalescing implementation and debugging: 8–10 hours
- Tests and manual validation: 4–6 hours
- Write-up, cleanup, and final review: 3–5 hours

Total: approximately 35–45 hours.

## 9) References

- `source/extensions/http/cache/simple_http_cache/` as baseline.
- `source/extensions/http/cache/file_system_http_cache/config.cc` for factory/singleton patterns.
- Coalescing references:
  - `source/extensions/filters/http/cache_v2/cache_sessions_impl.h`
  - `source/extensions/filters/http/cache_v2/cache_sessions_impl.cc`
  - `source/common/common/cancel_wrapper.h`
- Test baseline:
  - `test/extensions/http/cache/simple_http_cache/simple_http_cache_test.cc`
  - `test/extensions/http/cache/simple_http_cache/BUILD`
