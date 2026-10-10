// pulse-gen: writes a synthetic L3 market-data session.
//
//   pulse-gen --out session.pulse --events 10M --instruments 8 --seed 42
//             [--profile balanced|add-heavy|cancel-heavy|trade-heavy|burst]
//             [--faults drop=1e-4,dup=1e-4,reorder=1e-4,corrupt=1e-5,burst=1e-6]
//
// With --faults, two files are written: <out> (what the receiver saw) and
// <out>.rewind (the clean stream the venue's retransmission server holds).

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>

#include "pulse/core/args.hpp"
#include "pulse/market_data/fault_injector.hpp"
#include "pulse/market_data/generator.hpp"
#include "pulse/market_data/session_file.hpp"
#include "pulse/protocol/codec.hpp"

using namespace pulse;

int main(int argc, char** argv) {
  try {
    Args a(argc, argv);
    if (a.has("help") || !a.has("out")) {
      std::puts(
          "usage: pulse-gen --out FILE [--events N] [--instruments N] [--seed N]\n"
          "                 [--profile balanced|add-heavy|cancel-heavy|trade-heavy|burst]\n"
          "                 [--mean-gap-ns N] [--faults drop=P,dup=P,reorder=P,corrupt=P,truncate=P,burst=P,"
          "burst_len=N,seed=N]\n"
          "                 [--no-checksum]");
      return a.has("help") ? 0 : 2;
    }
    md::GeneratorConfig gc;
    gc.events = static_cast<std::uint64_t>(a.num("events", 1e6));
    gc.instruments = static_cast<std::size_t>(a.num("instruments", 8));
    gc.seed = static_cast<std::uint64_t>(a.num("seed", 42));
    gc.mean_gap_ns = a.num("mean-gap-ns", 1000);
    if (a.has("profile") && !md::parse_profile(a.str("profile"), gc.profile)) {
      std::fprintf(stderr, "unknown profile '%s'\n", a.str("profile").c_str());
      return 2;
    }
    md::FaultConfig fc;
    if (a.has("faults")) {
      std::string err;
      if (!md::parse_faults(a.str("faults"), fc, err)) {
        std::fprintf(stderr, "--faults: %s\n", err.c_str());
        return 2;
      }
    }
    const bool checksum = !a.has("no-checksum");
    const auto uni = InstrumentUniverse::synthetic(gc.instruments);
    const std::string out = a.str("out");
    md::SessionInfo info{uni, gc.seed, 0, gc.start_ts};

    const auto t0 = std::chrono::steady_clock::now();
    md::FeedGenerator gen(gc, uni);
    md::SessionWriter wire(out, info);
    std::unique_ptr<md::SessionWriter> rewind;
    if (fc.any()) rewind = std::make_unique<md::SessionWriter>(out + ".rewind", info);
    md::FaultInjector inj(fc);
    auto sink = [&](std::span<const std::byte> f) { wire.write(f); };

    MarketEvent ev;
    std::array<std::byte, protocol::kMaxFrameSize> buf{};
    std::array<std::uint64_t, 5> counts{};
    while (gen.next(ev)) {
      ++counts[static_cast<std::size_t>(ev.type)];
      const auto n = protocol::encode(ev, buf, checksum);
      if (rewind) {
        rewind->write({buf.data(), n});
        inj.process({buf.data(), n}, sink);
      } else {
        wire.write({buf.data(), n});
      }
    }
    inj.flush(sink);
    wire.close();
    if (rewind) rewind->close();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("wrote %s: %llu events, %zu instruments, profile %s, seed %llu (%.2fs)\n", out.c_str(),
                static_cast<unsigned long long>(gen.produced()), gc.instruments, md::to_string(gc.profile),
                static_cast<unsigned long long>(gc.seed), secs);
    std::printf("  adds %llu  cancels %llu  modifies %llu  trades %llu  heartbeats %llu\n",
                static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(counts[2]),
                static_cast<unsigned long long>(counts[3]), static_cast<unsigned long long>(counts[4]),
                static_cast<unsigned long long>(counts[0]));
    if (rewind) {
      const auto& s = inj.stats();
      std::printf("faults (%s):\n  dropped %llu  duplicated %llu  reordered %llu  corrupted %llu  truncated %llu\n",
                  md::describe(fc).c_str(), static_cast<unsigned long long>(s.dropped),
                  static_cast<unsigned long long>(s.duplicated), static_cast<unsigned long long>(s.reordered),
                  static_cast<unsigned long long>(s.corrupted), static_cast<unsigned long long>(s.truncated));
      std::printf("  clean stream for retransmission: %s.rewind\n", out.c_str());
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pulse-gen: %s\n", e.what());
    return 1;
  }
}
