#pragma once

#include <functional>
#include <queue>
#include <random>
#include <vector>

#include "pulse/core/instrument.hpp"
#include "pulse/execution/order.hpp"
#include "pulse/orderbook/book_update.hpp"

namespace pulse::exec {

struct DisconnectWindow {
  TimestampNs start = 0;
  TimestampNs end = 0;  // exclusive
};

struct ExchangeConfig {
  TimestampNs latency_in_ns = 25'000;   // client -> venue (one way)
  TimestampNs latency_out_ns = 25'000;  // venue -> client
  TimestampNs jitter_ns = 10'000;       // uniform [0, jitter] added to each leg
  // false: one ordered session (TCP-like): reports arrive in the order sent.
  // true:  independent paths (multiple gateways): reports can overtake each
  //        other, e.g. a fill can arrive before the acknowledgement.
  bool reorder_reports = false;
  double reject_probability = 0.0;      // "venue busy" rejects on new orders
  double fill_ratio = 1.0;              // share of displayed liquidity we can take
  bool cancel_on_disconnect = true;
  std::vector<DisconnectWindow> disconnects;
  std::uint64_t seed = 99;
};

struct ExchangeStats {
  std::uint64_t requests = 0;
  std::uint64_t dropped_while_down = 0;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t fills = 0;
  std::uint64_t filled_qty = 0;
  std::uint64_t cancelled = 0;
  std::uint64_t replaced = 0;
  std::uint64_t cancel_rejects = 0;
  std::uint64_t reports_deferred = 0;  // delivery held across a disconnect
};

// Deterministic venue simulator running on market-data time.
//
// Fill model (documented simplifications — this is a *simulator*, not alpha):
//  * No market impact: our fills do not remove liquidity from the public book
//    the feed reconstructs. Within one market update we cannot take the same
//    displayed quantity twice.
//  * Marketable orders fill against the displayed depth (kBookDepth levels) at
//    each level's price.
//  * Resting orders join the back of the queue: queue_ahead starts at the
//    displayed quantity at our price. Prints at our price consume
//    queue_ahead first; prints through our price, or the opposite side
//    crossing our price, fill us. queue_ahead is clamped to the displayed
//    level size (cancels ahead of us shrink it).
class ExchangeSimulator {
 public:
  using ReportSink = std::function<void(const ExecReport&)>;

  ExchangeSimulator(const InstrumentUniverse& universe, const ExchangeConfig& cfg);

  // Client sends a request at simulated time `now`.
  void submit(const OrderRequest& req, TimestampNs now);

  // Processes every venue-side event with time <= t in time order, delivering
  // reports whose delivery time <= t to `sink`. The sink may call submit().
  void advance_to(TimestampNs t, const ReportSink& sink);

  // Market data reaches the venue's matching logic (call after advance_to).
  void on_market(const BookUpdate& u);

  // Runs everything still in flight (end of session).
  void drain(const ReportSink& sink);

  bool connected_at(TimestampNs t) const noexcept;
  std::size_t resting_orders() const noexcept;
  const ExchangeStats& stats() const noexcept { return stats_; }

 private:
  struct Resting {
    ClOrdId cl_id;
    ExchOrdId exch_id;
    Side side;
    TimeInForce tif;
    Price price;
    Qty leaves;
    Qty cum;
    std::uint64_t queue_ahead;
  };
  struct Arrival {
    TimestampNs at;
    std::uint64_t seq;
    OrderRequest req;
    bool operator>(const Arrival& o) const noexcept { return at != o.at ? at > o.at : seq > o.seq; }
  };
  struct Outbound {
    ExecReport r;
    bool operator>(const Outbound& o) const noexcept {
      return r.deliver_ts != o.r.deliver_ts ? r.deliver_ts > o.r.deliver_ts : r.exec_id > o.r.exec_id;
    }
  };
  struct Consumed {
    Side side;
    Price price;
    std::uint64_t qty;
  };
  struct InstrumentView {
    BookUpdate book{};
    bool have_book = false;
    std::vector<Consumed> consumed;  // liquidity we took since the last update
    std::vector<Resting> resting;
  };

  TimestampNs jitter();
  void process_arrival(const OrderRequest& req, TimestampNs now);
  void report(ExecReport r, TimestampNs now);
  // Fills `o` against displayed opposite liquidity at prices <= / >= its
  // limit. Aggressive orders trade at each level's price; a resting order
  // being crossed by the market trades at its own (passive) price.
  void match(InstrumentView& v, Resting& o, bool at_own_price, TimestampNs now);
  void on_resting_market(InstrumentView& v, Resting& o, const BookUpdate& u);
  void fill(InstrumentView& v, Resting& o, Price px, Qty qty, TimestampNs now);
  void on_disconnect(TimestampNs at);
  void on_reconnect(TimestampNs at);
  Resting* find(InstrumentView& v, ClOrdId cl_id);

  const InstrumentUniverse& universe_;
  ExchangeConfig cfg_;
  std::mt19937_64 rng_;
  std::vector<InstrumentView> views_;
  std::priority_queue<Arrival, std::vector<Arrival>, std::greater<>> inbound_;
  std::priority_queue<Outbound, std::vector<Outbound>, std::greater<>> outbound_;
  std::uint64_t arrival_seq_ = 0;
  std::uint64_t next_exec_id_ = 1;
  ExchOrdId next_exch_id_ = 1;
  TimestampNs last_arrival_ = 0;
  TimestampNs last_delivery_ = 0;
  std::size_t next_window_ = 0;
  bool down_ = false;
  std::vector<ExecReport> cancelled_while_down_;
  std::vector<ExecReport> deferred_;  // reports that were in flight during a disconnect
  ExchangeStats stats_;
};

}  // namespace pulse::exec
