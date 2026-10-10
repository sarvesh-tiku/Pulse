#include "pulse/engine/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#include "pulse/concurrency/mutex_queue.hpp"
#include "pulse/concurrency/spsc_ring.hpp"
#include "pulse/concurrency/thread.hpp"
#include "pulse/core/clock.hpp"
#include "pulse/orderbook/array_order_book.hpp"
#include "pulse/orderbook/map_order_book.hpp"
#include "pulse/protocol/wire.hpp"
#include "pulse/telemetry/dashboard.hpp"

namespace pulse::engine {

const char* to_string(QueueKind q) noexcept {
  switch (q) {
    case QueueKind::Spsc: return "spsc";
    case QueueKind::SpscNoCache: return "spsc-nocache";
    case QueueKind::SpscUnpadded: return "spsc-unpadded";
    case QueueKind::Mutex: return "mutex";
  }
  return "?";
}

const char* to_string(BookKind b) noexcept {
  switch (b) {
    case BookKind::Array: return "array";
    case BookKind::ArrayStdIndex: return "array-stdindex";
    case BookKind::Map: return "map";
  }
  return "?";
}

bool parse_queue(const std::string& s, QueueKind& out) noexcept {
  for (auto q : {QueueKind::Spsc, QueueKind::SpscNoCache, QueueKind::SpscUnpadded, QueueKind::Mutex})
    if (s == to_string(q)) return out = q, true;
  return false;
}

bool parse_book(const std::string& s, BookKind& out) noexcept {
  for (auto b : {BookKind::Array, BookKind::ArrayStdIndex, BookKind::Map})
    if (s == to_string(b)) return out = b, true;
  return false;
}

namespace {

// Raw frame for the recorder queue (datagrams are split into <= 64-byte
// frames; grouping does not affect replay semantics).
struct RawFrame {
  std::uint64_t capture_ns;
  std::uint16_t len;
  std::array<std::byte, protocol::kMaxFrameSize> data;
};

class Pacer {
 public:
  Pacer(double speed, double rate) : speed_(speed), rate_(rate) {}
  // Blocks until datagram number `i` with venue timestamp `ts` is due.
  void wait(std::uint64_t i, std::uint64_t ts) {
    if (speed_ <= 0 && rate_ <= 0) return;
    const auto now = now_ns();
    if (!started_) {
      started_ = true;
      t0_ = now;
    }
    std::uint64_t due;
    if (rate_ > 0) {
      due = t0_ + static_cast<std::uint64_t>(static_cast<double>(i) * 1e9 / rate_);
    } else {
      if (ts == 0) return;  // unvalidated frame: don't pace on it
      if (ts0_ == 0) ts0_ = ts;
      if (ts < ts0_) return;  // retransmissions carry older timestamps
      due = t0_ + static_cast<std::uint64_t>(static_cast<double>(ts - ts0_) / speed_);
    }
    while (true) {
      const auto t = now_ns();
      if (t >= due) break;
      if (due - t > 200'000) {
        std::this_thread::sleep_for(std::chrono::microseconds((due - t) / 2000));
      } else {
        cpu_relax();
      }
    }
  }

