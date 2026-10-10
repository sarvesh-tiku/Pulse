# Design decisions

Each section is a question an interviewer is likely to ask, the decision
Pulse makes, and the trade-off. Code references point at the implementation.

---

## Why separate feed decoding from book reconstruction?

They have different failure modes and different costs.

* The **feed thread** owns everything about *the wire*: framing, checksums,
  sequence numbers, retransmission requests, and recording. Its job is to
  turn an unreliable datagram stream into a gap-free, validated, ordered
  stream of `MarketEvent`s (`market_data/feed_handler.hpp`). If it stalls, the
  kernel socket buffer fills and the venue's packets are dropped, so it must
  never do slow work — which is why recording is pushed to its own thread and
  retransmission is the only blocking call (see "What happens when a consumer
  cannot keep up").
* The **book thread** owns *state*: per-instrument books, top-of-book
  snapshots, conflation (`engine/book_builder.hpp`).

The split also creates a natural pipeline boundary. With both on one thread
the per-event cost is the sum; split, throughput is bounded by the slower
stage. Measured (inline pipeline stage breakdown, see
[benchmarks.md](benchmarks.md)): the feed handler is ~17 ns/event and the book
stage ~43 ns/event, so the book stage is the bottleneck and the feed thread
spends most of its time waiting on a full queue at max speed.

## Why SPSC and not MPMC?

Because the topology is a pipeline: every queue has exactly one writer and
one reader. An SPSC ring needs no CAS loops and no per-slot sequence numbers —
each side owns one index and publishes it with a release store
(`concurrency/spsc_ring.hpp`). An MPMC queue would pay for contention that
cannot happen here.

When would MPMC be needed? If several feed lines (A/B arbitration, multiple
multicast groups) fed one book thread, or several strategy threads shared one
gateway. The usual answer is still *not* MPMC but N SPSC queues polled by the
consumer, which keeps each queue single-writer and preserves per-source order.

## Is lock-free automatically faster?

No, and the benchmarks show where it isn't:

* An uncontended `std::mutex` lock/unlock is cheap. The real cost of the
  mutex queue is (a) both threads writing the same cache lines (the lock word
  and the indices), and (b) condition-variable *wake-ups* when a consumer has
  gone to sleep: a syscall plus a context switch, microseconds each.
* Lock-free does not remove cache-coherence traffic. A naive SPSC ring whose
  producer reads the consumer's index on every push (and vice versa) still
  ping-pongs those cache lines between cores. The `SpscNoCache` variant exists
  to measure exactly that.
* Batching can matter more than the queue algorithm: the consumer popping up to
  64 items and publishing `head` once amortises the hand-off.

See the queue tables in [benchmarks.md](benchmarks.md) for the measured
numbers, including where the mutex queue is competitive.

## False sharing and cache lines

`head_` (written by the consumer) and `tail_` (written by the producer) sit on
separate 64-byte lines (`alignas(kCacheLine)`), each next to the *owning*
side's private cached copy of the other index. Without padding, every push
invalidates the line the consumer is reading and every pop invalidates the
producer's — two cores fighting over one line even though they never touch
the same variable. `SpscRing<T, /*Padded=*/false>` is kept in the codebase so
this can be measured, not just asserted.

Note on Apple Silicon: the L2 line is 128 bytes, but false sharing is decided
at the L1/coherence granule; 64 bytes is used and documented in
`core/cpu.hpp`.

## Memory ordering

The producer writes the slot, then `tail.store(release)`. The consumer does
`tail.load(acquire)`, reads the slot, then `head.store(release)`. The producer
does `head.load(acquire)` before reusing a slot. Each release/acquire pair
creates the happens-before edge that makes the slot contents visible; no
`seq_cst` is needed with one producer and one consumer. The indices grow
monotonically and are masked on access, so "full" is `tail - head ==
capacity` and every slot is usable.

The seqlock (`concurrency/seqlock.hpp`) uses the fence-based pattern: odd
sequence → release fence → data stores → even sequence (release); readers load
the sequence (acquire), copy, acquire fence, re-check. The payload is stored
as relaxed atomic words so the reader's racy copy is not a data race (and
ThreadSanitizer agrees — the TSan CI job runs the seqlock stress test).

## Bounded vs unbounded queues; back-pressure

All queues are bounded. An unbounded queue turns a slow consumer into
unbounded memory growth and unbounded latency — the failure just shows up
later and worse. With a bounded queue the producer finds out immediately:
`SpscRing::push` spins (then yields) while full and returns the number of
failed attempts, which the pipeline reports as `full-spins`.

## What happens when one consumer cannot keep up?

It depends on the stage, and the answer is different for each:

* **Book thread slower than feed thread** (the normal case at max replay
  speed): the feed thread blocks on Q1. For a *file* that is harmless
  back-pressure. For a *live UDP feed* it means the socket buffer absorbs the
  burst (Pulse requests an 8 MB `SO_RCVBUF`); if that overflows, the kernel
  drops datagrams, the sequencer sees gaps, and recovery kicks in. That is the
  realistic behaviour and it is observable in the report.
