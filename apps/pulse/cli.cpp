#include "cli.hpp"

#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <sstream>

#include "pulse/market_data/rewind.hpp"
#include "pulse/net/retransmit.hpp"
#include "pulse/protocol/codec.hpp"

namespace pulse::cli {
namespace {

bool file_exists(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0;
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, sep))
    if (!item.empty()) out.push_back(item);
  return out;
}

}  // namespace

void print_run_help() {
  std::puts(
      "pipeline:\n"
      "  --inline                 single-threaded reference pipeline (default: threaded)\n"
      "  --queue Q                spsc | spsc-nocache | spsc-unpadded | mutex      (default spsc)\n"
      "  --book B                 array | array-stdindex | map                     (default array)\n"
      "  --capacity N             queue capacity (default 16384)\n"
      "  --batch N                max items per consumer visit (default 64)\n"
      "  --speed Nx               replay at N x venue-time speed (default: max speed)\n"
      "  --rate N                 fixed offered load, datagrams/s (latency measurement)\n"
      "  --pin F,B,C              pin feed/book/core threads to CPUs (Linux)\n"
      "  --no-latency             disable wall-clock latency stamping\n"
      "  --dashboard              live terminal dashboard\n"
      "  --record FILE            record every datagram received (incl. retransmissions)\n"
      "  --json                   print the report as JSON\n"
      "feed:\n"
      "  --rewind FILE            retransmission source (default <session>.rewind if present)\n"
      "  --no-rewind              no retransmission source (gaps heal passively or go stale)\n"
      "  --no-checksum-verify     skip CRC32C verification\n"
      "strategy & risk:\n"
      "  --strategy a,b           none | market-maker | imbalance | runaway | crash  (default market-maker)\n"
      "  --instruments i,j        restrict strategies to these instrument ids\n"
      "  --quote-size N  --max-inventory N  --crash-after N\n"
      "  --max-order-qty N  --max-position N  --max-open-orders N  --collar F\n"
      "  --max-msgs-per-sec N  --throttle-trip N  --max-loss DOLLARS\n"
      "  --keep-quotes-on-gap     do not cancel working orders when a feed gap opens\n"
      "venue simulator:\n"
      "  --venue-latency-us N     one-way latency each direction (default 25)\n"
      "  --venue-jitter-us N      uniform jitter per leg (default 10)\n"
      "  --reorder-reports        reports may overtake each other (multi-path)\n"
      "  --reject-prob P          random 'venue busy' rejects\n"
      "  --disconnect A:B[,C:D]   venue session down from A ms to B ms of venue time\n"
      "  --venue-seed N");
}

