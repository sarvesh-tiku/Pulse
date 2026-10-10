#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pulse/engine/book_builder.hpp"
#include "pulse/engine/trading_core.hpp"
#include "pulse/market_data/feed_handler.hpp"
#include "pulse/market_data/session_file.hpp"
#include "pulse/telemetry/histogram.hpp"

namespace pulse::engine {

enum class QueueKind : std::uint8_t {
  Spsc,          // padded, cached indices (default)
  SpscNoCache,   // padded, always reads the peer's index
  SpscUnpadded,  // cached indices, head/tail share a cache line
  Mutex,         // std::mutex + condition_variable
};
enum class BookKind : std::uint8_t { Array, ArrayStdIndex, Map };

const char* to_string(QueueKind q) noexcept;
const char* to_string(BookKind b) noexcept;
bool parse_queue(const std::string& s, QueueKind& out) noexcept;
bool parse_book(const std::string& s, BookKind& out) noexcept;

// Where datagrams come from: a session file / memory, or a UDP socket.
struct Datagram {
  std::span<const std::byte> bytes;
  std::uint64_t capture_ns = 0;
};
class DatagramSource {
 public:
  virtual ~DatagramSource() = default;
  // Blocks until a datagram is available; false at end of stream.
  virtual bool next(Datagram& out) = 0;
  // Live sources are stamped at receipt; replayed ones may be paced.
  virtual bool live() const noexcept { return false; }
};

class SessionSource final : public DatagramSource {
 public:
  explicit SessionSource(md::SessionReader& reader) : reader_(reader) {}
  bool next(Datagram& out) override {
    md::Record r;
    if (!reader_.next(r)) return false;
    out = {r.frame, r.capture_ns};
    return true;
  }

 private:
  md::SessionReader& reader_;
};

struct PipelineConfig {
  bool threaded = true;
  QueueKind queue = QueueKind::Spsc;
  BookKind book = BookKind::Array;
  std::size_t queue_capacity = 1 << 14;
  std::size_t batch = 64;     // max items a consumer takes per queue visit
  double speed = 0.0;         // replay at venue-time speed x N; 0 = as fast as possible
  double rate = 0.0;          // fixed offered load in datagrams/s (latency tests); 0 = off
  int cpu_feed = -1, cpu_book = -1, cpu_core = -1;
  bool measure_latency = true;
  bool dashboard = false;     // live terminal dashboard
  std::string record_path;    // record every datagram (incl. retransmits) here
  md::FeedHandlerConfig feed;
  md::RetransmitSource* rewind = nullptr;
  CoreConfig core;
  std::vector<std::string> strategies{"market-maker"};
  strategy::StrategyParams params;
};

struct RunReport {
  // configuration echo
  bool threaded = false;
  QueueKind queue = QueueKind::Spsc;
  BookKind book = BookKind::Array;
  // volume / time
  std::uint64_t wall_ns = 0;
  std::uint64_t datagrams = 0;
  std::uint64_t events = 0;       // sequenced events delivered to the book stage
  std::uint64_t book_updates = 0; // published to the core
  // stage stats
  md::FeedStats feed;
  md::SequencerStats seq;
  BookStats books;
  CoreStats core;
  exec::OmsStats oms;
  exec::ExchangeStats exchange;
  std::array<std::uint64_t, static_cast<std::size_t>(risk::RiskCheck::kCount)> risk{};
  bool killed = false;
  std::string kill_reason;
  std::vector<std::string> incidents;
  std::vector<std::int64_t> positions;
  std::size_t open_orders_at_end = 0;  // should be 0: finish() cancels and drains
  portfolio::Money realized = 0;
  portfolio::Money unrealized = 0;
  // back-pressure
  std::uint64_t feed_queue_full_spins = 0;
  std::uint64_t book_queue_full_spins = 0;
  // latency (wall clock)
  telemetry::LatencyHistogram feed_to_book;   // ingress -> book stage done with the event
  telemetry::LatencyHistogram book_to_core;   // book publish -> core dequeue
  telemetry::LatencyHistogram end_to_end;     // ingress -> core done with the update
  telemetry::LatencyHistogram tick_to_order;  // ingress -> order handed to the gateway
  // determinism
  std::uint64_t book_digest = 0;
  std::uint64_t core_digest = 0;
  std::uint64_t digest = 0;  // combined

  double events_per_sec() const noexcept {
    return wall_ns ? static_cast<double>(events) * 1e9 / static_cast<double>(wall_ns) : 0.0;
  }
};

RunReport run_pipeline(const InstrumentUniverse& uni, DatagramSource& src, const PipelineConfig& cfg);

std::string format_report(const RunReport& r, const InstrumentUniverse& uni);
std::string report_json(const RunReport& r);

}  // namespace pulse::engine
