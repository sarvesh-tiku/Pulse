#include "pulse/telemetry/dashboard.hpp"

#include <cstdio>
#include <string>

#include "pulse/market_data/sequencer.hpp"
#include "pulse/telemetry/histogram.hpp"

namespace pulse::telemetry {
namespace {

std::string money(std::int64_t m) {
  char buf[48];
  const double d = static_cast<double>(m) / static_cast<double>(kPriceScale);
  std::snprintf(buf, sizeof(buf), "%s$%.2f", d < 0 ? "-" : "", d < 0 ? -d : d);
  return buf;
}

std::string px(Price p) {
  if (p == kInvalidPrice || p == 0) return "-";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", to_double(p));
  return buf;
}

std::string group(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
  return s;
}

}  // namespace

std::string render_dashboard(const InstrumentUniverse& uni, const engine::CoreSnapshot& c, const DashboardView& v) {
  std::string out;
  char line[256];
  out += "\x1b[H\x1b[2J";  // home + clear
  out += "PULSE LIVE\n\n";
  std::snprintf(line, sizeof(line), "Elapsed          %8.1fs\nEvents/sec   %12s\nEvents       %12s\n",
                v.elapsed_s, group(static_cast<std::uint64_t>(v.events_per_sec)).c_str(), group(v.events).c_str());
  out += line;
  std::snprintf(line, sizeof(line), "p50 latency  %12s\np99 latency  %12s\n\n",
                format_ns(static_cast<double>(v.p50_ns)).c_str(), format_ns(static_cast<double>(v.p99_ns)).c_str());
  out += line;

  std::snprintf(line, sizeof(line), "%-6s %10s %10s %8s %9s %12s\n", "SYM", "BID", "ASK", "SPREAD", "POS", "P&L");
  out += line;
  for (std::uint32_t i = 0; i < c.instruments; ++i) {
    const auto& spec = uni[i];
    const bool two = c.bid[i] != kInvalidPrice && c.ask[i] != kInvalidPrice;
    std::snprintf(line, sizeof(line), "%-6.*s %10s %10s %8s %+9lld %12s\n",
                  static_cast<int>(spec.symbol_view().size()), spec.symbol_view().data(), px(c.bid[i]).c_str(),
                  px(c.ask[i]).c_str(), two ? px(c.ask[i] - c.bid[i]).c_str() : "-",
                  static_cast<long long>(c.position[i]), money(c.inst_pnl[i]).c_str());
    out += line;
  }
  std::snprintf(line, sizeof(line), "\nRealized P&L   %14s\nUnrealized P&L %14s\n", money(c.realized).c_str(),
                money(c.unrealized).c_str());
  out += line;
  std::snprintf(line, sizeof(line), "Orders %s  Fills %s  Risk rejects %s  Working %u\n\n",
                group(c.orders).c_str(), group(c.fills).c_str(), group(c.risk_rejects).c_str(), c.live_orders);
  out += line;

  const char* feed = md::to_string(static_cast<md::FeedState>(v.feed_state));
  const char* health = c.md_stale ? "STALE" : (v.feed_state == 0 ? "HEALTHY" : feed);
  std::snprintf(line, sizeof(line),
                "Feed status   %10s\nSequence gaps %10llu\nVenue         %10s\nKill switch   %10s\n"
                "Queue util    feed->book %3.0f%%  book->core %3.0f%%\n",
                health, static_cast<unsigned long long>(v.gaps), c.venue_up ? "UP" : "DOWN",
                c.killed ? "TRIPPED" : "armed", v.q1_util * 100.0, v.q2_util * 100.0);
  out += line;
  return out;
}

}  // namespace pulse::telemetry
