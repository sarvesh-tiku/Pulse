#include <gtest/gtest.h>

#include <vector>

#include "pulse/market_data/sequencer.hpp"

using namespace pulse;
using namespace pulse::md;

namespace {

MarketEvent msg(SeqNum s) {
  MarketEvent e{};
  e.type = EventType::AddOrder;
  e.seq = s;
  e.order_id = s;
  return e;
}

MarketEvent heartbeat(SeqNum next) {
  MarketEvent e{};
  e.type = EventType::Heartbeat;
  e.seq = next;
  return e;
}

struct Collector {
  std::vector<MarketEvent> out;
  void operator()(const MarketEvent& e) { out.push_back(e); }
  std::vector<SeqNum> data_seqs() const {
    std::vector<SeqNum> v;
    for (const auto& e : out)
      if (e.type == EventType::AddOrder) v.push_back(e.seq);
    return v;
  }
  std::vector<EventType> statuses() const {
    std::vector<EventType> v;
    for (const auto& e : out)
      if (e.type >= EventType::FeedGap) v.push_back(e.type);
    return v;
  }
};

}  // namespace

TEST(Sequencer, InOrderPassesThrough) {
  Sequencer s(16);
  Collector c;
  for (SeqNum i = 1; i <= 5; ++i) s.on_message(msg(i), c);
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 2, 3, 4, 5}));
  EXPECT_TRUE(c.statuses().empty());
  EXPECT_EQ(s.state(), FeedState::Live);
  EXPECT_EQ(s.expected(), 6u);
}

TEST(Sequencer, DropsDuplicates) {
  Sequencer s(16);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(msg(2), c);
  s.on_message(msg(2), c);
  s.on_message(msg(1), c);
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 2}));
  EXPECT_EQ(s.stats().duplicates, 2u);
}

TEST(Sequencer, GapIsBufferedRequestedAndFilled) {
  Sequencer s(16);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(msg(4), c);  // 2,3 missing
  EXPECT_EQ(s.state(), FeedState::Recovering);
  GapRange g;
  ASSERT_TRUE(s.take_request(g));
  EXPECT_EQ(g.first, 2u);
  EXPECT_EQ(g.last, 3u);
  EXPECT_FALSE(s.take_request(g));
  s.on_message(msg(5), c);  // buffered too, no new request
  EXPECT_FALSE(s.take_request(g));
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1}));

  s.on_message(msg(2), c);
  EXPECT_EQ(s.state(), FeedState::Recovering);
  s.on_message(msg(3), c);
  EXPECT_EQ(s.state(), FeedState::Live);
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 2, 3, 4, 5}));
  EXPECT_EQ(c.statuses(), (std::vector<EventType>{EventType::FeedGap, EventType::FeedRecovered}));
  EXPECT_EQ(s.stats().gaps, 1u);
  EXPECT_EQ(s.stats().missing, 2u);
  EXPECT_EQ(s.stats().recovered, 2u);
}

TEST(Sequencer, SecondHoleInsideOpenGapRequestsOnlyNewNumbers) {
  Sequencer s(32);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(msg(3), c);
  GapRange g;
  ASSERT_TRUE(s.take_request(g));
  s.on_message(msg(6), c);  // 4,5 newly missing
  ASSERT_TRUE(s.take_request(g));
  EXPECT_EQ(g.first, 4u);
  EXPECT_EQ(g.last, 5u);
  for (SeqNum q : {SeqNum{2}, SeqNum{4}, SeqNum{5}}) s.on_message(msg(q), c);
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 2, 3, 4, 5, 6}));
  EXPECT_EQ(s.state(), FeedState::Live);
}

TEST(Sequencer, HeartbeatRevealsTrailingLoss) {
  Sequencer s(16);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(heartbeat(4), c);  // venue says next is 4: 2,3 lost
  EXPECT_EQ(s.state(), FeedState::Recovering);
  GapRange g;
  ASSERT_TRUE(s.take_request(g));
  EXPECT_EQ(g.first, 2u);
  EXPECT_EQ(g.last, 3u);
  s.on_message(msg(2), c);
  s.on_message(msg(3), c);
  EXPECT_EQ(s.state(), FeedState::Live);
}

TEST(Sequencer, OverflowBeyondWindowGoesStale) {
  Sequencer s(8);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(msg(100), c);
  EXPECT_EQ(s.state(), FeedState::Stale);
  EXPECT_EQ(c.statuses(), (std::vector<EventType>{EventType::FeedStale}));
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 100}));
  s.on_message(msg(101), c);
  EXPECT_EQ(s.expected(), 102u);
}

TEST(Sequencer, GiveUpSkipsToBufferedAndDelivers) {
  Sequencer s(16);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(msg(5), c);
  s.on_message(msg(6), c);
  s.give_up(c);
  EXPECT_EQ(s.state(), FeedState::Stale);
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 5, 6}));
  EXPECT_EQ(s.stats().abandoned, 3u);
  EXPECT_EQ(s.expected(), 7u);
}

TEST(Sequencer, OutOfPositionFillsAreCountedAsRecovered) {
  Sequencer s(16);
  Collector c;
  s.on_message(msg(1), c);
  s.on_message(msg(5), c);  // 2,3,4 missing
  s.on_message(msg(4), c);
  s.on_message(msg(3), c);
  s.on_message(msg(2), c);
  EXPECT_EQ(s.state(), FeedState::Live);
  EXPECT_EQ(s.stats().missing, 3u);
  EXPECT_EQ(s.stats().recovered, 3u);
  EXPECT_EQ(c.data_seqs(), (std::vector<SeqNum>{1, 2, 3, 4, 5}));
}