engine::PipelineConfig make_config(const Args& a, TimestampNs session_start) {
  engine::PipelineConfig c;
  c.threaded = !a.has("inline");
  if (a.has("queue") && !engine::parse_queue(a.str("queue"), c.queue))
    throw std::invalid_argument("unknown --queue " + a.str("queue"));
  if (a.has("book") && !engine::parse_book(a.str("book"), c.book))
    throw std::invalid_argument("unknown --book " + a.str("book"));
  c.queue_capacity = static_cast<std::size_t>(a.num("capacity", 16384));
  c.batch = static_cast<std::size_t>(a.num("batch", 64));
  c.speed = a.has("max-speed") ? 0.0 : a.num("speed", 0.0);
  c.rate = a.num("rate", 0.0);
  if (a.has("pin")) {
    const auto p = split(a.str("pin"), ',');
    if (p.size() != 3) throw std::invalid_argument("--pin expects F,B,C");
    c.cpu_feed = std::stoi(p[0]);
    c.cpu_book = std::stoi(p[1]);
    c.cpu_core = std::stoi(p[2]);
  }
  c.measure_latency = !a.has("no-latency");
  c.dashboard = a.has("dashboard");
  c.record_path = a.str("record");
  c.feed.verify_checksum = !a.has("no-checksum-verify");

  if (a.has("strategy")) c.strategies = split(a.str("strategy"), ',');
  auto& p = c.params;
  if (a.has("instruments"))
    for (const auto& s : split(a.str("instruments"), ',')) p.instruments.push_back(static_cast<InstrumentId>(std::stoul(s)));
  p.quote_size = static_cast<Qty>(a.num("quote-size", p.quote_size));
  p.max_inventory = static_cast<std::int64_t>(a.num("max-inventory", static_cast<double>(p.max_inventory)));
  p.crash_after_updates = static_cast<std::uint64_t>(a.num("crash-after", static_cast<double>(p.crash_after_updates)));

  auto& r = c.core.risk;
  r.max_order_qty = static_cast<Qty>(a.num("max-order-qty", r.max_order_qty));
  r.max_position = static_cast<std::int64_t>(a.num("max-position", static_cast<double>(r.max_position)));
  r.max_open_orders = static_cast<std::size_t>(a.num("max-open-orders", static_cast<double>(r.max_open_orders)));
  r.price_collar = a.num("collar", r.price_collar);
  r.max_msgs_per_sec = a.num("max-msgs-per-sec", r.max_msgs_per_sec);
  r.throttle_trip = static_cast<std::uint32_t>(a.num("throttle-trip", r.throttle_trip));
  if (a.has("max-loss")) r.max_loss = static_cast<portfolio::Money>(a.num("max-loss", 0) * kPriceScale);
  c.core.cancel_on_feed_gap = !a.has("keep-quotes-on-gap");

  auto& x = c.core.exchange;
  x.latency_in_ns = x.latency_out_ns = static_cast<TimestampNs>(a.num("venue-latency-us", 25) * 1000);
  x.jitter_ns = static_cast<TimestampNs>(a.num("venue-jitter-us", 10) * 1000);
  x.reorder_reports = a.has("reorder-reports");
  x.reject_probability = a.num("reject-prob", 0.0);
  x.seed = static_cast<std::uint64_t>(a.num("venue-seed", 99));
  if (a.has("disconnect")) {
    for (const auto& w : split(a.str("disconnect"), ',')) {
      const auto ab = split(w, ':');
      if (ab.size() != 2) throw std::invalid_argument("--disconnect expects START_MS:END_MS");
      x.disconnects.push_back({session_start + static_cast<TimestampNs>(std::stod(ab[0]) * 1e6),
                               session_start + static_cast<TimestampNs>(std::stod(ab[1]) * 1e6)});
    }
  }
  return c;
}

int cmd_run(const Args& a, bool replay) {
  if (a.has("help") || a.positional().empty()) {
    std::printf("usage: pulse %s <session> [options]\n\n", replay ? "replay" : "run");
    print_run_help();
    return a.has("help") ? 0 : 2;
  }
  const std::string path = a.positional()[0];
  auto reader = md::SessionReader::open(path);
  const auto& uni = reader.info().universe;
  auto cfg = make_config(a, reader.info().start_ts);

  std::unique_ptr<md::SessionRewindStore> rewind;
  if (!replay && !a.has("no-rewind")) {
    const std::string rp = a.has("rewind") ? a.str("rewind") : path + ".rewind";
    if (a.has("rewind") || file_exists(rp)) {
      rewind = md::SessionRewindStore::open(rp);
      cfg.rewind = rewind.get();
    }
  }
  if (!a.has("json") && !cfg.dashboard) {
    std::printf("%s %s: %zu instruments, %llu records, strategies:", replay ? "replaying" : "running",
                path.c_str(), uni.size(), static_cast<unsigned long long>(reader.info().record_count));
    for (const auto& s : cfg.strategies) std::printf(" %s", s.c_str());
    std::printf(" | rewind: %s\n", rewind ? "session-rewind" : "none");
  }
  engine::SessionSource src(reader);
  const auto rep = engine::run_pipeline(uni, src, cfg);
  if (a.has("json")) {
    std::puts(engine::report_json(rep).c_str());
  } else {
    std::fputs(engine::format_report(rep, uni).c_str(), stdout);
  }
  if (!cfg.record_path.empty() && !a.has("json")) std::printf("recorded to %s\n", cfg.record_path.c_str());
  return 0;
}

int cmd_listen(const Args& a) {
  if (a.has("help") || !a.has("udp")) {
    std::puts("usage: pulse listen --udp HOST:PORT --universe N [--rewind-tcp HOST:PORT] [--idle-ms N] [options]\n");
    print_run_help();
    return a.has("help") ? 0 : 2;
  }
  const auto uni = InstrumentUniverse::synthetic(static_cast<std::size_t>(a.num("universe", 8)));
  auto cfg = make_config(a, 0);
  std::unique_ptr<net::TcpRewindClient> rewind;
  if (a.has("rewind-tcp")) {
    rewind = std::make_unique<net::TcpRewindClient>(net::parse_endpoint(a.str("rewind-tcp")));
    cfg.rewind = rewind.get();
  }
  net::UdpSource src(net::parse_endpoint(a.str("udp")),
                     std::chrono::milliseconds(static_cast<long long>(a.num("idle-ms", 1000))));
  if (!cfg.dashboard && !a.has("json")) {
    std::printf("listening on udp %s (SO_RCVBUF %d bytes), rewind: %s\n", a.str("udp").c_str(), src.rcvbuf(),
                rewind ? a.str("rewind-tcp").c_str() : "none");
  }
  const auto rep = engine::run_pipeline(uni, src, cfg);
  if (a.has("json")) {
    std::puts(engine::report_json(rep).c_str());
  } else {
    std::fputs(engine::format_report(rep, uni).c_str(), stdout);
  }
  return 0;
}

