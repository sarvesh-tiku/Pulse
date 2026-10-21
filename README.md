# Pulse

A low-latency market data, execution, and risk engine in C++20.

Pulse processes binary feeds, reconstructs L3 order books, and runs strategies through pre-trade risk checks and simulated execution. Deterministic record/replay makes failures reproducible.

## Performance

Measured on Apple M1 Pro. Each row represents a separate benchmark.

| Measurement | Result |
|:--|:--|
| Inline throughput across 8 instruments | 11.0M events/s |
| Threaded pipeline at 1M messages/s | 0.92 µs p50 · 1.6 µs p99 |
| L3 order-book updates | 10–13 ns/event |
| Missing messages recovered in fault-injection session | 1,896 / 1,896 |

Inline execution achieved the highest throughput; threading provides stage isolation at the cost of queue handoffs. See [benchmark methodology](docs/benchmarks.md).

## Features

- **Feed recovery:** CRC32C validation, sequencing, duplicate filtering, and TCP rewind. Unrecoverable gaps halt trading.
- **L3 order books:** Price-indexed arrays, pooled FIFO queues, and direct order lookup with zero hot-path heap allocations.
- **Execution and risk:** Order lifecycle management, pre-trade limits, kill switches, and exactly-once fill accounting under duplicate or reordered reports.
- **Deterministic replay:** Record recovered events and reproduce engine state across book implementations and queue configurations.
- **Validation:** 118 tests covering differential checks, invariants, and injected failures; GCC, Clang, and Linux sanitizer checks.

## Build and Run

Requires CMake ≥ 3.24, Clang ≥ 15 or GCC ≥ 12, and a POSIX environment.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release

# Generate a feed
build/release/pulse-gen --out session.pulse --events 1M

# Run strategies and record
build/release/pulse run session.pulse \
  --strategy market-maker,imbalance --record recorded.pulse

# Replay
build/release/pulse replay recorded.pulse --max-speed
```

## Scope

Orders execute against an in-process simulated venue. Snapshot recovery, a live exchange gateway, kernel bypass, and market-impact modeling are outside the current scope. Latency figures measure the local pipeline, not production wire-to-wire execution.

## Documentation

- [Architecture](docs/architecture.md)
- [Design Decisions](docs/design-decisions.md)
- [Benchmarks](docs/benchmarks.md)
- [Failure Handling](docs/failure-handling.md)
