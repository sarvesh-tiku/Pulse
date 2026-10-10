#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "pulse/engine/pipeline.hpp"
#include "pulse/market_data/rewind.hpp"
#include "pulse/net/socket.hpp"

// Pulse Rewind Protocol (TCP), modelled on venue retransmission services:
//
//   request  (16 bytes): first_seq u64 | last_seq u64
//   response (8 + N)   : status u32 (0 ok, 1 unavailable) | length u32 | N bytes of frames
//
// One request in flight per connection; the connection is reused.

namespace pulse::net {

class RetransmitServer {
 public:
  RetransmitServer(md::SessionRewindStore& store, const Endpoint& ep);
  // Serves until `stop` becomes true. One client at a time (sufficient for a
  // single feed handler; a production service would multiplex).
  void serve(const std::atomic<bool>& stop);
  std::uint16_t port() const { return listener_.port(); }
  std::uint64_t requests() const noexcept { return requests_.load(std::memory_order_relaxed); }

 private:
  md::SessionRewindStore& store_;
  TcpListener listener_;
  std::atomic<std::uint64_t> requests_{0};
};

class TcpRewindClient final : public md::RetransmitSource {
 public:
  explicit TcpRewindClient(const Endpoint& ep) : ep_(ep) {}
  bool fetch(SeqNum first, SeqNum last, std::vector<std::byte>& out) override;
  const char* name() const noexcept override { return "tcp-rewind"; }

 private:
  Endpoint ep_;
  TcpStream conn_;
};

// Receives datagrams from a bound UDP socket. Ends the stream after
// `idle_timeout` without traffic (once traffic has started).
class UdpSource final : public engine::DatagramSource {
 public:
  UdpSource(const Endpoint& ep, std::chrono::milliseconds idle_timeout,
            std::chrono::milliseconds first_timeout = std::chrono::milliseconds(30'000));
  bool next(engine::Datagram& out) override;
  bool live() const noexcept override { return true; }
  int rcvbuf() const { return sock_.actual_rcvbuf(); }

 private:
  UdpSocket sock_;
  std::chrono::milliseconds idle_, first_;
  bool started_ = false;
  std::vector<std::byte> buf_;
};

}  // namespace pulse::net