 private:
  double speed_, rate_;
  bool started_ = false;
  std::uint64_t t0_ = 0, ts0_ = 0;
};

// Venue timestamp used for replay pacing. Only trusted if the frame passes
// decode + CRC: a corrupted timestamp would otherwise schedule the next
// datagram centuries in the future and stall the replay. Returns 0 (= don't
// wait) for anything that doesn't validate.
std::uint64_t pacing_ts(std::span<const std::byte> b) {
  MarketEvent ev;
  if (protocol::decode(b, ev).status != protocol::DecodeStatus::Ok) return 0;
  return ev.exchange_ts;
}

std::vector<std::unique_ptr<strategy::Strategy>> build_strategies(const PipelineConfig& cfg, std::size_t n) {
  std::vector<std::unique_ptr<strategy::Strategy>> v;
  for (const auto& name : cfg.strategies) v.push_back(strategy::make_strategy(name, cfg.params, n));
  return v;
}

template <class Book>
void collect(RunReport& r, const md::FeedHandler& h, const BookBuilder<Book>& b, const TradingCore& core) {
  r.feed = h.stats();
  r.seq = h.sequencer().stats();
  r.books = b.stats();
  r.events = b.stats().events;
  r.book_updates = b.stats().published;
  r.datagrams = h.stats().datagrams;
  r.core = core.stats();
  r.oms = core.oms().stats();
  r.exchange = core.exchange().stats();
  r.risk = core.risk().counts();
  r.killed = core.risk().killed();
  r.kill_reason = core.risk().kill_reason();
  r.incidents = core.incidents();
  r.open_orders_at_end = core.oms().live_count();
  for (std::size_t i = 0; i < core.portfolio().size(); ++i)
    r.positions.push_back(core.portfolio()[static_cast<InstrumentId>(i)].qty());
  r.realized = core.portfolio().realized();
  r.unrealized = core.portfolio().unrealized();
  r.tick_to_order = core.tick_to_order();
  r.book_digest = b.digest().value();
  r.core_digest = core.digest().value();
  Digest d;
  d.mix(r.book_digest);
  d.mix(r.core_digest);
  r.digest = d.value();
}

// Dashboard thread: reads counters and the core's seqlock snapshot.
template <class Q1, class Q2>
conc::Worker start_dashboard(const InstrumentUniverse& uni, const TradingCore& core,
                             const telemetry::LiveCounters& live, const Q1* q1, const Q2* q2) {
  return conc::Worker("pulse-dash", -1, [&uni, &core, &live, q1, q2](const std::atomic<bool>& stop) {
    const auto t0 = now_ns();
    std::uint64_t last_events = 0, last_t = t0;
    while (!stop.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      const auto t = now_ns();
      telemetry::DashboardView v;
      v.elapsed_s = static_cast<double>(t - t0) * 1e-9;
      v.events = live.events.load(std::memory_order_relaxed);
      v.events_per_sec = static_cast<double>(v.events - last_events) * 1e9 / static_cast<double>(t - last_t);
      last_events = v.events;
      last_t = t;
      v.p50_ns = live.p50_ns.load(std::memory_order_relaxed);
      v.p99_ns = live.p99_ns.load(std::memory_order_relaxed);
      v.feed_state = live.feed_state.load(std::memory_order_relaxed);
      v.gaps = live.gaps.load(std::memory_order_relaxed);
      if (q1) v.q1_util = static_cast<double>(q1->size_approx()) / static_cast<double>(q1->capacity());
      if (q2) v.q2_util = static_cast<double>(q2->size_approx()) / static_cast<double>(q2->capacity());
      const auto s = core.snapshot().load();
      const auto text = telemetry::render_dashboard(uni, s, v);
      std::fwrite(text.data(), 1, text.size(), stdout);
      std::fflush(stdout);
    }
  });
}

struct NoQueue {
  std::size_t size_approx() const { return 0; }
  std::size_t capacity() const { return 1; }
};

// ---------------------------------------------------------------------------
// Inline mode: one thread does everything, in order. This is the reference
// the threaded pipeline must match bit-for-bit (same digest).
// ---------------------------------------------------------------------------
template <class Book>
RunReport run_inline(const InstrumentUniverse& uni, DatagramSource& src, const PipelineConfig& cfg) {
  RunReport rep;
  rep.threaded = false;
  rep.book = cfg.book;
  md::FeedHandler handler(uni, cfg.feed, cfg.rewind);
  BookBuilder<Book> builder(uni);
  CoreConfig cc = cfg.core;
  cc.measure_latency = cfg.measure_latency;
  TradingCore core(uni, cc, build_strategies(cfg, uni.size()));
  telemetry::LiveCounters live;
  const bool measure = cfg.measure_latency;

  std::unique_ptr<md::SessionWriter> writer;
  if (!cfg.record_path.empty()) {
    writer = std::make_unique<md::SessionWriter>(cfg.record_path, md::SessionInfo{uni, 0, 0, 0});
    handler.set_record_hook([&](std::span<const std::byte> b, std::uint64_t ts) { writer->write(b, ts); });
  }

  conc::Worker dash;
  if (cfg.dashboard) dash = start_dashboard<NoQueue, NoQueue>(uni, core, live, nullptr, nullptr);

  BookUpdate upd;
  std::uint64_t updates = 0;
  auto on_event = [&](const MarketEvent& ev) {
    // Status events and end-of-stream recovery carry no ingress stamp.
    const bool stamped = measure && ev.ingress_ns != 0;
    if (!builder.apply(ev, upd)) {
      if (stamped) rep.feed_to_book.record(now_ns() - ev.ingress_ns);
      return;
    }
    if (stamped) {
      upd.book_ns = now_ns();
      rep.feed_to_book.record(upd.book_ns - ev.ingress_ns);
    }
    core.on_book_update(upd);
    if (stamped) rep.end_to_end.record(now_ns() - upd.ingress_ns);
    if ((++updates & 4095) == 0 && cfg.dashboard) {
      core.publish_snapshot();
      live.updates.store(updates, std::memory_order_relaxed);
      live.events.store(builder.stats().events, std::memory_order_relaxed);
      live.p50_ns.store(rep.end_to_end.percentile(50), std::memory_order_relaxed);
      live.p99_ns.store(rep.end_to_end.percentile(99), std::memory_order_relaxed);
      live.gaps.store(handler.sequencer().stats().gaps, std::memory_order_relaxed);
      live.feed_state.store(static_cast<std::uint8_t>(handler.sequencer().state()), std::memory_order_relaxed);
    }
  };

  Pacer pacer(cfg.speed, cfg.rate);
  Datagram d;
  std::uint64_t i = 0;
  const auto t0 = now_ns();
  while (src.next(d)) {
    if (!src.live()) pacer.wait(i, pacing_ts(d.bytes));
    ++i;
    const std::uint64_t ingress = measure ? (src.live() && d.capture_ns ? d.capture_ns : now_ns()) : 0;
    handler.on_datagram(d.bytes, ingress, on_event);
  }
  handler.finish(on_event);
  core.finish();
  rep.wall_ns = now_ns() - t0;
  if (writer) writer->close();
  dash.join();
  collect(rep, handler, builder, core);
  return rep;
}

// ---------------------------------------------------------------------------
// Threaded mode:
//
//   [feed thread]  source -> pace -> FeedHandler (decode/checksum/sequence)
//        | Q1: MarketEvent
//   [book thread]  BookBuilder -> conflate
//        | Q2: BookUpdate
//   [core thread]  TradingCore (strategy, risk, OMS, venue sim, P&L)
//
//   [recorder]     raw frames from the feed thread -> session file
//   [dashboard]    counters + seqlock snapshot -> terminal
//
// Each queue has exactly one producer and one consumer, which is what allows
// the SPSC ring. Every stage preserves order, so the core sees exactly the
// sequence it would see inline.
// ---------------------------------------------------------------------------
template <class Book, class Q1, class Q2>
RunReport run_threaded(const InstrumentUniverse& uni, DatagramSource& src, const PipelineConfig& cfg) {
  RunReport rep;
  rep.threaded = true;
  rep.queue = cfg.queue;
  rep.book = cfg.book;
  const bool measure = cfg.measure_latency;

  md::FeedHandler handler(uni, cfg.feed, cfg.rewind);
  BookBuilder<Book> builder(uni);
  CoreConfig cc = cfg.core;
  cc.measure_latency = measure;
  TradingCore core(uni, cc, build_strategies(cfg, uni.size()));
  Q1 q1(cfg.queue_capacity);
  Q2 q2(cfg.queue_capacity);
  telemetry::LiveCounters live;
  std::atomic<bool> feed_done{false}, book_done{false}, rec_done{false};
  std::uint64_t q1_full = 0, q2_full = 0;
  telemetry::LatencyHistogram feed_to_book;

  // Recorder: its own thread so disk I/O never stalls the feed.
  std::unique_ptr<conc::SpscRing<RawFrame>> recq;
  std::unique_ptr<md::SessionWriter> writer;
  conc::Worker recorder;
  if (!cfg.record_path.empty()) {
    recq = std::make_unique<conc::SpscRing<RawFrame>>(1 << 16);
    writer = std::make_unique<md::SessionWriter>(cfg.record_path, md::SessionInfo{uni, 0, 0, 0});
    handler.set_record_hook([&](std::span<const std::byte> b, std::uint64_t ts) {
      for (std::size_t pos = 0; pos < b.size();) {
        RawFrame f{};
        f.capture_ns = ts;
        const std::size_t n = std::min(b.size() - pos, f.data.size());
        // Split on frame boundaries when the length field is sane.
        std::size_t take = n;
        if (b.size() - pos >= 2) {
          std::uint16_t len;
          std::memcpy(&len, b.data() + pos, 2);
          if (len >= protocol::kHeaderSize && len <= n) take = len;
        }
        f.len = static_cast<std::uint16_t>(take);
        std::memcpy(f.data.data(), b.data() + pos, take);
        recq->push(f);
        pos += take;
      }
    });
    recorder = conc::Worker("pulse-rec", -1, [&](const std::atomic<bool>&) {
      auto write = [&](const RawFrame& f) { writer->write({f.data.data(), f.len}, f.capture_ns); };
      while (true) {
        if (recq->pop_batch(write, 256) == 0) {
          if (rec_done.load(std::memory_order_acquire) && recq->size_approx() == 0) break;
          recq->wait_nonempty(std::chrono::microseconds(1000));
        }
      }
    });
  }

  conc::Worker dash;
  if (cfg.dashboard) dash = start_dashboard(uni, core, live, &q1, &q2);

  const auto t0 = now_ns();

  conc::Worker feed("pulse-feed", cfg.cpu_feed, [&](const std::atomic<bool>&) {
    Pacer pacer(cfg.speed, cfg.rate);
    auto push = [&](const MarketEvent& ev) { q1_full += q1.push(ev); };
    Datagram d;
    std::uint64_t i = 0;
    while (src.next(d)) {
      if (!src.live()) pacer.wait(i, pacing_ts(d.bytes));
      ++i;
      const std::uint64_t ingress = measure ? (src.live() && d.capture_ns ? d.capture_ns : now_ns()) : 0;
      handler.on_datagram(d.bytes, ingress, push);
      if ((i & 1023) == 0) {
        live.datagrams.store(i, std::memory_order_relaxed);
        live.gaps.store(handler.sequencer().stats().gaps, std::memory_order_relaxed);
        live.feed_state.store(static_cast<std::uint8_t>(handler.sequencer().state()), std::memory_order_relaxed);
      }
    }
    handler.finish(push);
    rec_done.store(true, std::memory_order_release);
    feed_done.store(true, std::memory_order_release);
  });

  conc::Worker book("pulse-book", cfg.cpu_book, [&](const std::atomic<bool>&) {
    BookUpdate upd;
    std::uint64_t n = 0;
    auto on_event = [&](const MarketEvent& ev) {
      const bool publish = builder.apply(ev, upd);
      if (measure) {
        const auto t = now_ns();
        if (ev.ingress_ns != 0) feed_to_book.record(t - ev.ingress_ns);
        upd.book_ns = t;
      }
      if (publish) q2_full += q2.push(upd);
      if ((++n & 1023) == 0) live.events.store(n, std::memory_order_relaxed);
    };
    while (true) {
      if (q1.pop_batch(on_event, cfg.batch) == 0) {
        if (feed_done.load(std::memory_order_acquire) && q1.size_approx() == 0) {
          // Re-check after observing done: the producer's last pushes are
          // visible once we have seen feed_done (release/acquire).
          if (q1.pop_batch(on_event, cfg.batch) == 0) break;
          continue;
        }
        q1.wait_nonempty(std::chrono::microseconds(500));
      }
    }
    live.events.store(n, std::memory_order_relaxed);
    book_done.store(true, std::memory_order_release);
  });

  // Core runs on this thread.
  {
    if (cfg.cpu_core >= 0) pin_current_thread(cfg.cpu_core);
    std::uint64_t n = 0;
    auto on_update = [&](const BookUpdate& u) {
      if (measure) rep.book_to_core.record(now_ns() - u.book_ns);
      core.on_book_update(u);
      if (measure && u.ingress_ns != 0) rep.end_to_end.record(now_ns() - u.ingress_ns);
      if ((++n & 4095) == 0 && cfg.dashboard) {
        core.publish_snapshot();
        live.updates.store(n, std::memory_order_relaxed);
        live.p50_ns.store(rep.end_to_end.percentile(50), std::memory_order_relaxed);
        live.p99_ns.store(rep.end_to_end.percentile(99), std::memory_order_relaxed);
      }
    };
    while (true) {
      if (q2.pop_batch(on_update, cfg.batch) == 0) {
        if (book_done.load(std::memory_order_acquire) && q2.size_approx() == 0) {
          if (q2.pop_batch(on_update, cfg.batch) == 0) break;
          continue;
        }
        q2.wait_nonempty(std::chrono::microseconds(500));
      }
    }
    core.finish();
  }
  rep.wall_ns = now_ns() - t0;
  feed.join();
  book.join();
  recorder.join();
  if (writer) writer->close();
  dash.join();
  rep.feed_to_book = feed_to_book;
  rep.feed_queue_full_spins = q1_full;
  rep.book_queue_full_spins = q2_full;
  collect(rep, handler, builder, core);
  return rep;
}

template <class Book>
RunReport dispatch_queue(const InstrumentUniverse& uni, DatagramSource& src, const PipelineConfig& cfg) {
  if (!cfg.threaded) return run_inline<Book>(uni, src, cfg);
  switch (cfg.queue) {
    case QueueKind::Spsc:
      return run_threaded<Book, conc::SpscRing<MarketEvent>, conc::SpscRing<BookUpdate>>(uni, src, cfg);
    case QueueKind::SpscNoCache:
      return run_threaded<Book, conc::SpscRing<MarketEvent, true, false>, conc::SpscRing<BookUpdate, true, false>>(
          uni, src, cfg);
    case QueueKind::SpscUnpadded:
      return run_threaded<Book, conc::SpscRing<MarketEvent, false, true>, conc::SpscRing<BookUpdate, false, true>>(
          uni, src, cfg);
    case QueueKind::Mutex:
      return run_threaded<Book, conc::MutexQueue<MarketEvent>, conc::MutexQueue<BookUpdate>>(uni, src, cfg);
  }
  return {};
}

}  // namespace

RunReport run_pipeline(const InstrumentUniverse& uni, DatagramSource& src, const PipelineConfig& cfg) {
  switch (cfg.book) {
    case BookKind::Array: return dispatch_queue<book::ArrayOrderBook<book::FlatIndex>>(uni, src, cfg);
    case BookKind::ArrayStdIndex: return dispatch_queue<book::ArrayOrderBook<book::StdIndex>>(uni, src, cfg);
    case BookKind::Map: return dispatch_queue<book::MapOrderBook>(uni, src, cfg);
  }
  return {};
}

// ---------------------------------------------------------------------------
namespace {
std::string money(std::int64_t m) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(m) / static_cast<double>(kPriceScale));
  return buf;
}
}  // namespace

