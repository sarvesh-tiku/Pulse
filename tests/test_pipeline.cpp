#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>

#include "pulse/engine/pipeline.hpp"
#include "pulse/market_data/fault_injector.hpp"
#include "pulse/market_data/generator.hpp"
#include "pulse/market_data/rewind.hpp"
#include "pulse/protocol/codec.hpp"

using namespace pulse;
using namespace pulse::engine;

namespace {

struct Session {
  InstrumentUniverse uni;
  std::vector<std::byte> wire;
  std::vector<std::byte> clean;
};

Session make(std::uint64_t events, const md::FaultConfig& fc = {}, std::uint64_t seed = 21) {
  Session s{InstrumentUniverse::synthetic(6), {}, {}};
  md::GeneratorConfig gc;
  gc.events = events;
  gc.instruments = 6;
  gc.seed = seed;
  md::FeedGenerator gen(gc, s.uni);
  md::SessionInfo info{s.uni, seed, 0, gc.start_ts};
  md::MemorySessionBuilder wire(info), clean(info);
  md::FaultInjector inj(fc);
  std::array<std::byte, protocol::kMaxFrameSize> buf{};
  MarketEvent ev;
  auto sink = [&](std::span<const std::byte> f) { wire.write(f); };
  while (gen.next(ev)) {
    const auto n = protocol::encode(ev, buf);
    clean.write({buf.data(), n});
    inj.process({buf.data(), n}, sink);
  }
  inj.flush(sink);
  s.wire = std::move(wire).finish();
  s.clean = std::move(clean).finish();
  return s;
}

RunReport run(const Session& s, PipelineConfig cfg, bool use_rewind = true) {
  auto reader = md::SessionReader::from_memory(s.wire);
  md::SessionRewindStore store(md::SessionReader::from_memory(s.clean));
  if (use_rewind) cfg.rewind = &store;
  SessionSource src(reader);
  return run_pipeline(s.uni, src, cfg);
}

PipelineConfig base() {
  PipelineConfig c;
  c.strategies = {"market-maker", "imbalance"};
  c.measure_latency = false;
  c.queue_capacity = 256;  // small: exercise back-pressure
  c.core.exchange.jitter_ns = 20'000;
  c.core.exchange.reorder_reports = true;  // out-of-order reports in play
  return c;
}

}  // namespace

