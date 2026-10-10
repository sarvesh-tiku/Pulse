# Pulse

**A low-latency market data, execution and risk engine in C++20.**

Pulse ingests a binary order-by-order market-data feed (from a file or over
UDP), recovers from packet loss, reconstructs limit order books, runs
strategies through pre-trade risk into a simulated exchange, tracks positions
and P&L, and can record and deterministically replay everything it saw.

It is a miniature version of the infrastructure between an exchange feed and
a trading strategy, built to be measured and broken on purpose. It has no
alpha.

```
UDP / session file ─► feed thread ─► SPSC ─► book thread ─► SPSC ─► core thread
                      decode, CRC32C,        L3 books,              strategy → risk → OMS
                      sequence, recover      top-5, conflation      → venue sim → P&L
                         │                                            │
                      recorder thread                          dashboard (seqlock)
```

## Headline results (measured; Apple M1 Pro, see [docs/benchmarks.md](docs/benchmarks.md))

| | |
|---|---|
| Inline pipeline throughput (8 instruments, market maker running) | **11.0 M events/s** (90.8 ns/event) |
| Order book update (price-indexed array + flat hash index) | **10–13 ns/event**, 0 heap allocations (vs 47–50 ns for `std::map` + `std::list` + `unordered_map`) |
| End-to-end latency, threaded SPSC pipeline @ 1 M msgs/s | **p50 0.92 µs, p99 1.6 µs** (mutex + condvar: p50 33.5 µs, p99 142 µs) |
| SPSC queue hand-off (ping-pong, one way) | p50 62 ns, p99 83 ns |
| Feed recovery (1M events with drops, bursts, dups, reorders, corruption) | 1,896 / 1,896 missing messages recovered, 0 abandoned |
| Determinism | identical 64-bit digest across 3 book types × inline / 4 queue types, and record → replay |
| Tests | 118 tests; clean under ASan, UBSan and TSan (Linux) with gcc and clang |