std::string format_report(const RunReport& r, const InstrumentUniverse& uni) {
  std::ostringstream o;
  char b[256];
  o << "== Pulse run ==\n";
  std::snprintf(b, sizeof(b), "mode            %s%s%s, book=%s\n", r.threaded ? "threaded" : "inline",
                r.threaded ? ", queue=" : "", r.threaded ? to_string(r.queue) : "", to_string(r.book));
  o << b;
  std::snprintf(b, sizeof(b), "wall time       %.3f s\n", static_cast<double>(r.wall_ns) * 1e-9);
  o << b;
  std::snprintf(b, sizeof(b), "throughput      %.2fM events/s  (%llu events, %llu datagrams, %llu book updates)\n",
                r.events_per_sec() / 1e6, static_cast<unsigned long long>(r.events),
                static_cast<unsigned long long>(r.datagrams), static_cast<unsigned long long>(r.book_updates));
  o << b;
  o << "\n-- latency (wall clock) --\n";
  o << "feed -> book    " << r.feed_to_book.summary() << "\n";
  if (r.threaded) o << "book -> core    " << r.book_to_core.summary() << "\n";
  o << "end to end      " << r.end_to_end.summary() << "\n";
  o << "tick -> order   " << r.tick_to_order.summary() << "\n";
  if (r.threaded) {
    std::snprintf(b, sizeof(b), "back-pressure   feed->book full-spins %llu, book->core full-spins %llu\n",
                  static_cast<unsigned long long>(r.feed_queue_full_spins),
                  static_cast<unsigned long long>(r.book_queue_full_spins));
    o << b;
  }

  o << "\n-- feed --\n";
  std::snprintf(b, sizeof(b),
                "frames ok %llu | decode errors %llu (checksum %llu, truncated %llu, length %llu) | invalid %llu\n",
                static_cast<unsigned long long>(r.feed.frames_ok),
                static_cast<unsigned long long>(r.feed.decode_error_total()),
                static_cast<unsigned long long>(r.feed.decode_errors[static_cast<std::size_t>(protocol::DecodeStatus::BadChecksum)]),
                static_cast<unsigned long long>(r.feed.decode_errors[static_cast<std::size_t>(protocol::DecodeStatus::Truncated)]),
                static_cast<unsigned long long>(r.feed.decode_errors[static_cast<std::size_t>(protocol::DecodeStatus::BadLength)]),
                static_cast<unsigned long long>(r.feed.invalid));
  o << b;
  std::snprintf(b, sizeof(b),
                "sequencer: gaps %llu, missing %llu, recovered %llu, duplicates %llu, abandoned %llu, peak buffered %llu\n",
                static_cast<unsigned long long>(r.seq.gaps), static_cast<unsigned long long>(r.seq.missing),
                static_cast<unsigned long long>(r.seq.recovered), static_cast<unsigned long long>(r.seq.duplicates),
                static_cast<unsigned long long>(r.seq.abandoned), static_cast<unsigned long long>(r.seq.buffered_peak));
  o << b;
  std::snprintf(b, sizeof(b), "retransmit: requests %llu, frames %llu, failures %llu\n",
                static_cast<unsigned long long>(r.feed.retransmit_requests),
                static_cast<unsigned long long>(r.feed.retransmitted_frames),
                static_cast<unsigned long long>(r.feed.retransmit_failures));
  o << b;
  std::snprintf(b, sizeof(b), "books: published %llu, conflated %llu, errors %llu, crossed %llu, trade mismatches %llu\n",
                static_cast<unsigned long long>(r.books.published), static_cast<unsigned long long>(r.books.conflated),
                static_cast<unsigned long long>(r.books.error_total()), static_cast<unsigned long long>(r.books.crossed),
                static_cast<unsigned long long>(r.books.trade_mismatch));
  o << b;

  o << "\n-- trading --\n";
  std::snprintf(b, sizeof(b), "orders %llu, replaces %llu, cancels %llu, fills %llu (%llu shares), halts %llu\n",
                static_cast<unsigned long long>(r.core.orders), static_cast<unsigned long long>(r.core.replaces),
                static_cast<unsigned long long>(r.core.cancels), static_cast<unsigned long long>(r.core.fills),
                static_cast<unsigned long long>(r.core.filled_qty), static_cast<unsigned long long>(r.core.halts));
  o << b;
  std::snprintf(b, sizeof(b),
                "venue: accepted %llu, rejected %llu, cancel-rejects %llu, dropped-while-down %llu, deferred %llu\n",
                static_cast<unsigned long long>(r.exchange.accepted), static_cast<unsigned long long>(r.exchange.rejected),
                static_cast<unsigned long long>(r.exchange.cancel_rejects),
                static_cast<unsigned long long>(r.exchange.dropped_while_down),
                static_cast<unsigned long long>(r.exchange.reports_deferred));
  o << b;
  std::snprintf(b, sizeof(b),
                "oms: fill-before-ack %llu, late fills %llu, duplicates %llu, unknown %llu, expired %llu, overfills %llu\n",
                static_cast<unsigned long long>(r.oms.fill_before_ack), static_cast<unsigned long long>(r.oms.late_fills),
                static_cast<unsigned long long>(r.oms.duplicate_reports),
                static_cast<unsigned long long>(r.oms.unknown_order_reports),
                static_cast<unsigned long long>(r.oms.expired), static_cast<unsigned long long>(r.oms.overfills));
  o << b;
  o << "risk:";
  bool any = false;
  for (std::size_t i = 1; i < r.risk.size(); ++i) {
    if (r.risk[i] == 0) continue;
    o << " " << risk::to_string(static_cast<risk::RiskCheck>(i)) << "=" << r.risk[i];
    any = true;
  }
  o << (any ? "" : " no rejects") << "  (passed " << r.risk[0] << ")\n";
  if (r.open_orders_at_end) o << "WARNING: " << r.open_orders_at_end << " orders still working at end of session\n";
  o << "kill switch: " << (r.killed ? "TRIPPED (" + r.kill_reason + ")" : std::string("armed")) << "\n";
  o << "P&L: realized " << money(r.realized) << ", unrealized " << money(r.unrealized) << ", positions [";
  for (std::size_t i = 0; i < r.positions.size(); ++i) {
    if (i) o << ", ";
    o << uni[static_cast<InstrumentId>(i)].symbol_view() << " " << r.positions[i];
  }
  o << "]\n";
  if (!r.incidents.empty()) {
    o << "\n-- incidents (" << r.incidents.size() << ") --\n";
    const std::size_t show = std::min<std::size_t>(r.incidents.size(), 12);
    for (std::size_t i = 0; i < show; ++i) o << "  " << r.incidents[i] << "\n";
    if (show < r.incidents.size()) o << "  ... " << r.incidents.size() - show << " more\n";
  }
  std::snprintf(b, sizeof(b), "\ndigest          %016llx  (book %016llx, core %016llx)\n",
                static_cast<unsigned long long>(r.digest), static_cast<unsigned long long>(r.book_digest),
                static_cast<unsigned long long>(r.core_digest));
  o << b;
  return o.str();
}

