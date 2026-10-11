# Pulse

**A low-latency market data, execution, and risk engine in C++20.**

**11.0M events/s · 0.92 µs p50 / 1.6 µs p99 pipeline latency · 10–13 ns L3 book updates · deterministic record/replay**

Pulse is an end-to-end trading infrastructure engine: it decodes binary exchange-style feeds, reconstructs order-by-order limit order books, handles packet loss and reordering, evaluates strategies against pre-trade risk, routes orders through an order management system (OMS) and simulated venue, and maintains positions and P&L.

The point is not to predict prices. **The point is to build the machinery between market data and execution—and make its latency, ordering guarantees, failure behavior, and correctness measurable.**

Implemented in C++20, with an allocation-free hot path in the optimized order book, explicit concurrency boundaries, fault injection, differential testing, and deterministic replay. All performance figures below are measured, not theoretical; see [benchmark methodology and raw results](docs/benchmarks.md).

## Performance at a glance

Measured on **Apple M1 Pro**, unless otherwise stated. Workloads and baselines matter; results are not claims about colocated production trading latency.

| Measurement | Pulse | Comparison / context |
|:--|--:|:--|
| **Inline end-to-end throughput** | **11.0M events/s** | 8 instruments, market-making strategy active; 90.8 ns/event |
| **Optimized order-book mutation** | **10–13 ns/event** | 47–50 ns/event with `std::map` + `std::list` + `std::unordered_map`; zero hot-path heap allocations |
| **Threaded pipeline latency @ 1M messages/s** | **0.92 µs p50 · 1.6 µs p99** | Mutex/condition-variable pipeline: 33.5 µs p50 · 142 µs p99 |
| **SPSC one-way handoff** | **62 ns p50 · 83 ns p99** | Ping-pong microbenchmark |
| **Recovery under injected faults** | **1,896 / 1,896 recovered** | 1M-event session with drops, bursts, duplicates, reordering, and corruption; zero abandoned |
| **Replay fidelity** | **Identical 64-bit digests** | Across three book implementations, inline/four queue configurations, and record→replay |
| **Validation** | **118 tests** | GCC + Clang; ASan, UBSan, TSan on Linux |

