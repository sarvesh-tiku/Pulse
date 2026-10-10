#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "pulse/core/instrument.hpp"
#include "pulse/engine/digest.hpp"
#include "pulse/market_data/event.hpp"
#include "pulse/orderbook/book_types.hpp"
#include "pulse/orderbook/book_update.hpp"

namespace pulse::engine {

struct BookStats {
  std::uint64_t events = 0;
  std::uint64_t published = 0;
  std::uint64_t conflated = 0;       // events that did not change the top of book
  std::uint64_t status_events = 0;
  std::array<std::uint64_t, 5> book_errors{};  // indexed by book::BookResult
  std::uint64_t trade_mismatch = 0;  // trade price/side disagrees with the resting order
  std::uint64_t crossed = 0;

  std::uint64_t error_total() const noexcept {
    std::uint64_t n = 0;
    for (std::size_t i = 1; i < book_errors.size(); ++i) n += book_errors[i];
    return n;
  }
};

// Book stage: applies normalized events to one book per instrument and emits
// a BookUpdate when the visible top kBookDepth levels change, a trade prints,
// or feed health changes. Events that leave the visible book untouched are
// conflated (not published): the strategy only ever needs current state.
template <book::OrderBook Book>
class BookBuilder {
 public:
  explicit BookBuilder(const InstrumentUniverse& uni) {
    books_.reserve(uni.size());
    for (const auto& s : uni.specs()) books_.push_back(std::make_unique<Book>(s));
    last_.resize(uni.size());
  }

  // Returns true (and fills `out`) if the event produced a publishable update.
  bool apply(const MarketEvent& ev, BookUpdate& out) {
    ++stats_.events;
    if (ev.type >= EventType::FeedGap) return status(ev, out);
    if (ev.instrument >= books_.size()) {
      ++stats_.book_errors[static_cast<std::size_t>(book::BookResult::UnknownOrder)];
      return false;
    }
    Book& b = *books_[ev.instrument];
    Snapshot& last = last_[ev.instrument];
    // Can this event change the visible top-kBookDepth levels? If the price
    // it touches is strictly behind the last published 5th level, it cannot,
    // and we skip the depth snapshot entirely.
    auto visible = [&](Side side, Price px) {
      if (side == Side::Buy) return last.bid_levels < kBookDepth || px >= last.bid_px[kBookDepth - 1];
      return last.ask_levels < kBookDepth || px <= last.ask_px[kBookDepth - 1];
    };
    book::BookResult r = book::BookResult::Ok;
    bool trade = false;
    bool touches_top = true;
    switch (ev.type) {
      case EventType::AddOrder:
        touches_top = visible(ev.side, ev.price);
        r = b.add(ev.order_id, ev.side, ev.price, ev.qty);
        break;
      case EventType::CancelOrder: {
        book::OrderView o;
        if (b.find(ev.order_id, o)) touches_top = visible(o.side, o.price);
        r = b.cancel(ev.order_id);
        break;
      }
      case EventType::ModifyOrder: {
        book::OrderView o;
        if (b.find(ev.order_id, o)) touches_top = visible(o.side, o.price) || visible(o.side, ev.price);
        r = b.modify(ev.order_id, ev.price, ev.qty);
        break;
      }
      case EventType::Trade: {
        // Cross-check the print against the order it claims to hit.
        book::OrderView o;
        if (b.find(ev.order_id, o) && (o.price != ev.price || o.side != ev.side)) ++stats_.trade_mismatch;
        r = b.execute(ev.order_id, ev.qty);
        trade = r == book::BookResult::Ok;
        break;
      }
      default: return false;
    }
    if (r != book::BookResult::Ok) ++stats_.book_errors[static_cast<std::size_t>(r)];
    if (!touches_top && !trade) {
      ++stats_.conflated;
      return false;
    }

    fill_levels(b, out);
    const bool changed = out.bid_levels != last.bid_levels || out.ask_levels != last.ask_levels ||
                         out.bid_px != last.bid_px || out.ask_px != last.ask_px ||
                         out.bid_qty != last.bid_qty || out.ask_qty != last.ask_qty;
    if (!changed && !trade) {
      ++stats_.conflated;
      return false;
    }
    last = {out.bid_levels, out.ask_levels, out.bid_px, out.ask_px, out.bid_qty, out.ask_qty};

    out.seq = ev.seq;
    out.exchange_ts = ev.exchange_ts;
    out.ingress_ns = ev.ingress_ns;
    out.instrument = ev.instrument;
    out.cause = ev.type;
    out.flags = health_flags();
    out.trade_px = 0;
    out.trade_qty = 0;
    out.trade_resting_side = Side::Buy;
    if (trade) {
      out.flags |= BookUpdate::kFlagTrade;
      out.trade_px = ev.price;
      out.trade_qty = ev.qty;
      out.trade_resting_side = ev.side;
    }
    if (out.bid_levels && out.ask_levels && out.bid_px[0] >= out.ask_px[0]) {
      out.flags |= BookUpdate::kFlagCrossed;
      ++stats_.crossed;
    }
    ++stats_.published;
    digest(out);
    return true;
  }