* **Core slower than book thread**: Q2 fills and the book thread blocks. The
  book stage already conflates events that do not change the visible top of
  book, which is the standard way to shed load on market data without losing
  correctness (state is current; intermediate states are skipped).
* **Recorder slower than feed**: it has its own 64K-slot queue so disk I/O
  never touches the feed thread's path except through that queue.

## How is ordering preserved?

Sequence numbers at the edge, FIFO queues in between, single-threaded state
in the middle. The sequencer (`market_data/sequencer.hpp`) delivers strictly
in sequence order, buffering out-of-order arrivals in a fixed power-of-two
window indexed by `seq & (W-1)`. Every downstream stage is FIFO with one
producer and one consumer. Feed-health changes (`FeedGap`, `FeedRecovered`,
`FeedStale`) are **in-band** events on the same queues, so the core sees "gap
opened" exactly between the last good event and the first event after it.

## What can run concurrently? Where are the consistency boundaries?

* Parallel across **stages** (feed, book, core, recorder, dashboard).
* Parallel across **instruments** is possible for the book stage (books are
  independent) and it *is* the throughput bottleneck today, but it is not done
  yet: the core needs a single global order of events to be deterministic, so
  sharded book threads would need a deterministic merge (by channel sequence)
  in front of the core. That is the next scaling step — see the last section.
