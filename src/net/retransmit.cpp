#include "pulse/net/retransmit.hpp"

#include <cstring>

#include "pulse/core/clock.hpp"

namespace pulse::net {

RetransmitServer::RetransmitServer(md::SessionRewindStore& store, const Endpoint& ep)
    : store_(store), listener_(TcpListener::listen(ep)) {}

void RetransmitServer::serve(const std::atomic<bool>& stop) {
  while (!stop.load(std::memory_order_acquire)) {
    TcpStream c = listener_.accept(std::chrono::milliseconds(100));
    if (!c) continue;
    std::vector<std::byte> frames;
    while (!stop.load(std::memory_order_acquire)) {
      std::byte req[16];
      if (!c.read_exact(req)) break;
      std::uint64_t first, last;
      std::memcpy(&first, req, 8);
      std::memcpy(&last, req + 8, 8);
      requests_.fetch_add(1, std::memory_order_relaxed);
      frames.clear();
      const bool ok = last >= first && last - first < 1'000'000 && store_.fetch(first, last, frames);
      std::byte hdr[8];
      const std::uint32_t status = ok ? 0 : 1;
      const auto len = static_cast<std::uint32_t>(ok ? frames.size() : 0);
      std::memcpy(hdr, &status, 4);
      std::memcpy(hdr + 4, &len, 4);
      if (!c.write_all(hdr)) break;
      if (len && !c.write_all(frames)) break;
    }
  }
}

bool TcpRewindClient::fetch(SeqNum first, SeqNum last, std::vector<std::byte>& out) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      if (!conn_) conn_ = TcpStream::connect(ep_);
    } catch (const std::exception&) {
      return false;
    }
    std::byte req[16];
    std::memcpy(req, &first, 8);
    std::memcpy(req + 8, &last, 8);
    std::byte hdr[8];
    if (!conn_.write_all(req) || !conn_.read_exact(hdr)) {
      conn_ = TcpStream();  // reconnect once
      continue;
    }
    std::uint32_t status, len;
    std::memcpy(&status, hdr, 4);
    std::memcpy(&len, hdr + 4, 4);
    if (status != 0) return false;
    const auto at = out.size();
    out.resize(at + len);
    if (!conn_.read_exact({out.data() + at, len})) {
      out.resize(at);
      conn_ = TcpStream();
      continue;
    }
    return true;
  }
  return false;
}

UdpSource::UdpSource(const Endpoint& ep, std::chrono::milliseconds idle_timeout,
                     std::chrono::milliseconds first_timeout)
    : sock_(UdpSocket::bind(ep)), idle_(idle_timeout), first_(first_timeout), buf_(65536) {}

bool UdpSource::next(engine::Datagram& out) {
  const std::size_t n = sock_.recv(buf_, started_ ? idle_ : first_);
  if (n == 0) return false;  // idle: end of session
  started_ = true;
  out.bytes = {buf_.data(), n};
  out.capture_ns = now_ns();
  return true;
}

}  // namespace pulse::net