  const BookStats& stats() const noexcept { return stats_; }
  const Book& book(InstrumentId i) const noexcept { return *books_[i]; }
  std::size_t size() const noexcept { return books_.size(); }
  const Digest& digest() const noexcept { return digest_; }
  bool stale() const noexcept { return stale_; }
  bool gap_open() const noexcept { return gap_open_; }

 private:
  struct Snapshot {
    std::uint8_t bid_levels = 0, ask_levels = 0;
    std::array<Price, kBookDepth> bid_px{}, ask_px{};
    std::array<std::uint32_t, kBookDepth> bid_qty{}, ask_qty{};
  };

  std::uint8_t health_flags() const noexcept {
    std::uint8_t f = 0;
    if (gap_open_) f |= BookUpdate::kFlagFeedGap;
    if (stale_) f |= BookUpdate::kFlagFeedStale;
    return f;
  }

  static void fill_levels(const Book& b, BookUpdate& out) {
    std::array<book::Level, kBookDepth> lv;
    const auto nb = b.depth(Side::Buy, lv);
    for (std::size_t i = 0; i < kBookDepth; ++i) {
      out.bid_px[i] = i < nb ? lv[i].price : 0;
      out.bid_qty[i] = i < nb ? clamp32(lv[i].qty) : 0;
    }
    const auto na = b.depth(Side::Sell, lv);
    for (std::size_t i = 0; i < kBookDepth; ++i) {
      out.ask_px[i] = i < na ? lv[i].price : 0;
      out.ask_qty[i] = i < na ? clamp32(lv[i].qty) : 0;
    }
    out.bid_levels = static_cast<std::uint8_t>(nb);
    out.ask_levels = static_cast<std::uint8_t>(na);
  }

  static std::uint32_t clamp32(std::uint64_t q) noexcept {
    return q > 0xFFFF'FFFFull ? 0xFFFF'FFFFu : static_cast<std::uint32_t>(q);
  }

  bool status(const MarketEvent& ev, BookUpdate& out) {
    ++stats_.status_events;
    std::uint8_t f = BookUpdate::kFlagStatusOnly;
    if (ev.type == EventType::FeedGap) {
      gap_open_ = true;
    } else if (ev.type == EventType::FeedRecovered) {
      gap_open_ = false;
      f |= BookUpdate::kFlagFeedRecovered;
    } else if (ev.type == EventType::FeedStale) {
      gap_open_ = false;
      stale_ = true;  // sticky for the session: we may have missed deletes
    }
    out = BookUpdate{};
    if (ev.type == EventType::FeedGap && ev.order_id >= ev.seq) {
      // Gap size (last - first + 1), passed in the otherwise unused trade_qty.
      const auto n = ev.order_id - ev.seq + 1;
      out.trade_qty = n > 0xFFFF'FFFFull ? 0xFFFF'FFFFu : static_cast<Qty>(n);
    }
    out.seq = ev.seq;
    out.exchange_ts = ev.exchange_ts;
    out.ingress_ns = ev.ingress_ns;
    out.cause = ev.type;
    out.flags = static_cast<std::uint8_t>(f | health_flags());
    ++stats_.published;
    // Not digested: the book digest covers book *content* only, so a faulty
    // feed that was fully recovered yields the same digest as a clean one.
    // Health transitions reach the core, which digests its reaction.
    return true;
  }

  void digest(const BookUpdate& u) noexcept {
    digest_.mix(u.seq);
    digest_.mix(u.instrument);
    digest_.mix(u.flags & BookUpdate::kFlagTrade);
    digest_.mix_signed(u.bid_levels ? u.bid_px[0] : 0);
    digest_.mix_signed(u.ask_levels ? u.ask_px[0] : 0);
    digest_.mix(u.bid_qty[0]);
    digest_.mix(u.ask_qty[0]);
  }

  std::vector<std::unique_ptr<Book>> books_;
  std::vector<Snapshot> last_;
  BookStats stats_;
  Digest digest_;
  bool gap_open_ = false;
  bool stale_ = false;
};

}  // namespace pulse::engine