* **Not** parallel inside the core. Strategy, risk, OMS, positions and the
  venue simulator share state that must be mutually consistent: risk needs
  positions *and* open orders at the instant an order is checked; positions
  depend on fills; the strategy reads all of it. That is the consistency
  boundary: everything inside `TradingCore` is one thread (the LMAX "business
  logic processor" pattern). Telemetry reads it through a seqlock snapshot,
  never directly.

## Why deterministic replay, and how is it achieved?

Because production incidents in trading systems are almost always about a
specific sequence of events. If you can replay the exact input and get the
exact same decisions, a bug becomes a unit test.

How:
1. **Record raw input at the edge**: every datagram, including duplicates,
   corrupt frames and retransmissions, wrapped in a record header that stores
   the length (`market_data/session_file.hpp`). A corrupt length field can't
   desynchronise the reader.
2. **No wall clock in business logic**: the core runs on venue timestamps plus
   modelled latencies. Wall-clock time is only read for *measurement*.
3. **Seeded, portable randomness**: the venue simulator's jitter and rejects,
   the generator and the fault injector use `std::mt19937_64` with Pulse's own
   distributions (`core/random.hpp`). Standard-library distributions are
   implementation-defined — libstdc++ and libc++ produced different sessions
   from the same seed until this was fixed — and the library is built with
   `-ffp-contract=off` so FMA contraction can't change results.
4. **Deterministic iteration**: anything iterated from an unordered container
   on the decision path is sorted first (`OrderManager::live_orders`).
5. **A digest** over every observable decision: book states, risk verdicts,
   execution reports, fills, final positions (`engine/digest.hpp`).

Tests assert: inline == threaded (all 4 queue types) == all 3 book types ==
replay of the recording (`tests/test_pipeline.cpp`). CI also checks the
digest of a `run` against its `replay` from the command line.

## Why is the venue simulated in-process instead of over TCP?

Determinism. A real TCP gateway puts wall-clock timing and scheduler noise
into the order/execution path, and then replay no longer reproduces fills.
The simulator models the network instead: one-way latency, jitter, and either
a single ordered session (TCP-like) or independent paths where reports can
overtake each other. The OMS is written to survive the second mode (fills
before acks, fills after cancel acks, duplicates). Market data, where the real
networking problems live (loss, reordering, retransmission), *does* go over
real sockets (`pulse-feed` / `pulse listen`).

## Where should state live?

* Venue state (resting orders, queue position): the venue simulator.
* Client view of orders: the OMS, which treats the venue as the source of
  truth for *fills* and its own state as a best-effort view.
* Positions: derived **only** from fills, never from order state. A fill that
  arrives after a cancel acknowledgement (a cancel/fill race) still updates
  the position — the OMS counts it as a `late_fill` but applies it.
* Market state: the book thread owns the books; the core only sees immutable
  `BookUpdate` snapshots.

## Why a price-indexed array book?

The first version used `std::map<Price, Level>` + `std::list` + `std::unordered_map`
(still in the tree as `MapOrderBook`, used as the reference model). It is
simple and correct, but every add allocates (list node, hash node, sometimes a
tree node) and every lookup chases pointers.

The instruments have a bounded price band, so every legal price maps to a slot:
`(price - min) / tick`. `ArrayOrderBook` uses:
* a flat array of levels per side (O(1), adjacent prices adjacent in memory),
* a bitmap of non-empty levels to find the next best level with
  `countl_zero`/`countr_zero` over 64-level words when the top empties,
* an intrusive FIFO threaded through a pooled array of order nodes (32-bit
  indices, free list), and
* an open-addressing order-id index (`core/flat_id_map.hpp`) with linear
  probing, Fibonacci hashing and backward-shift deletion (no tombstones, so
  probe lengths don't degrade under add/cancel churn).

Measured effect on one instrument's L3 stream: see the book tables in
[benchmarks.md](benchmarks.md) (≈49 → ≈13 ns/event, ~0.9 → 0 heap allocations
per event). The `ArrayOrderBook<StdIndex>` variant isolates how much of that
is the level layout versus the hash map.

Trade-off: memory is O(band width), not O(live levels). An instrument with no
meaningful price band would need a hybrid: array around the touch, tree for
the tails.

## The cache-footprint lesson

The book micro-benchmark (one instrument) said 13 ns/event, but in the full
8-instrument pipeline the book stage cost ~78 ns/event. The cause was the
default sizing: each book pre-reserved an index for 65,536 orders — a 2 MB
hash table plus 2 MB of node pool *per instrument*, ~30 MB in total, randomly
accessed. Live orders per instrument are ~300. Sizing the index to 1,024 (it
grows on demand) cut the book stage to ~48 ns/event. Lesson: micro-benchmarks
with one instance hide working-set effects; measure the composed system.

## The digest lesson

The first digest was byte-wise FNV-1a: 8 dependent multiplies per 64-bit word
and ~7 words per published update — ~50 serial multiplies on every event. It
was a measurable fraction of the core's per-event budget. It now scrambles
each word with a splitmix64 finalizer (independent of the running state, so it
pipelines) and folds it in with one dependent multiply.

## Why fixed-point prices?

Floating-point prices can't represent most decimal ticks exactly, so equality
at a price level (the entire premise of a price-level book) becomes fragile,
and P&L accumulates rounding error. Prices are `int64` in units of 1e-4;
money is `int64` in the same units × shares. Average-cost accounting keeps the
integer-division remainder in `open_cost` so that when a position goes flat,
realized P&L equals the exact cash flow (a property test checks this over
random fill sequences).

## Why one recovery path for loss and corruption?

A frame that fails its CRC32C is dropped at decode time. The sequencer then
sees a missing sequence number — indistinguishable from packet loss — and the
same retransmission path fills it. There is exactly one recovery mechanism to
reason about and test. The fault-injection test runs drops, burst drops,
duplicates, reorders, corruption and truncation together and asserts that the
recovered stream equals the clean one event-for-event.

Subtle bug caught by a test: a flag bit (`kFlagNoChecksum`) lets frames skip
verification. A single bit flip in the flags field would therefore have
disabled checksum verification for that frame. The decoder now rejects any
frame that claims "no checksum" but has a non-zero checksum field.

## Gap policy: when to pull orders

First version: every gap cancelled all working orders. Over real UDP with
reordering, *every* one-frame reorder is a gap, so the system cancelled
everything hundreds of times a second, the cancels exhausted the message rate
budget, and the throttle tripped the kill switch — a self-inflicted outage.

Current policy:
* any open gap → **new orders blocked** (`MarketDataStale`) until recovered;
* gap of ≥ 64 messages (configurable) → also **pull working orders**, because a
  burst loss can hide a sweep through our price;
* feed **Stale** (unrecoverable) → pull everything, halt for the session.

## Risk: why "worst case" position?

`position + all open buy quantity + this order ≤ limit`. Checking only the
current position lets a strategy with many working orders blow through the
limit when they all fill at once. Pending replaces count at the larger of old
and new size, because the replace may or may not be applied.

## Strategy retry storms

The market maker originally only throttled itself after a *successful*
request. When risk rejected it, it retried on every tick, hit the rate
limiter, and tripped the kill switch. Throttling now applies to *attempts*.
The runaway strategy (`--strategy runaway`) reproduces the failure on purpose
to show the risk layer containing it.

## How would this change at 100× traffic?

* **Feed**: kernel UDP becomes the bottleneck first. Next steps: `recvmmsg`
  batching, busy-polling sockets, then kernel bypass (ef_vi / DPDK) with
  hardware timestamps. Multiple multicast groups → one feed thread per group,
  merged by sequence per group.
* **Book stage**: shard instruments across book threads (books are
  independent). The core then consumes N SPSC queues and must merge them; to
  keep determinism, merge by (venue timestamp, channel, sequence).
* **Core**: stays single-threaded per *strategy group*; scale by running
  independent groups on separate cores with separate risk budgets and a
  shared, lock-free firm-wide risk aggregate.
* **Memory**: huge pages for the books and rings; pin threads to isolated
  cores (`--pin` does the pinning on Linux); NUMA-local allocation.
* **Measurement**: switch from `steady_clock` to calibrated TSC reads and
  hardware NIC timestamps; record latency histograms per stage continuously
  rather than per run.
