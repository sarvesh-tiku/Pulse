#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include "pulse/net/socket.hpp"

namespace pulse::net {

Endpoint parse_endpoint(const std::string& s) {
  Endpoint ep;
  const auto colon = s.rfind(':');
  std::string port;
  if (colon == std::string::npos) {
    port = s;
  } else {
    if (colon > 0) ep.host = s.substr(0, colon);
    port = s.substr(colon + 1);
  }
  const long p = std::strtol(port.c_str(), nullptr, 10);
  if (p <= 0 || p > 65535) throw std::invalid_argument("bad endpoint: " + s);
  ep.port = static_cast<std::uint16_t>(p);
  return ep;
}

Fd& Fd::operator=(Fd&& o) noexcept {
  if (this != &o) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = o.release();
  }
  return *this;
}

Fd::~Fd() {
  if (fd_ >= 0) ::close(fd_);
}

sockaddr_in to_sockaddr(const Endpoint& ep) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(ep.port);
  if (::inet_pton(AF_INET, ep.host.c_str(), &a.sin_addr) != 1) {
    throw std::invalid_argument("not an IPv4 address: " + ep.host);
  }
  return a;
}

namespace {
[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + std::strerror(errno)); }
}  // namespace

UdpSocket UdpSocket::bind(const Endpoint& ep, int rcvbuf_bytes) {
  UdpSocket s;
  s.fd_ = Fd(::socket(AF_INET, SOCK_DGRAM, 0));
  if (!s.fd_) fail("socket");
  const int one = 1;
  ::setsockopt(s.fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // A large receive buffer absorbs bursts while the feed thread is busy; if
  // it overflows, the kernel drops datagrams and the sequencer sees gaps.
  ::setsockopt(s.fd_.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf_bytes, sizeof(rcvbuf_bytes));
  const auto a = to_sockaddr(ep);
  if (::bind(s.fd_.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) fail("bind");
  return s;
}

UdpSocket UdpSocket::connect(const Endpoint& ep, int sndbuf_bytes) {
  UdpSocket s;
  s.fd_ = Fd(::socket(AF_INET, SOCK_DGRAM, 0));
  if (!s.fd_) fail("socket");
  ::setsockopt(s.fd_.get(), SOL_SOCKET, SO_SNDBUF, &sndbuf_bytes, sizeof(sndbuf_bytes));
  const auto a = to_sockaddr(ep);
  if (::connect(s.fd_.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) fail("connect");
  return s;
}

int UdpSocket::actual_rcvbuf() const {
  int v = 0;
  socklen_t len = sizeof(v);
  ::getsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &v, &len);
  return v;
}

std::size_t UdpSocket::recv(std::span<std::byte> buf, std::chrono::milliseconds timeout) {
  while (true) {
    const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), MSG_DONTWAIT);
    if (n >= 0) return static_cast<std::size_t>(n);
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) fail("recv");
    pollfd p{fd_.get(), POLLIN, 0};
    const int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (r == 0) return 0;
    if (r < 0 && errno != EINTR) fail("poll");
  }
}

bool UdpSocket::send(std::span<const std::byte> data) {
  while (true) {
    const ssize_t n = ::send(fd_.get(), data.data(), data.size(), 0);
    if (n >= 0) return true;
    if (errno == EINTR) continue;
    if (errno == ENOBUFS || errno == EAGAIN) return false;
    // ECONNREFUSED: nobody listening yet (ICMP port unreachable). Treat as loss.
    if (errno == ECONNREFUSED) return false;
    fail("send");
  }
}

}  // namespace pulse::net