int cmd_inspect(const Args& a) {
  if (a.positional().empty()) {
    std::puts("usage: pulse inspect <session> [--dump N]");
    return 2;
  }
  auto reader = md::SessionReader::open(a.positional()[0]);
  const auto& info = reader.info();
  std::printf("session %s\n  size %.1f MB, %llu records, seed %llu\n", a.positional()[0].c_str(),
              static_cast<double>(reader.bytes()) / 1e6, static_cast<unsigned long long>(info.record_count),
              static_cast<unsigned long long>(info.seed));
  std::printf("  instruments:");
  for (const auto& s : info.universe.specs())
    std::printf(" %.*s[%s..%s]", static_cast<int>(s.symbol_view().size()), s.symbol_view().data(),
                format_price(s.min_price).c_str(), format_price(s.max_price).c_str());
  std::printf("\n");

  const auto dump = static_cast<std::uint64_t>(a.num("dump", 0));
  std::array<std::uint64_t, 8> errors{};
  std::array<std::uint64_t, 5> types{};
  std::uint64_t frames = 0, retrans = 0, out_of_order = 0, dups = 0;
  SeqNum max_seq = 0;
  TimestampNs first_ts = 0, last_ts = 0;
  md::Record rec;
  MarketEvent ev;
  while (reader.next(rec)) {
    std::size_t pos = 0;
    while (pos < rec.frame.size()) {
      const auto r = protocol::decode(rec.frame.subspan(pos), ev);
      if (r.status != protocol::DecodeStatus::Ok) {
        ++errors[static_cast<std::size_t>(r.status)];
        if (r.consumed == 0) break;
        pos += r.consumed;
        continue;
      }
      pos += r.consumed;
      ++frames;
      ++types[static_cast<std::size_t>(ev.type)];
      if (ev.flags & protocol::kFlagRetransmit) ++retrans;
      if (!first_ts) first_ts = ev.exchange_ts;
      last_ts = std::max(last_ts, ev.exchange_ts);
      if (ev.type != EventType::Heartbeat) {
        if (ev.seq <= max_seq) {
          ++(ev.seq == max_seq ? dups : out_of_order);
        }
        max_seq = std::max(max_seq, ev.seq);
      }
      if (frames <= dump) {
        std::printf("  #%-8llu %-9s inst %u  id %llu  %s %u @ %s\n", static_cast<unsigned long long>(ev.seq),
                    to_string(ev.type), ev.instrument, static_cast<unsigned long long>(ev.order_id),
                    std::string(to_string(ev.side)).c_str(), ev.qty, format_price(ev.price).c_str());
      }
    }
  }
  std::printf("  frames %llu (retransmitted %llu): add %llu, cancel %llu, modify %llu, trade %llu, heartbeat %llu\n",
              static_cast<unsigned long long>(frames), static_cast<unsigned long long>(retrans),
              static_cast<unsigned long long>(types[1]), static_cast<unsigned long long>(types[2]),
              static_cast<unsigned long long>(types[3]), static_cast<unsigned long long>(types[4]),
              static_cast<unsigned long long>(types[0]));
  std::printf("  max seq %llu, non-increasing seqs %llu, venue time span %.3f s\n",
              static_cast<unsigned long long>(max_seq), static_cast<unsigned long long>(out_of_order + dups),
              static_cast<double>(last_ts - first_ts) * 1e-9);
  std::printf("  decode errors:");
  bool any = false;
  for (std::size_t i = 1; i < errors.size(); ++i) {
    if (!errors[i]) continue;
    any = true;
    std::printf(" %s=%llu", protocol::to_string(static_cast<protocol::DecodeStatus>(i)),
                static_cast<unsigned long long>(errors[i]));
  }
  std::printf("%s\n", any ? "" : " none");
  return 0;
}

}  // namespace pulse::cli
