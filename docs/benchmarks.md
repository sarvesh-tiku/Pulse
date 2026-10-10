# Benchmarks

Every number here was produced by the tools in this repo on the machine listed.
Raw outputs are in [`results/`](results/). Re-run with:

```bash
cmake --preset release && cmake --build --preset release
./build/release/benchmarks/pulse-bench --events 5M --repeat 5 --suite all --md out.md --json out.json
./build/release/benchmarks/pulse-micro --benchmark_min_time=0.5s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

## Environment

| | |
|---|---|
| Machine | Apple M1 Pro (8 cores: 6 performance + 2 efficiency), 16 GB |
| OS | macOS (Darwin 25.6.0, arm64) |
| Compiler | Apple clang 17.0.0, `-O3 -DNDEBUG`, no `-mcpu=native` |
| Workload | 5,000,000 synthetic L3 events, 8 instruments, `balanced` profile (41% add, 32% cancel, 11% modify, 17% trade), seed 42 |
| Strategy | `market-maker` (so the core does real work: risk checks, OMS, venue sim, P&L) |
| Method | end-to-end: median of 5 runs; micro: median of 3 repetitions |
| Linux cross-check | Ubuntu 24.04 container on Docker Desktop (arm64 VM, 4 vCPU), gcc 13 / clang 18 |

Caveats, stated up front:
* macOS has no hard thread-affinity API, and the M1 Pro mixes performance and
  efficiency cores. **Median latencies are stable run to run; tail latencies
  (p99 and beyond) varied by 10–100× between identical runs** and are reported
  as indicative only. A credible tail-latency study needs Linux with
  `isolcpus` + `--pin`.
* Every end-to-end run's digest is checked against the reference; a run that
  produced different decisions aborts the benchmark. All rows in the
  end-to-end tables share digest `c92ed408a31c91d6`.

---

## 1. Order book data structures

### Book operations alone (one instrument, 1M-event L3 stream, `pulse-micro`)

| Book | balanced | add-heavy | cancel-heavy | trade-heavy | heap allocs / event |
|---|---:|---:|---:|---:|---:|
| `MapOrderBook` (std::map + std::list + unordered_map) | 48.3 ns | 50.2 ns | 47.2 ns | 48.2 ns | 0.82–0.98 |
| `ArrayOrderBook<StdIndex>` (price array + pooled FIFO, unordered_map index) | 20.6 ns | 19.9 ns | 20.6 ns | 21.9 ns | 0.37–0.47 |
| `ArrayOrderBook<FlatIndex>` (price array + pooled FIFO + open addressing) | **11.7 ns** | **10.1 ns** | **10.2 ns** | **13.3 ns** | **0** |

Supporting micro-benchmarks:

| Operation | Time |
|---|---:|
| top-5 depth snapshot, both sides — map book | 20.1 ns |
| top-5 depth snapshot, both sides — array book (bitmap scan) | 7.8 ns |
| order-id index churn (insert + lookup + erase, ~20k live) — `std::unordered_map` | 31.4 ns |
| same — `FlatIdMap` (linear probing, backward-shift deletion) | 6.2 ns |

Reading: the price-indexed layout is worth ~2.4×, the flat hash index another
~1.8×, and together they eliminate steady-state allocation.

### In the full pipeline (inline, 8 instruments, market maker)

| Book | Throughput | ns / event | heap allocs / event |
|---|---:|---:|---:|
| `map` | 6.63 M/s | 150.8 | 0.904 |
| `array-stdindex` | 9.03 M/s | 110.8 | 0.416 |
| `array` | **11.01 M/s** | **90.8** | **0.008** |

The gain shrinks from ~4× (micro) to ~1.7× (pipeline) because the book is now
a minority of the per-event cost — see the stage breakdown below.

## 2. Where the time goes: stage breakdown

Cumulative cost per event, inline, 8 instruments, 1M events (`BM_PipelineStages`):

| Stage added | Cumulative ns/event | Stage cost |
|---|---:|---:|
| read records from memory | 4.3 | 4.3 |
| + feed handler: decode, CRC32C, sequencer, validation | 20.7 | 16.4 |
| + book builder: 8 books, top-5 snapshot, conflation, digest | 60.4 | 39.7 |
| + trading core, no strategy (venue sim, marks, digest) | 70.2 | 9.8 |
| + market-maker strategy (risk, OMS, orders) | 84.3 | 14.1 |

Protocol pieces: decode 8.8 ns without checksum, 13.3 ns with CRC32C;
CRC32C over a 56-byte frame is 3.8 ns with the ARMv8 instruction vs 78.8 ns
table-driven.

## 3. Optimization log (measured before → after)

Each step was found by profiling (`sample` on macOS, `perf record` on Linux)
or by the stage breakdown, changed in isolation, and re-measured.

| # | Finding | Change | Before | After |
|---|---|---|---:|---:|
| 1 | Node-based book: allocation + pointer chasing | `ArrayOrderBook<FlatIndex>` | 48.3 ns/event | 11.7 ns/event (book micro) |
| 2 | Book stage cost 78 ns in the 8-instrument pipeline vs 13 ns in the 1-instrument micro-benchmark: every book pre-sized a 65,536-order index + node pool (~4 MB per instrument, ~30 MB total, randomly accessed) | default capacity 1,024, grows on demand | book stage +78 ns | +48.5 ns |
| 3 | Determinism digest was byte-wise FNV-1a: 8 *serially dependent* multiplies per word | splitmix64 finalizer per word + one dependent multiply | core stage +17.5 ns | +5.4 ns |
| 4 | Depth snapshot taken for every event, even those behind the visible top-5 | skip snapshot when the touched price is behind the published 5th level | book stage +48.5 ns | +42.8 ns |
| 5 | **Linux only**: `perf` showed `crc32c` at 16.9% of runtime. The compile-time `__ARM_FEATURE_CRC32` check is false on default `-march=armv8-a` builds, so Linux silently used the table implementation | per-function `target("arch=armv8-a+crc")` / `target("sse4.2")` + runtime CPU detection | 5.0 M events/s (Linux, inline) | 8.5–9.0 M events/s |

Net effect on the end-to-end inline pipeline on the M1 Pro (`pulse-bench`,
array book): **124.1 → 90.8 ns/event (8.06 → 11.01 M events/s)** for steps
2–4; step 1 is the `map` → `array` row above (150.8 → 90.8 ns/event).

Linux after step 5 (Docker VM, inline, 3 runs each): gcc 13: 8.32 / 8.52 /
8.69 M events/s; clang 18: 8.99 / 8.81 / 8.97 M events/s.

## 4. Queues

### Raw queue throughput (one producer thread → one consumer, 2M × 56-byte items, `BM_QueueThroughput`)

| Queue | pop batch 1 | pop batch 64 |
|---|---:|---:|
| `std::mutex` + `condition_variable` | 27.4 M/s | 34.0 M/s |
| SPSC, **unpadded**, no index cache | 21.6 M/s | 32.9 M/s |
| SPSC, unpadded, cached indices | 21.2 M/s | 30.7 M/s |
| SPSC, padded, no index cache | 45.8 M/s | 43.2 M/s |
| SPSC, padded, cached indices | **70.7 M/s** | 42.8 M/s |

Reading:
* **False sharing is the dominant effect**: putting `head` and `tail` on
  separate cache lines is worth 2–3×. An *unpadded* lock-free ring is slower
  than the mutex queue.
* Index caching adds ~1.5× on top of padding for single-item pops.
* With padding + caching, batching the consumer *lowered* raw throughput here:
  a faster consumer catches up with the producer more often and spends time on
  empty polls. Batching's value shows up in the pipeline (below), where the
  consumer does real work per item.

### Hand-off latency (ping-pong between two threads, one-way = RTT/2, `BM_QueuePingPong`)

| Queue | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| mutex queue (spinning `try_pop`, no sleeping) | 167 ns | 14.1 µs | 24.7 µs |
| SPSC unpadded, no cache | 125 ns | 125 ns | 209 ns |
| SPSC padded, cached | **62 ns** | **83 ns** | 478 ns |

(Resolution note: `steady_clock` on Apple Silicon ticks at 24 MHz, ~42 ns.)

### Full threaded pipeline at max speed (feed → book → core threads)

| Pipeline | batch | Throughput |
|---|---:|---:|
| inline (1 thread) | – | 11.04 M/s |
| mutex + condvar | 1 | 2.57 M/s |
| mutex + condvar | 64 | 4.17 M/s |
| SPSC, no index cache | 1 | 4.77 M/s |
| SPSC, unpadded | 1 | 3.14 M/s |
| SPSC | 1 | 4.30 M/s |
| SPSC | 64 | **11.29 M/s** |

**The honest result: at ~85 ns of work per event, splitting the pipeline
across threads does not increase throughput.** With item-at-a-time hand-off,
per-item cross-core traffic (the 192-byte BookUpdate is three cache lines)
costs more than the parallelism gains; batching 64 items per queue visit
recovers parity with the single-threaded pipeline, and the book stage (the
slowest stage, ~40 ns/event) caps it. What threading *does* buy is isolation:
the feed thread keeps draining the socket while the core is busy (see
latency), and recording/telemetry never touch the hot path. The next
throughput step is sharding books across threads (see
[design-decisions.md](design-decisions.md#how-would-this-change-at-100-traffic)).

## 5. Latency under a fixed load

Offered load 1.0 M msgs/s (below saturation), paced by the feed thread;
latency from frame receipt to the core finishing the resulting update
(`pulse-bench --suite latency`, median-throughput run of 5):

| Pipeline | feed→book p50 | book→core p50 | end-to-end p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| inline (1 thread) | 83 ns | – | **125 ns** | 251 ns | 2.6 µs |
| mutex + condvar | 27.8 µs | 4.1 µs | 33.5 µs | 142 µs | 602 µs |
| SPSC, no index cache | 374 ns | 418 ns | 876 ns | 14.4 µs | 130 µs |
| SPSC | 458 ns | 374 ns | **916 ns** | 1.6 µs | 31.6 µs |

* The mutex/condvar pipeline's median is 30–40× worse: when a consumer finds
  the queue empty it sleeps on the condition variable, and every wake-up is a
  kernel round trip plus a context switch.
* The SPSC pipeline's ~0.9 µs median is two cross-core hand-offs plus the
  consumers' spin/yield wake-up; the inline pipeline has none, which is why
  it is the latency floor on this machine.
* Tail latency is scheduler-dominated on macOS. Re-running SPSC at batch 1
  and 64 three times each gave end-to-end p99 between 2.6 µs and 53 µs for the
  *same* configuration, while p50 stayed within 796–876 ns. Don't read
  batch-size effects into the tails from this machine.
* Latency at max speed is *not* reported: it measures queueing delay (the
  feed thread outruns the book thread and queues fill), not processing
  latency. That is why the latency suite uses a fixed offered load.

## 6. Correctness under measurement

Sanitizer builds (Linux container, all tests): gcc Release, clang Release,
clang ASan, clang UBSan, clang TSan — all passing. (Apple clang's ASan/TSan
runtimes hang at process start on this macOS version — the runtime spins in
`get_dyld_hdr` during shadow-memory setup — so sanitizers run on Linux via
`scripts/linux_check.sh` and in CI.)
