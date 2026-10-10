#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pulse/core/instrument.hpp"
#include "pulse/market_data/event.hpp"
#include "pulse/orderbook/array_order_book.hpp"

namespace pulse::md {

enum class WorkloadProfile : std::uint8_t {
  Balanced,     // adds ~ cancels, some modifies and trades
  AddHeavy,     // book builds up: deep levels, long FIFO queues
  CancelHeavy,  // HFT-style: most orders cancelled shortly after placement
  TradeHeavy,   // aggressive flow: many executions, levels deplete often
  Burst,        // balanced mix with bursty (regime-switching) arrivals
};

const char* to_string(WorkloadProfile p) noexcept;
bool parse_profile(const std::string& s, WorkloadProfile& out) noexcept;

struct GeneratorConfig {
  std::uint64_t events = 1'000'000;
  std::size_t instruments = 8;
  std::uint64_t seed = 42;
  WorkloadProfile profile = WorkloadProfile::Balanced;
  std::uint64_t start_ts = 34'200'000'000'000ull;  // 09:30:00 in ns since midnight
  double mean_gap_ns = 1'000.0;                     // aggregate ~1M msgs/s of venue time
  std::uint32_t target_orders_per_side = 150;
};

// Synthetic L3 venue. Produces a *valid* order-by-order stream: every cancel,
// modify and trade refers to a live order, resting books never cross, and
// marketable orders are turned into the Trade events a matching engine would
// print. It maintains its own reference book to guarantee that.
class FeedGenerator {
 public:
  FeedGenerator(const GeneratorConfig& cfg, const InstrumentUniverse& universe);

  // Produces the next event (seq numbers 1, 2, 3, ...). Returns false once
  // cfg.events events have been produced. The final event is a heartbeat.
  bool next(MarketEvent& out);

  const InstrumentUniverse& universe() const noexcept { return universe_; }
  std::uint64_t produced() const noexcept { return produced_; }

 private:
  struct Live {
    OrderId id;
    Side side;
  };
  struct InstrumentState {
    Price fair = 0;
    std::vector<Live> live;  // random access for cancel/modify picks
    FlatIdMap where{512};    // order id -> index in `live` (swap-remove)
    std::unique_ptr<book::FastOrderBook> book;
  };

  void emit(MarketEvent ev);
  void step();
  void do_add(InstrumentId inst);
  void do_cancel(InstrumentId inst);
  void do_modify(InstrumentId inst);
  void do_aggressive(InstrumentId inst, Side aggressor, Qty qty, Price limit);
  void add_live(InstrumentState& s, OrderId id, Side side);
  void remove_live(InstrumentState& s, OrderId id);
  Qty draw_qty();
  std::uint64_t draw_gap();

  GeneratorConfig cfg_;
  InstrumentUniverse universe_;
  std::mt19937_64 rng_;
  std::vector<InstrumentState> inst_;
  std::deque<MarketEvent> pending_;
  std::uint64_t produced_ = 0;
  SeqNum next_seq_ = 1;
  OrderId next_order_id_ = 1;
  std::uint64_t now_ = 0;
  bool burst_ = false;
  std::uint64_t regime_left_ = 0;
};

}  // namespace pulse::md