// The central correctness property: every execution mode produces exactly the
// same observable behaviour (digest over book states, risk verdicts, execution
// reports, fills and final positions) for the same input.
TEST(Pipeline, AllModesProduceIdenticalDigest) {
  md::FaultConfig fc;
  fc.drop = 5e-4;
  fc.duplicate = 5e-4;
  fc.reorder = 5e-4;
  fc.corrupt = 2e-4;
  const auto s = make(150'000, fc);

  PipelineConfig c = base();
  c.threaded = false;
  c.book = BookKind::Map;
  const auto ref = run(s, c);
  EXPECT_GT(ref.core.fills, 0u);
  EXPECT_GT(ref.seq.gaps, 0u);
  EXPECT_EQ(ref.seq.abandoned, 0u);

  for (auto book : {BookKind::Map, BookKind::Array, BookKind::ArrayStdIndex}) {
    c.threaded = false;
    c.book = book;
    EXPECT_EQ(run(s, c).digest, ref.digest) << "inline " << to_string(book);
  }
  for (auto q : {QueueKind::Spsc, QueueKind::SpscNoCache, QueueKind::SpscUnpadded, QueueKind::Mutex}) {
    c.threaded = true;
    c.queue = q;
    c.book = BookKind::Array;
    c.batch = q == QueueKind::Mutex ? 1 : 32;
    const auto r = run(s, c);
    EXPECT_EQ(r.digest, ref.digest) << "threaded " << to_string(q);
    EXPECT_EQ(r.events, ref.events);
  }
}

TEST(Pipeline, RecoveredFaultyFeedMatchesCleanFeed) {
  md::FaultConfig fc;
  fc.drop = 1e-3;
  fc.burst_drop = 1e-4;
  fc.corrupt = 1e-3;
  const auto faulty = make(80'000, fc);
  Session clean = faulty;
  clean.wire = clean.clean;
  auto c = base();
  c.core.cancel_on_feed_gap = false;  // gaps would otherwise (correctly) change trading
  const auto a = run(faulty, c);
  const auto b = run(clean, c);
  EXPECT_GT(a.seq.gaps, 0u);
  EXPECT_EQ(a.book_digest, b.book_digest);  // identical book states after recovery
}

TEST(Pipeline, RecordingReplaysToSameDigest) {
  md::FaultConfig fc;
  fc.drop = 1e-3;
  fc.reorder = 1e-3;
  const auto s = make(60'000, fc);
  const auto path = (std::filesystem::temp_directory_path() / "pulse_test_record.pulse").string();
  auto c = base();
  c.record_path = path;
  const auto live = run(s, c);

  // Replay the recording with *no* retransmission source: the retransmitted
  // frames are in the recording, so the result must be identical.
  auto reader = md::SessionReader::open(path);
  SessionSource src(reader);
  auto rc = base();
  const auto replay = run_pipeline(s.uni, src, rc);
  EXPECT_EQ(replay.digest, live.digest);
  EXPECT_EQ(replay.feed.retransmit_requests, 0u);
  std::filesystem::remove(path);
}

TEST(Pipeline, WithoutRecoveryLossHaltsTrading) {
  md::FaultConfig fc;
  fc.burst_drop = 2e-4;
  fc.burst_len = 50;
  const auto s = make(60'000, fc);
  const auto r = run(s, base(), /*use_rewind=*/false);
  EXPECT_GT(r.seq.abandoned, 0u);
  EXPECT_GT(r.risk[static_cast<std::size_t>(risk::RiskCheck::MarketDataStale)] + r.core.halts, 0u);
  bool stale_incident = false;
  for (const auto& i : r.incidents) stale_incident |= i.find("STALE") != std::string::npos;
  EXPECT_TRUE(stale_incident);
}

TEST(Pipeline, RunawayStrategyIsContainedByKillSwitch) {
  const auto s = make(60'000);
  auto c = base();
  c.strategies = {"runaway"};
  const auto r = run(s, c);
  EXPECT_TRUE(r.killed);
  EXPECT_NE(r.kill_reason.find("throttle"), std::string::npos);
  EXPECT_GT(r.risk[static_cast<std::size_t>(risk::RiskCheck::KillSwitch)], 0u);
  // Worst-case exposure never breached the position limit.
  for (auto p : r.positions) EXPECT_LE(std::llabs(p), c.core.risk.max_position);
}

TEST(Pipeline, CrashingStrategyIsIsolated) {
  const auto s = make(60'000);
  auto c = base();
  c.strategies = {"crash", "imbalance"};
  c.params.crash_after_updates = 5'000;
  const auto r = run(s, c);
  EXPECT_EQ(r.core.strategy_exceptions, 1u);
  EXPECT_FALSE(r.killed);
  bool crash_incident = false;
  for (const auto& i : r.incidents) crash_incident |= i.find("crashed") != std::string::npos;
  EXPECT_TRUE(crash_incident);
  EXPECT_GT(r.core.orders, 0u);  // the other strategy kept trading
}

TEST(Pipeline, VenueDisconnectIsReconciled) {
  const auto s = make(120'000);
  auto c = base();
  c.core.exchange.reorder_reports = false;
  const TimestampNs start = md::GeneratorConfig{}.start_ts;
  c.core.exchange.disconnects = {{start + 30'000'000, start + 60'000'000}};
  const auto r = run(s, c);
  EXPECT_GT(r.exchange.accepted, 0u);
  bool down = false, up = false;
  for (const auto& i : r.incidents) {
    down |= i.find("DOWN") != std::string::npos;
    up |= i.find("UP") != std::string::npos;
  }
  EXPECT_TRUE(down);
  EXPECT_TRUE(up);
  EXPECT_GT(r.risk[static_cast<std::size_t>(risk::RiskCheck::VenueDown)] + r.core.halts, 0u);
  EXPECT_EQ(r.oms.overfills, 0u);
  // Nothing is left working: acknowledged orders are resolved by the venue's
  // cancel-on-disconnect, unacknowledged ones by expiry.
  EXPECT_EQ(r.open_orders_at_end, 0u);
}

// Regression: replay pacing read the venue timestamp before validating the
// frame, so a corrupted timestamp stalled a paced replay indefinitely.
TEST(Pipeline, PacedReplayIgnoresCorruptTimestamps) {
  auto s = make(30'000);  // ~30 ms of venue time
  // Corrupt the timestamp of one frame (without resealing: CRC now fails).
  auto reader = md::SessionReader::from_memory(s.wire);
  md::SessionInfo info = reader.info();
  md::MemorySessionBuilder b(info);
  md::Record r;
  int i = 0;
  while (reader.next(r)) {
    std::vector<std::byte> f(r.frame.begin(), r.frame.end());
    if (++i == 1000) f[protocol::off::timestamp + 7] = std::byte{0x7F};  // ~290 years ahead
    b.write(f);
  }
  s.wire = std::move(b).finish();
  auto c = base();
  c.speed = 1.0;
  const auto rep = run(s, c);
  EXPECT_LT(rep.wall_ns, 5'000'000'000ull);
  EXPECT_EQ(rep.feed.decode_errors[static_cast<std::size_t>(protocol::DecodeStatus::BadChecksum)], 1u);
  EXPECT_EQ(rep.seq.abandoned, 0u);  // recovered from the rewind store
}