Five profiling-driven optimizations are documented with before/after numbers
in [docs/benchmarks.md § Optimization log](docs/benchmarks.md#3-optimization-log-measured-before--after).
One of them is an honest negative result: at this per-event cost, threading
the pipeline does **not** raise throughput. It isolates work and cuts I/O
jitter, and batching is needed just to break even.

## Build

Requirements: CMake ≥ 3.24, a C++20 compiler (clang ≥ 15 or gcc ≥ 12), POSIX.
GoogleTest and Google Benchmark are fetched automatically.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release            # 118 tests
```

Sanitizers: `cmake --preset asan|ubsan|tsan`. On macOS, use
`scripts/linux_check.sh` instead: it runs gcc, clang, ASan, UBSan and TSan
builds in an Ubuntu container, because Apple clang's ASan/TSan runtime hangs
at startup on current macOS.

## Use

```bash
B=build/release

# 1. Generate a session: 10M events, 8 instruments, with injected network faults.
$B/pulse-gen --out s.pulse --events 10M --faults drop=1e-4,dup=1e-4,reorder=1e-4,corrupt=1e-5,burst=1e-6
#    -> s.pulse (what the receiver saw) + s.pulse.rewind (clean stream for retransmission)

# 2. Inspect / validate it.
$B/pulse inspect s.pulse

# 3. Run it through the engine (threaded, SPSC, array book, market maker), recording everything.
$B/pulse run s.pulse --strategy market-maker,imbalance --record rec.pulse

# 4. Replay the recording deterministically (same digest, no retransmission source needed).
$B/pulse replay rec.pulse --speed 10x
$B/pulse replay rec.pulse --max-speed

# 5. Live dashboard at a paced rate.
$B/pulse run s.pulse --rate 500k --dashboard

# 6. Over the network: UDP market data + TCP retransmission service.
$B/pulse listen --udp 127.0.0.1:9000 --rewind-tcp 127.0.0.1:9001 &
$B/pulse-feed s.pulse --udp 127.0.0.1:9000 --rewind-port 9001 --rate 200k --faults drop=1e-3,reorder=1e-3

# 7. Benchmarks.
$B/benchmarks/pulse-bench --events 5M --repeat 5 --suite all
$B/benchmarks/pulse-micro
```

`pulse run --help` lists every option: queue type (`spsc`, `spsc-nocache`,
`spsc-unpadded`, `mutex`), book type (`array`, `array-stdindex`, `map`),
batch size, pacing, CPU pinning, risk limits, venue latency/jitter/rejects,
report reordering, and disconnect windows.

## What's inside

| Area | Highlights | Code |
|---|---|---|
| **Binary protocol** | fixed-size little-endian frames, CRC32C with hardware (ARMv8 / SSE4.2) runtime dispatch, framing errors distinguished from corruption | `protocol/` |
| **Feed handler** | decode → checksum → sequence → validate; corrupt frames become gaps so there is one recovery path; heartbeats reveal trailing loss | `market_data/feed_handler.hpp` |
| **Sequencer** | fixed-window reorder buffer (no allocation), duplicate suppression, in-band `FeedGap/Recovered/Stale` events | `market_data/sequencer.hpp` |
| **Recovery** | retransmission from a clean session store or a TCP rewind server; passive healing when replaying | `market_data/rewind.hpp`, `net/` |
| **Order books** | `std::map` reference vs price-indexed array with bitmap best-level search, intrusive pooled FIFO and open-addressing id index; one `concept`, differential-tested | `orderbook/` |
| **Concurrency** | SPSC ring with padding and cached-index knobs, mutex/condvar baseline, seqlock, RAII workers, back-pressure accounting | `concurrency/` |
| **Trading core** | single-threaded and deterministic on venue time; strategies → risk → OMS → venue sim → P&L; contains strategy crashes | `engine/trading_core.cpp` |
| **Exchange simulator** | latency + jitter, ordered or overtaking report paths, queue-position fill model, rejects, disconnects with cancel-on-disconnect and resend | `execution/exchange_simulator.cpp` |
| **OMS** | state machine tolerant of fill-before-ack, fill-after-cancel, duplicates and lost requests; fills applied exactly once | `execution/order_manager.cpp` |
| **Risk** | kill switch, feed/venue health gates, size, notional, price collar, worst-case position, open orders, token-bucket rate limit, auto-trip on sustained throttling or max loss | `risk/risk_engine.cpp` |
| **P&L** | fixed-point average-cost accounting, exact when flat (property-tested) | `portfolio/position.cpp` |
| **Telemetry** | log-linear latency histograms (≤1.6% error, no allocation), per-stage latency, terminal dashboard | `telemetry/` |

## Failure drills

All outputs below are real runs (1M-event session, 8 instruments).

**Faulty feed, recovered, recorded, replayed:**

```
$ pulse-gen --out drill.pulse --events 1M --faults drop=5e-4,dup=5e-4,reorder=5e-4,corrupt=2e-4,burst=5e-6,burst_len=100
  dropped 1172  duplicated 499  reordered 504  corrupted 220  truncated 0

$ pulse run drill.pulse --record rec.pulse
frames ok 1001003 | decode errors 220 (checksum 217, truncated 0, length 0) | invalid 0
sequencer: gaps 1201, missing 1896, recovered 1896, duplicates 1003, abandoned 0, peak buffered 1
retransmit: requests 1201, frames 1896, failures 0
books: published 722257, conflated 280144, errors 0, crossed 0, trade mismatches 0
digest          6ba237735166253f

$ pulse replay rec.pulse
retransmit: requests 0, frames 0, failures 0
digest          6ba237735166253f
```

**Same feed, no retransmission source.** The gap can't be filled, so the feed
goes stale and trading halts:

```
sequencer: gaps 255, missing 1896, recovered 504, duplicates 499, abandoned 1384, peak buffered 65503
  feed STALE at seq 2252: trading halted for the session
```

**Runaway strategy** (fires orders on every tick and ignores health). Risk
contains it and the throttle trips the kill switch:

```
$ pulse run s.pulse --strategy runaway
risk: kill_switch=5747499 max_open_orders=6 rate_limit=500  (passed 503)
kill switch: TRIPPED (order rate: throttle limit exceeded)
P&L: realized 0.00, ... positions [ALFA 0, BRVO 0, ...]
```

**Strategy crash.** The strategy is isolated and the other one keeps trading:

```
$ pulse run s.pulse --strategy crash,imbalance --crash-after 20000
orders 2885, replaces 19, cancels 15, fills 2524 (243373 shares), halts 0
  strategy 'crash' crashed: strategy bug: invariant violated -> disabled, orders cancelled
```

**Venue disconnect with out-of-order reports.** Cancel-on-disconnect, resend
on reconnect, and fills that overtake their acks:

```
$ pulse run s.pulse --disconnect 300:500 --reorder-reports
venue: accepted 4294, rejected 0, cancel-rejects 2, dropped-while-down 2, deferred 0
oms: fill-before-ack 17, late fills 0, duplicates 0, unknown 0, expired 0, overfills 0
  venue session DOWN
  venue session UP
```

Full matrix of failures, detection, response and the test covering each:
[docs/failure-handling.md](docs/failure-handling.md).

## Docs

* [Architecture](docs/architecture.md): threads, data model, where the time goes
* [Design decisions](docs/design-decisions.md): SPSC vs MPMC, memory ordering,
  false sharing, back-pressure, determinism, book layout, gap policy, scaling
  to 100×
* [Benchmarks](docs/benchmarks.md): methodology, all tables, optimization log,
  caveats
* [Failure handling](docs/failure-handling.md)

## Repository layout

```
include/pulse/   core/ protocol/ market_data/ orderbook/ concurrency/
                 execution/ risk/ portfolio/ strategy/ engine/ telemetry/ net/
src/             implementations (same layout)
apps/pulse/      pulse CLI: run / replay / listen / inspect
tools/           pulse-gen (feed generator + fault injection), pulse-feed (UDP publisher + rewind server)
benchmarks/      micro/ (Google Benchmark), e2e/ (pulse-bench)
tests/           unit, property, differential, stress and integration tests
docs/            design docs and raw benchmark results
scripts/         linux_check.sh (gcc/clang/sanitizers in Docker)
```

## Limitations and future work

* **Snapshot recovery.** A stale feed halts for the session; a production
  handler would request a book snapshot and resume.
* **Book sharding.** The book stage is the throughput bottleneck; the next
  step is instruments sharded across book threads with a deterministic merge
  in front of the core.
* **Kernel bypass and hardware timestamps.** The receive path is POSIX UDP.
  Latency is measured with `steady_clock` (~42 ns resolution on Apple Silicon).
* **Tail latency** needs a Linux host with isolated, pinned cores (`--pin` is
  implemented for Linux). The macOS tails in the benchmarks are indicative only.
* **Fill model.** No market impact; queue position is estimated from
  displayed size.
* **Order entry** to the simulator is in-process by design (determinism); a
  TCP order gateway would be a separate, non-deterministic mode.