**Important counterintuitive result:** threading did **not** improve peak throughput at this per-event compute cost. The inline engine wins on throughput; the multi-threaded topology provides pipeline isolation and reduces I/O-induced jitter. Batching is required even to recover the cost of cross-thread communication. The [optimization log](docs/benchmarks.md#3-optimization-log-measured-before--after) records five measured changes, including this negative result.

## Architecture

```text
                    Binary session file / UDP multicast-style input
                                         │
                                         ▼
                           ┌───────────────────────────┐
                           │       Feed handler        │
                           │ Frame decode + CRC32C     │
                           │ Sequence / reorder / gaps │◄──── TCP rewind service
                           │ Duplicate suppression     │      or clean session store
                           └─────────────┬─────────────┘
                                         │
                                   SPSC ring buffer
                                         │
                                         ▼
                           ┌───────────────────────────┐
                           │       Book engine         │
                           │ Per-instrument L3 books   │
                           │ FIFO matching state       │
                           │ Top-5 / conflation        │
                           └─────────────┬─────────────┘
                                         │
                                   SPSC ring buffer
                                         │
                                         ▼
                           ┌───────────────────────────┐
                           │    Deterministic core     │
                           │ Strategy → pre-trade risk │
                           │ → OMS → simulated venue   │
                           │ → positions / P&L         │
                           └─────────────┬─────────────┘
                                         │
                              Seqlock-backed telemetry
                                         │
                                         ▼
                                  Live dashboard

               Feed events ───────► Recorder ───────► Replay
```

The three-stage threaded design separates feed processing, book reconstruction, and trading logic with **single-producer/single-consumer (SPSC)** queues. An inline mode removes inter-thread handoffs for a more accurate view of raw compute throughput. The **trading core advances deterministically on venue time**, enabling end-to-end state comparisons across implementations.

### 1. Feed correctness before speed

A market-data engine cannot safely process a stream it cannot trust. Pulse treats sequencing as a correctness boundary, not a logging concern.

- **Wire format:** fixed-size little-endian binary frames with CRC32C; hardware-accelerated checksum dispatch on supported ARMv8 and SSE4.2 targets.
- **Validation pipeline:** decode → checksum → sequence → validate. Corrupted packets are surfaced as sequence gaps so the recovery mechanism has one coherent failure path.
- **Sequencer:** fixed-capacity, allocation-free reorder window; duplicate filtering; explicit `FeedGap`, `Recovered`, and `Stale` events.
- **Gap repair:** fetch missing frames from a clean on-disk session or TCP rewind server; replay can also heal passively from subsequently observed data.
- **Trailing loss:** heartbeats make end-of-stream gaps observable, instead of waiting for a future market event to expose missing sequence numbers.
- **Safe degradation:** if missing data cannot be repaired, feed state becomes stale and trading stops for that session. Pulse does not silently continue making decisions against an invalid book.

Relevant code: [`protocol/`](include/pulse/protocol/), [`market_data/feed_handler.hpp`](include/pulse/market_data/feed_handler.hpp), [`market_data/sequencer.hpp`](include/pulse/market_data/sequencer.hpp), [`market_data/rewind.hpp`](include/pulse/market_data/rewind.hpp).

### 2. L3 order books designed around memory locality

Pulse maintains **order-by-order (Level 3)** state, not just aggregated price levels. The optimized implementation combines:

- A **price-indexed array** to avoid pointer chasing through a tree for normal price-level operations.
- A **bitmap-assisted best-level search** to locate active prices efficiently.
- **Intrusive, pool-backed FIFO order queues** to preserve arrival order without allocating a separate list node for every update.
- An **open-addressed order-ID index** for direct order lookup and cancellation.

A `concept`-based interface allows the optimized book and a conventional `std::map` reference book to process the same events. **Differential tests** check their observable behavior against one another, while benchmarks isolate the cost of a book mutation.

This is why the 10–13 ns/event result matters: it is a measured data-structure comparison under the project's workload, **not** a claim that every market-data event takes 10 ns end-to-end.

Relevant code: [`orderbook/`](include/pulse/orderbook/).

### 3. Concurrency with explicit costs

The pipeline uses bounded SPSC rings rather than a general-purpose concurrent queue because each adjacent stage has exactly one producer and one consumer. The implementation exposes controlled variants for cache-line padding, cached indices, and a mutex/condition-variable baseline.

That makes several performance questions testable instead of ideological: cache-line contention, false sharing, memory-ordering costs, queue back-pressure, and the trade-off between isolation and message handoff. Workers use RAII lifecycle management, and the dashboard reads published state through a seqlock.

**Design principle:** a lock-free queue is not automatically faster as a *system*. Its benefit depends on the amount of useful work between handoffs, producer/consumer scheduling, and the benchmarked load regime.

Relevant code: [`concurrency/`](include/pulse/concurrency/), [design decisions](docs/design-decisions.md).

### 4. Execution built for adversarial event ordering

A realistic OMS cannot assume that acknowledgments and fills arrive neatly in order. Pulse explicitly models **fill-before-ack**, **fill-after-cancel**, duplicate reports, lost requests, venue rejection, disconnects, and reconnects.

- The **OMS** is a state machine; each fill is applied to positions **exactly once** despite duplicate or reordered execution reports.
- The **venue simulator** supports latency and jitter, ordered or overtaking report delivery, estimated queue-position fills, rejects, session disconnects, cancel-on-disconnect, and resend on reconnect.
- The **risk engine** checks order size, notional, price collars, worst-case positions, open orders, message rate, feed/venue health, and maximum loss. A kill switch can trip automatically after sustained throttling.
- The **position engine** uses fixed-point average-cost accounting; flat-position accounting is property-tested.
- **Strategy fault containment** disables a crashing strategy and cancels its outstanding orders without taking down the remaining strategies.

The venue is intentionally **simulated in-process**: reproducible failure injection and deterministic state transitions take priority over pretending this is a production exchange gateway.

Relevant code: [`execution/`](include/pulse/execution/), [`risk/`](include/pulse/risk/), [`portfolio/`](include/pulse/portfolio/), [`engine/trading_core.cpp`](src/engine/trading_core.cpp).

## Failure drills: break it, then prove the response

These are observed results from 1M-event, 8-instrument sessions—not hypothetical behavior. The complete failure-to-test matrix is in [docs/failure-handling.md](docs/failure-handling.md).

| Scenario | Injected condition | Observed response |
|:--|:--|:--|
| **Feed corruption / packet loss** | Drops, bursts, duplicates, reordering, CRC errors | 1,896 missing messages identified and recovered; 0 abandoned; book validation clean |
| **Replay after recovery** | Persist the recovered event stream; replay without rewind server | Same `6ba237735166253f` state digest; 0 retransmission requests during replay |
| **No recovery source** | Missing sequence numbers cannot be supplied | Feed transitions to `STALE`; trading halts rather than consuming inconsistent market state |
| **Runaway strategy** | Strategy floods orders and ignores health | Rate limit and kill switch contain activity; no residual positions in the demonstrated run |
| **Strategy exception** | One strategy throws after 20,000 events | Only that strategy is disabled; its orders cancelled; the other strategy continues |
| **Venue disconnect / reordered reports** | Session down/up; execution reports overtake acks | Cancel/reconnect behavior exercised; 17 fill-before-ack cases correctly accounted for |

**Recovery + reproducibility example:**

```console
$ pulse-gen --out drill.pulse --events 1M --faults drop=5e-4,dup=5e-4,reorder=5e-4,corrupt=2e-4,burst=5e-6,burst_len=100
  dropped 1172  duplicated 499  reordered 504  corrupted 220  truncated 0

$ pulse run drill.pulse --record rec.pulse
sequencer: gaps 1201, missing 1896, recovered 1896, duplicates 1003, abandoned 0
retransmit: requests 1201, frames 1896, failures 0
books: published 722257, conflated 280144, errors 0, crossed 0, trade mismatches 0
digest          6ba237735166253f

$ pulse replay rec.pulse
retransmit: requests 0, frames 0, failures 0
digest          6ba237735166253f
```

The digest is a **regression oracle for equivalent engine state**, not a cryptographic proof of correctness. It complements differential book tests, invariants, sanitizers, and deliberately adversarial integration tests.

## Build and run

**Requirements:** CMake ≥ 3.24, C++20 (Clang ≥ 15 or GCC ≥ 12), POSIX. CMake fetches GoogleTest and Google Benchmark.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release                # 118 tests
```

**Generate → inspect → execute → replay:**

```bash
B=build/release

# Build a 10M-event feed with network faults and a clean retransmission source.
$B/pulse-gen --out s.pulse --events 10M \
  --faults drop=1e-4,dup=1e-4,reorder=1e-4,corrupt=1e-5,burst=1e-6
# Produces s.pulse and s.pulse.rewind

$B/pulse inspect s.pulse

# Run strategies through book reconstruction, pre-trade risk, OMS and venue.
$B/pulse run s.pulse --strategy market-maker,imbalance --record rec.pulse

# Reproduce the final state from the persisted recording.
$B/pulse replay rec.pulse --speed 10x
$B/pulse replay rec.pulse --max-speed

# View live telemetry on a paced input stream.
$B/pulse run s.pulse --rate 500k --dashboard
```

**UDP feed + TCP rewind:**

```bash
$B/pulse listen --udp 127.0.0.1:9000 --rewind-tcp 127.0.0.1:9001 &
$B/pulse-feed s.pulse --udp 127.0.0.1:9000 --rewind-port 9001 \
  --rate 200k --faults drop=1e-3,reorder=1e-3
```

**Benchmark and instrument:**

```bash
$B/benchmarks/pulse-bench --events 5M --repeat 5 --suite all
$B/benchmarks/pulse-micro

cmake --preset asan   # AddressSanitizer
cmake --preset ubsan  # UndefinedBehaviorSanitizer
cmake --preset tsan   # ThreadSanitizer
```

On macOS, run `scripts/linux_check.sh` for Linux GCC/Clang and sanitizer checks inside an Ubuntu container; the current Apple Clang sanitizer runtime can hang on startup. Run `pulse run --help` for queue type (`spsc`, `spsc-nocache`, `spsc-unpadded`, `mutex`), book type (`array`, `array-stdindex`, `map`), batch size, pacing, CPU pinning, venue jitter/reordering, risk thresholds, and disconnect windows.

## Measurement methodology and trade-offs

Pulse distinguishes **microbenchmarks** (queue handoff, book mutation) from **pipeline measurements** (scheduled work across threads) and **inline throughput** (no queue transfer). These quantities answer different questions and should not be compared interchangeably.

The current results establish the following under documented workloads:

1. **Data layout dominates order-book update cost.** Contiguous price slots, pooled FIFO nodes, and direct ID indexing outperform the tree/list/hash baseline used here.
2. **Queue design dominates latency once work is cheap.** The SPSC configuration cuts the measured p99 versus mutex/condition variable, but the handoff itself is still significant compared with a 10–13 ns book update.
3. **Correctness changes the fast path.** Sequence gaps, stale-feed health gates, out-of-order fills, and replay are first-class state transitions—not features bolted onto a happy-path benchmark.
4. **Throughput and isolation are different objectives.** The fastest inline path is not necessarily the right topology when reception jitter and operational fault containment matter.

Read [docs/benchmarks.md](docs/benchmarks.md) for workload construction, latency percentiles, full tables, five before/after optimization experiments, and caveats. **Tail-latency figures collected on macOS are indicative**, not a substitute for measurements on isolated Linux cores with hardware timestamps.

## Repository map

```text
include/pulse/
  core/          protocol/        market_data/    orderbook/
  concurrency/   execution/       risk/           portfolio/
  strategy/      engine/          telemetry/      net/
src/             Implementations, following the same module layout
apps/pulse/      CLI: run, replay, listen, inspect
tools/           pulse-gen and pulse-feed (fault injection / rewind)
benchmarks/      Google Benchmark microbenchmarks and E2E pulse-bench
tests/           Unit, property, differential, stress, integration
docs/            Architecture, design trade-offs, benchmark data, failures
scripts/         Linux GCC/Clang/sanitizer validation
```

## Scope and limits

This is an **execution-infrastructure and systems-engineering project, not a trading strategy or production exchange gateway**. It deliberately includes realistic failure semantics but excludes several production-critical components:

- **Snapshot recovery:** unrecoverable feed gaps currently halt the session; a production implementation would resynchronize from an authoritative order-book snapshot.
- **Horizontal book scaling:** the book stage is the throughput bottleneck; future work is instrument sharding across book threads followed by deterministic merge before the trading core.
- **Kernel bypass / hardware timestamps:** networking is POSIX UDP; timing uses `steady_clock` (approximately 42 ns observed resolution on Apple Silicon). Precision is not the same as ground-truth wire-to-wire latency.
- **Host-level tail validation:** Linux `--pin` is implemented, but isolated-core Linux benchmarking is still needed before making stronger p99/p99.9 claims.
- **Exchange fidelity:** fills estimate queue position using displayed size; there is no market-impact model.
- **External order gateway:** orders enter the in-process simulator to preserve determinism. A TCP-based gateway would be an intentionally nondeterministic operating mode.

## Technical documentation

- **[Architecture](docs/architecture.md)** — thread boundaries, data model, event flow, and where time is spent.
- **[Design decisions](docs/design-decisions.md)** — SPSC versus MPMC, memory ordering, false sharing, back-pressure, determinism, book layout, gap policy, and 100× scaling options.
- **[Benchmarks](docs/benchmarks.md)** — experimental setup, raw measurements, optimization log, baselines, and limitations.
- **[Failure handling](docs/failure-handling.md)** — injected faults, detection, containment behavior, and corresponding tests.

---

**Pulse optimizes for a narrower claim than “fast trading”: a market-data-to-execution pipeline whose performance can be measured, whose state can be replayed, and whose failure modes can be tested.**