std::string report_json(const RunReport& r) {
  std::ostringstream o;
  auto hist = [&](const char* name, const telemetry::LatencyHistogram& h) {
    o << "\"" << name << "\":{\"count\":" << h.count() << ",\"p50\":" << h.percentile(50)
      << ",\"p95\":" << h.percentile(95) << ",\"p99\":" << h.percentile(99) << ",\"p999\":" << h.percentile(99.9)
      << ",\"max\":" << h.max() << ",\"mean\":" << h.mean() << "}";
  };
  char dg[20];
  std::snprintf(dg, sizeof(dg), "%016llx", static_cast<unsigned long long>(r.digest));
  o << "{\"threaded\":" << (r.threaded ? "true" : "false") << ",\"queue\":\"" << to_string(r.queue)
    << "\",\"book\":\"" << to_string(r.book) << "\",\"wall_ns\":" << r.wall_ns << ",\"events\":" << r.events
    << ",\"events_per_sec\":" << r.events_per_sec() << ",\"book_updates\":" << r.book_updates
    << ",\"orders\":" << r.core.orders << ",\"fills\":" << r.core.fills << ",\"gaps\":" << r.seq.gaps
    << ",\"feed_full_spins\":" << r.feed_queue_full_spins << ",\"book_full_spins\":" << r.book_queue_full_spins
    << ",\"digest\":\"" << dg << "\",";
  hist("feed_to_book", r.feed_to_book);
  o << ",";
  hist("book_to_core", r.book_to_core);
  o << ",";
  hist("end_to_end", r.end_to_end);
  o << ",";
  hist("tick_to_order", r.tick_to_order);
  o << "}";
  return o.str();
}

}  // namespace pulse::engine
