# Architecture

Pulse is a miniature version of the infrastructure between an exchange feed
and a trading strategy: binary market-data ingestion, order-book
reconstruction, a strategy/risk/order-management core, a simulated venue, and
the tooling to record, replay, measure and break it.

```
          pulse-feed (UDP publisher + TCP rewind server)   or   session file (mmap)
                                   │ datagrams
┌──────────────────────────────────▼───────────────────────────────────────────┐
│ FEED THREAD                                                                   │
│   decode ─► CRC32C ─► sequencer (gap detect / buffer / dedupe) ─► validate    │
│                ▲                    │ gap                                     │
│                └── retransmission ◄─┘  (TcpRewindClient / SessionRewindStore) │
│   every datagram (incl. retransmits) ─► recorder queue ─► RECORDER THREAD     │
└──────────────────────────────────┬───────────────────────────────────────────┘
                                   │ Q1: SPSC ring of MarketEvent (56 B)
┌──────────────────────────────────▼───────────────────────────────────────────┐
│ BOOK THREAD                                                                   │
│   one L3 book per instrument (price-indexed array + pooled FIFO + flat index)│
│   top-5 snapshot, conflation of invisible changes, trade cross-check          │
└──────────────────────────────────┬───────────────────────────────────────────┘
                                   │ Q2: SPSC ring of BookUpdate (192 B, 3 lines)
┌──────────────────────────────────▼───────────────────────────────────────────┐
│ CORE THREAD (deterministic, simulated venue time)                             │
│   venue sim advance ─► strategies ─► pre-trade risk ─► OMS ─► venue sim       │
│        ▲                                                   │                   │
│        └──────── execution reports (latency, jitter) ◄─────┘                   │
│   positions / P&L ─► seqlock snapshot ─► DASHBOARD THREAD                      │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Components

| Layer | Code | Responsibility |
|---|---|---|
| Protocol | `protocol/wire.hpp`, `codec.cpp`, `checksum.cpp` | PMDP v1 binary frames, CRC32C (ARMv8 / SSE4.2 instructions, table fallback) |
| Feed handler | `market_data/feed_handler.hpp` | decode → checksum → sequence → validate; one recovery path for loss *and* corruption |
| Sequencer | `market_data/sequencer.hpp` | gap detection, fixed-window reorder buffer, duplicates, heartbeats, Stale on unrecoverable loss |
| Rewind | `market_data/rewind.hpp`, `net/retransmit.cpp` | retransmission from a clean session or over TCP |
| Generator | `market_data/generator.cpp` | valid synthetic L3 venue: never crosses, trades hit the front of the best level |
| Faults | `market_data/fault_injector.cpp` | seeded drop / burst drop / duplicate / reorder / corrupt / truncate |
| Sessions | `market_data/session_file.cpp` | record format wrapping raw datagrams; mmap reader |
| Books | `orderbook/` | `MapOrderBook` (reference), `ArrayOrderBook<Index>` (optimized) behind one concept |
| Book stage | `engine/book_builder.hpp` | per-instrument books, top-5 publish, conflation |
| Concurrency | `concurrency/` | `SpscRing` (padding / cached-index knobs), `MutexQueue`, `SeqLock`, `Worker` |
| Core | `engine/trading_core.cpp` | strategies, risk, OMS, venue sim, P&L, incidents, digest |
| Venue | `execution/exchange_simulator.cpp` | latency + jitter, FIFO or reordering report paths, queue-position fills, rejects, disconnects, cancel-on-disconnect |
| OMS | `execution/order_manager.cpp` | order state machine tolerant of out-of-order, duplicate and late reports |
| Risk | `risk/risk_engine.cpp` | kill switch, health gates, size/notional/collar, worst-case position, open orders, token-bucket rate, auto-trip |
| Portfolio | `portfolio/position.cpp` | average-cost positions, realized/unrealized P&L in fixed point |
| Telemetry | `telemetry/` | log-linear latency histograms, live dashboard |
| Pipeline | `engine/pipeline.cpp` | inline and threaded wiring, pacing, recording, reports |

## Data model

* **Prices** are `int64` fixed point (1e-4), as in ITCH. Floating point never
  touches the book, risk limits on price, or the P&L ledger.
* **MarketEvent** (56 bytes, trivially copyable) is the normalized event on
  Q1. Internal feed-health events (`FeedGap`, `FeedRecovered`, `FeedStale`)
  travel *in-band* on the same queue, so downstream stages learn about gaps
  in exactly the order they happened.
* **BookUpdate** (192 bytes, `alignas(64)`) carries a top-5 L2 snapshot, the
  trade (if any) and health flags.

## Threads and ordering

Each queue has exactly one producer and one consumer, and every stage is
FIFO, so the core observes exactly the sequence of BookUpdates it would see if
everything ran on one thread. The core itself is single-threaded and runs on
**simulated venue time** (market-data timestamps plus modelled latencies),
never on the wall clock. Consequence: the inline pipeline, the threaded
pipeline with any queue type, and the replay of a recording all produce the
same 64-bit digest, and the tests assert it.

## Where the time goes (inline, 8 instruments, M1 Pro)

Cumulative per-event cost from `pulse-micro --benchmark_filter=Stages` (see
[benchmarks.md](benchmarks.md) for the full tables and how they were produced):

| Stage | ns/event |
|---|---:|
| read records (mmap / memory) | ~4.5 |
| + feed handler (decode, CRC32C, sequencer, validation) | ~22 |
| + book builder (books, top-5 snapshot, conflation, digest) | ~65 |
| + trading core (no strategy) | ~79 |
| + trading core with the market maker | ~97 |

## What is deliberately *not* here

* No real exchange connectivity or FIX; the order gateway is in-process
  against a simulator. That is what makes the core deterministic.
* No alpha. The strategies exist to generate realistic load: resting orders,
  cancel/replace races, partial fills, IOCs, inventory limits.
* No kernel bypass (DPDK / ef_vi / io_uring). The receive path is a plain
  POSIX UDP socket; the design keeps the socket read isolated in the feed
  thread so it could be swapped.
