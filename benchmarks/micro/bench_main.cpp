// Micro-benchmarks (Google Benchmark). Run:
//   ./pulse-micro --benchmark_min_time=0.5s
//   ./pulse-micro --benchmark_filter=Book

#include <benchmark/benchmark.h>

#include <array>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../alloc_counter.hpp"
#include "pulse/concurrency/mutex_queue.hpp"
#include "pulse/concurrency/spsc_ring.hpp"
#include "pulse/core/clock.hpp"
#include "pulse/core/flat_id_map.hpp"
#include "pulse/engine/book_builder.hpp"
#include "pulse/engine/trading_core.hpp"
#include "pulse/market_data/feed_handler.hpp"
#include "pulse/market_data/session_file.hpp"
#include "pulse/market_data/generator.hpp"
#include "pulse/orderbook/array_order_book.hpp"
#include "pulse/orderbook/map_order_book.hpp"
#include "pulse/protocol/checksum.hpp"
#include "pulse/protocol/codec.hpp"
#include "pulse/telemetry/histogram.hpp"

using namespace pulse;

namespace {

// Pre-generated event streams per workload profile (generation excluded from timing).
const std::vector<MarketEvent>& events_for(md::WorkloadProfile p) {
  static std::array<std::vector<MarketEvent>, 5> cache;
  auto& v = cache[static_cast<std::size_t>(p)];
  if (v.empty()) {
    md::GeneratorConfig gc;
    gc.events = 1'000'000;
    gc.instruments = 1;
    gc.profile = p;
    gc.seed = 7;
    md::FeedGenerator gen(gc, InstrumentUniverse::synthetic(1));
    MarketEvent e;
    v.reserve(gc.events);
    while (gen.next(e))
      if (e.type != EventType::Heartbeat) v.push_back(e);
  }
  return v;
}

template <class Book>
void apply(Book& b, const MarketEvent& e) {
  switch (e.type) {
    case EventType::AddOrder: b.add(e.order_id, e.side, e.price, e.qty); break;
    case EventType::CancelOrder: b.cancel(e.order_id); break;
    case EventType::ModifyOrder: b.modify(e.order_id, e.price, e.qty); break;
    case EventType::Trade: b.execute(e.order_id, e.qty); break;
    default: break;
  }
}

// Book update cost: replay one instrument's L3 stream, then read top-of-book
// (what the book stage does per event).
template <class Book>
void BM_Book(benchmark::State& st) {
  const auto profile = static_cast<md::WorkloadProfile>(st.range(0));
  const auto& ev = events_for(profile);
  const auto spec = InstrumentUniverse::synthetic(1)[0];
  std::uint64_t allocs = 0, processed = 0;
  for (auto _ : st) {
    st.PauseTiming();
    auto b = std::make_unique<Book>(spec);
    const auto a0 = bench::allocations();
    st.ResumeTiming();
    for (const auto& e : ev) {
      apply(*b, e);
      benchmark::DoNotOptimize(b->best_bid());
    }
    st.PauseTiming();
    allocs += bench::allocations() - a0;
    processed += ev.size();
    b.reset();
    st.ResumeTiming();
  }
  st.SetItemsProcessed(static_cast<std::int64_t>(processed));
  st.counters["allocs/event"] = static_cast<double>(allocs) / static_cast<double>(processed);
  st.counters["ns/event"] = benchmark::Counter(static_cast<double>(processed),
                                               benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  st.SetLabel(md::to_string(profile));
}

#define BOOK_ARGS ->DenseRange(0, 3)->Unit(benchmark::kMillisecond)
BENCHMARK(BM_Book<book::MapOrderBook>) BOOK_ARGS;
BENCHMARK(BM_Book<book::ArrayOrderBook<book::StdIndex>>) BOOK_ARGS;
BENCHMARK(BM_Book<book::ArrayOrderBook<book::FlatIndex>>) BOOK_ARGS;

// Top-5 depth snapshot (published by the book stage when the top changes).
template <class Book>
void BM_Depth5(benchmark::State& st) {
  const auto& ev = events_for(md::WorkloadProfile::Balanced);
  const auto spec = InstrumentUniverse::synthetic(1)[0];
  Book b(spec);
  for (std::size_t i = 0; i < 200'000; ++i) apply(b, ev[i]);
  std::array<book::Level, 5> lv;
  for (auto _ : st) {
    benchmark::DoNotOptimize(b.depth(Side::Buy, lv));
    benchmark::DoNotOptimize(b.depth(Side::Sell, lv));
  }
}
BENCHMARK(BM_Depth5<book::MapOrderBook>);
BENCHMARK(BM_Depth5<book::ArrayOrderBook<book::FlatIndex>>);

// ---------------------------------------------------------------------------
void BM_Decode(benchmark::State& st) {
  const bool verify = st.range(0) != 0;
  const auto& ev = events_for(md::WorkloadProfile::Balanced);
  std::vector<std::byte> buf(ev.size() * protocol::kMaxFrameSize);
  std::vector<std::size_t> offs;
  std::size_t pos = 0;
  for (std::size_t i = 0; i < 100'000; ++i) {
    offs.push_back(pos);
    pos += protocol::encode(ev[i], {buf.data() + pos, protocol::kMaxFrameSize});
  }
  MarketEvent out;
  std::size_t i = 0;
  for (auto _ : st) {
    auto r = protocol::decode({buf.data() + offs[i], protocol::kMaxFrameSize}, out, verify);
    benchmark::DoNotOptimize(r);
    if (++i == offs.size()) i = 0;
  }
  st.SetItemsProcessed(st.iterations());
  st.SetLabel(verify ? "crc32c verified" : "no checksum");
}
BENCHMARK(BM_Decode)->Arg(0)->Arg(1);

void BM_Crc32c(benchmark::State& st) {
  std::vector<std::byte> b(56, std::byte{0x5A});
  const bool hw = st.range(0) != 0;
  for (auto _ : st) {
    benchmark::DoNotOptimize(hw ? protocol::crc32c(0, b) : protocol::crc32c_software(0, b));
  }
  st.SetBytesProcessed(st.iterations() * 56);
  st.SetLabel(hw ? (protocol::crc32c_hardware_available() ? "hardware" : "hardware(unavailable->sw)") : "table");
}
BENCHMARK(BM_Crc32c)->Arg(0)->Arg(1);

// ---------------------------------------------------------------------------
template <class Map>
void BM_IdIndex(benchmark::State& st) {
  // Churn typical of an order index: insert new ids, look up, erase old ones,
  // with ~20k live entries.
  std::mt19937_64 rng(1);
  Map m{};
  std::uint64_t next = 1, oldest = 1;
  for (; next <= 20'000; ++next) m.insert(next, static_cast<std::uint32_t>(next));
  for (auto _ : st) {
    m.insert(next, static_cast<std::uint32_t>(next));
    ++next;
    benchmark::DoNotOptimize(m.find(oldest + rng() % (next - oldest)));
    m.erase(oldest++);
  }
  st.SetItemsProcessed(st.iterations());
}
struct StdMapAdapter {
  std::unordered_map<std::uint64_t, std::uint32_t> m;
  void insert(std::uint64_t k, std::uint32_t v) { m.emplace(k, v); }
  std::uint32_t find(std::uint64_t k) const {
    auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
  }
  void erase(std::uint64_t k) { m.erase(k); }
};
struct FlatAdapter {
  FlatIdMap m{1 << 15};
  void insert(std::uint64_t k, std::uint32_t v) { m.insert(k, v); }
  std::uint32_t find(std::uint64_t k) const { return m.find(k); }
  void erase(std::uint64_t k) { m.erase(k); }
};
BENCHMARK(BM_IdIndex<StdMapAdapter>);
BENCHMARK(BM_IdIndex<FlatAdapter>);

// ---------------------------------------------------------------------------
// Queue throughput: one producer thread, one consumer (the benchmark thread).
template <class Q>
void BM_QueueThroughput(benchmark::State& st) {
  const auto batch = static_cast<std::size_t>(st.range(0));
  constexpr std::uint64_t kItems = 2'000'000;
  std::uint64_t total = 0;
  for (auto _ : st) {
    Q q(4096);
    std::thread prod([&] {
      MarketEvent e{};
      for (std::uint64_t i = 0; i < kItems; ++i) {
        e.seq = i;
        q.push(e);
      }
    });
    std::uint64_t got = 0, sum = 0;
    while (got < kItems) {
      const auto n = q.pop_batch([&](const MarketEvent& e) { sum += e.seq; }, batch);
      if (n == 0) q.wait_nonempty(std::chrono::microseconds(50));
      got += n;
    }
    prod.join();
    benchmark::DoNotOptimize(sum);
    total += kItems;
  }
  st.SetItemsProcessed(static_cast<std::int64_t>(total));
}
#define Q_ARGS ->Arg(1)->Arg(64)->UseRealTime()->Unit(benchmark::kMillisecond)
BENCHMARK(BM_QueueThroughput<conc::MutexQueue<MarketEvent>>) Q_ARGS;
BENCHMARK(BM_QueueThroughput<conc::SpscRing<MarketEvent, false, false>>) Q_ARGS;
BENCHMARK(BM_QueueThroughput<conc::SpscRing<MarketEvent, true, false>>) Q_ARGS;
BENCHMARK(BM_QueueThroughput<conc::SpscRing<MarketEvent, false, true>>) Q_ARGS;
BENCHMARK(BM_QueueThroughput<conc::SpscRing<MarketEvent, true, true>>) Q_ARGS;

// Queue hand-off latency: ping-pong over two queues, report one-way = RTT/2.
template <class Q>
void BM_QueuePingPong(benchmark::State& st) {
  Q ping(1024), pong(1024);
  std::atomic<bool> stop{false};
  std::thread echo([&] {
    MarketEvent e;
    while (!stop.load(std::memory_order_relaxed)) {
      if (ping.try_pop(e)) {
        pong.push(e);
      } else {
        cpu_relax();
      }
    }
  });
  telemetry::LatencyHistogram h;
  MarketEvent e{};
  for (auto _ : st) {
    const auto t0 = now_ns();
    ping.push(e);
    while (!pong.try_pop(e)) cpu_relax();
    h.record((now_ns() - t0) / 2);
  }
  stop = true;
  echo.join();
  st.counters["p50_ns"] = static_cast<double>(h.percentile(50));
  st.counters["p99_ns"] = static_cast<double>(h.percentile(99));
  st.counters["p99.9_ns"] = static_cast<double>(h.percentile(99.9));
}
BENCHMARK(BM_QueuePingPong<conc::MutexQueue<MarketEvent>>)->UseRealTime();
BENCHMARK(BM_QueuePingPong<conc::SpscRing<MarketEvent, false, false>>)->UseRealTime();
BENCHMARK(BM_QueuePingPong<conc::SpscRing<MarketEvent, true, true>>)->UseRealTime();

// ---------------------------------------------------------------------------
// Cumulative pipeline stage cost (inline, in-memory session, 8 instruments):
//   0 read records   1 + decode/CRC/sequence/validate (FeedHandler)
//   2 + book build & top-5 snapshot   3 + trading core (strategy "none")
//   4 + trading core with market-maker
const std::vector<std::byte>& session_bytes() {
  static std::vector<std::byte> s = [] {
    md::GeneratorConfig gc;
    gc.events = 1'000'000;
    gc.instruments = 8;
    const auto uni = InstrumentUniverse::synthetic(8);
    md::FeedGenerator gen(gc, uni);
    md::MemorySessionBuilder b({uni, 0, 0, gc.start_ts});
    std::array<std::byte, protocol::kMaxFrameSize> buf{};
    MarketEvent ev;
    while (gen.next(ev)) b.write({buf.data(), protocol::encode(ev, buf)});
    return std::move(b).finish();
  }();
  return s;
}

void BM_PipelineStages(benchmark::State& st) {
  const int depth = static_cast<int>(st.range(0));
  static const char* kNames[] = {"read", "+feed handler", "+book builder", "+core(none)", "+core(mm)"};
  std::uint64_t events = 0;
  for (auto _ : st) {
    st.PauseTiming();
    auto reader = md::SessionReader::from_memory(session_bytes());
    const auto uni = reader.info().universe;
    md::FeedHandler handler(uni);
    engine::BookBuilder<book::FastOrderBook> builder(uni);
    std::vector<std::unique_ptr<strategy::Strategy>> strats;
    strats.push_back(strategy::make_strategy(depth >= 4 ? "market-maker" : "none", {}, uni.size()));
    engine::CoreConfig cc;
    cc.measure_latency = false;
    engine::TradingCore core(uni, cc, std::move(strats));
    BookUpdate upd;
    std::uint64_t sink = 0;
    st.ResumeTiming();
    md::Record r;
    auto on_event = [&](const MarketEvent& ev) {
      ++events;
      if (depth < 2) {
        sink += ev.seq;
        return;
      }
      if (builder.apply(ev, upd) && depth >= 3) core.on_book_update(upd);
    };
    while (reader.next(r)) {
      if (depth == 0) {
        sink += r.frame.size();
        ++events;
        continue;
      }
      handler.on_datagram(r.frame, 0, on_event);
    }
    benchmark::DoNotOptimize(sink);
  }
  st.counters["ns/event"] =
      benchmark::Counter(static_cast<double>(events), benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  st.SetLabel(kNames[depth]);
}
BENCHMARK(BM_PipelineStages)->DenseRange(0, 4)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
void BM_HistogramRecord(benchmark::State& st) {
  telemetry::LatencyHistogram h;
  std::uint64_t v = 12345;
  for (auto _ : st) {
    v = v * 6364136223846793005ull + 1442695040888963407ull;
    h.record(v >> 44);
  }
  benchmark::DoNotOptimize(h.count());
}
BENCHMARK(BM_HistogramRecord);

void BM_Clock(benchmark::State& st) {
  for (auto _ : st) benchmark::DoNotOptimize(now_ns());
}
BENCHMARK(BM_Clock);

}  // namespace

BENCHMARK_MAIN();
