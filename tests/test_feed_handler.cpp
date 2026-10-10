#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "pulse/market_data/fault_injector.hpp"
#include "pulse/market_data/feed_handler.hpp"
#include "pulse/market_data/generator.hpp"
#include "pulse/market_data/session_file.hpp"

using namespace pulse;
using namespace pulse::md;

namespace {

struct Session {
  InstrumentUniverse uni;
  std::vector<std::byte> clean;   // what the venue sent
  std::vector<std::byte> faulty;  // what the receiver saw
  FaultStats faults;
};

Session make_session(std::uint64_t events, const FaultConfig& fc) {
  Session s{InstrumentUniverse::synthetic(4), {}, {}, {}};
  GeneratorConfig gc;
  gc.events = events;
  gc.instruments = 4;
  gc.seed = 11;
  FeedGenerator gen(gc, s.uni);
  SessionInfo info{s.uni, gc.seed, 0, gc.start_ts};
  MemorySessionBuilder clean(info), faulty(info);
  FaultInjector inj(fc);
  MarketEvent ev;
  std::array<std::byte, protocol::kMaxFrameSize> buf{};
  auto sink = [&](std::span<const std::byte> f) { faulty.write(f); };
  while (gen.next(ev)) {
    const auto n = protocol::encode(ev, buf);
    clean.write({buf.data(), n});
    inj.process({buf.data(), n}, sink);
  }
  inj.flush(sink);
  s.faults = inj.stats();
  s.clean = std::move(clean).finish();
  s.faulty = std::move(faulty).finish();
  return s;
}

struct Out {
  std::vector<MarketEvent> data;
  std::vector<MarketEvent> status;
  void operator()(const MarketEvent& e) {
    if (e.type >= EventType::FeedGap) {
      status.push_back(e);
    } else {
      data.push_back(e);
    }
  }
};

Out run(const std::vector<std::byte>& bytes, const InstrumentUniverse& uni, RetransmitSource* rewind,
        FeedStats* stats = nullptr, SequencerStats* seq = nullptr) {
  auto reader = SessionReader::from_memory(bytes);
  FeedHandler h(uni, {}, rewind);
  Out out;
  Record r;
  while (reader.next(r)) h.on_datagram(r.frame, 0, out);
  h.finish(out);
  if (stats) *stats = h.stats();
  if (seq) *seq = h.sequencer().stats();
  return out;
}

void expect_same_data(const std::vector<MarketEvent>& a, const std::vector<MarketEvent>& b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(a[i].seq, b[i].seq) << i;
    ASSERT_EQ(a[i].type, b[i].type) << i;
    ASSERT_EQ(a[i].order_id, b[i].order_id) << i;
    ASSERT_EQ(a[i].qty, b[i].qty) << i;
    ASSERT_EQ(a[i].price, b[i].price) << i;
  }
}

}  // namespace

TEST(FeedHandler, CleanStreamDeliversEverythingInOrder) {
  const auto s = make_session(20'000, {});
  const auto out = run(s.clean, s.uni, nullptr);
  ASSERT_EQ(out.data.size(), 20'000u - 1);  // minus trailing heartbeat
  for (std::size_t i = 0; i < out.data.size(); ++i) ASSERT_EQ(out.data[i].seq, i + 1);
  EXPECT_TRUE(out.status.empty());
}

TEST(FeedHandler, RecoversEveryFaultTypeFromRewindStore) {
  FaultConfig fc;
  fc.drop = 2e-3;
  fc.burst_drop = 2e-4;
  fc.burst_len = 40;
  fc.duplicate = 2e-3;
  fc.reorder = 2e-3;
  fc.corrupt = 2e-3;
  fc.truncate = 1e-3;
  const auto s = make_session(50'000, fc);
  ASSERT_GT(s.faults.dropped, 0u);
  ASSERT_GT(s.faults.duplicated, 0u);
  ASSERT_GT(s.faults.reordered, 0u);
  ASSERT_GT(s.faults.corrupted, 0u);
  ASSERT_GT(s.faults.truncated, 0u);

  SessionRewindStore store(SessionReader::from_memory(s.clean));
  FeedStats fs;
  SequencerStats ss;
  const auto got = run(s.faulty, s.uni, &store, &fs, &ss);
  const auto want = run(s.clean, s.uni, nullptr);

  // After recovery the downstream sees exactly the venue's stream.
  expect_same_data(got.data, want.data);
  EXPECT_GT(ss.gaps, 0u);
  EXPECT_EQ(ss.abandoned, 0u);
  EXPECT_GT(fs.retransmit_requests, 0u);
  EXPECT_GT(fs.decode_errors[static_cast<std::size_t>(protocol::DecodeStatus::BadChecksum)], 0u);
  // Every gap opened is closed, in band.
  ASSERT_FALSE(got.status.empty());
  EXPECT_EQ(got.status.size() % 2, 0u);
  for (std::size_t i = 0; i < got.status.size(); i += 2) {
    EXPECT_EQ(got.status[i].type, EventType::FeedGap);
    EXPECT_EQ(got.status[i + 1].type, EventType::FeedRecovered);
  }
}

TEST(FeedHandler, WithoutRewindReorderIsHealedPassively) {
  FaultConfig fc;
  fc.reorder = 5e-3;
  fc.duplicate = 5e-3;
  const auto s = make_session(20'000, fc);
  SequencerStats ss;
  const auto got = run(s.faulty, s.uni, nullptr, nullptr, &ss);
  const auto want = run(s.clean, s.uni, nullptr);
  expect_same_data(got.data, want.data);
  EXPECT_EQ(ss.abandoned, 0u);
}

TEST(FeedHandler, WithoutRewindLossGoesStale) {
  FaultConfig fc;
  fc.drop = 1e-3;
  const auto s = make_session(20'000, fc);
  ASSERT_GT(s.faults.dropped, 0u);
  SequencerStats ss;
  const auto got = run(s.faulty, s.uni, nullptr, nullptr, &ss);
  EXPECT_GT(ss.abandoned, 0u);
  bool saw_stale = false;
  for (const auto& e : got.status) saw_stale |= e.type == EventType::FeedStale;
  EXPECT_TRUE(saw_stale);
}

TEST(FeedHandler, RecordHookSeesRetransmissions) {
  FaultConfig fc;
  fc.drop = 1e-3;
  const auto s = make_session(10'000, fc);
  SessionRewindStore store(SessionReader::from_memory(s.clean));
  auto reader = SessionReader::from_memory(s.faulty);
  FeedHandler h(s.uni, {}, &store);
  SessionInfo info{s.uni, 0, 0, 0};
  MemorySessionBuilder rec(info);
  h.set_record_hook([&](std::span<const std::byte> b, std::uint64_t ts) { rec.write(b, ts); });
  Out live;
  Record r;
  while (reader.next(r)) h.on_datagram(r.frame, 0, live);
  h.finish(live);

  // Replaying the recording *without* a rewind source reproduces the same
  // downstream stream: the retransmissions are part of the recording.
  const auto recording = std::move(rec).finish();
  const auto replayed = run(recording, s.uni, nullptr);
  expect_same_data(live.data, replayed.data);
  ASSERT_EQ(live.status.size(), replayed.status.size());
}
