#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include "pulse/net/socket.hpp"

namespace pulse::net {

sockaddr_in to_sockaddr(const Endpoint& ep);  // udp.cpp

namespace {
[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + std::strerror(errno)); }

void no_delay(int fd) {
  // Small request/response messages: disable Nagle so they are not held back.
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}
}  // namespace

TcpStream::TcpStream(Fd fd) : fd_(std::move(fd)) {
  if (fd_) no_delay(fd_.get());
}

TcpStream TcpStream::connect(const Endpoint& ep) {
  Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd) fail("socket");
  const auto a = to_sockaddr(ep);
  if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) fail("connect");
  return TcpStream(std::move(fd));
}

bool TcpStream::write_all(std::span<const std::byte> data) {
  std::size_t off = 0;
  while (off < data.size()) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd_.get(), data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd_.get(), data.data() + off, data.size() - off, 0);
#endif
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

bool TcpStream::read_exact(std::span<std::byte> out) {
  std::size_t off = 0;
  while (off < out.size()) {
    const ssize_t n = ::recv(fd_.get(), out.data() + off, out.size() - off, 0);
    if (n == 0) return false;  // peer closed
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

TcpListener TcpListener::listen(const Endpoint& ep) {
  TcpListener l;
  l.fd_ = Fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!l.fd_) fail("socket");
  const int one = 1;
  ::setsockopt(l.fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  const auto a = to_sockaddr(ep);
  if (::bind(l.fd_.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0) fail("bind");
  if (::listen(l.fd_.get(), 16) != 0) fail("listen");
  return l;
}

TcpStream TcpListener::accept(std::chrono::milliseconds timeout) {
  pollfd p{fd_.get(), POLLIN, 0};
  const int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
  if (r <= 0) return {};
  const int c = ::accept(fd_.get(), nullptr, nullptr);
  if (c < 0) return {};
  return TcpStream(Fd(c));
}

std::uint16_t TcpListener::port() const {
  sockaddr_in a{};
  socklen_t len = sizeof(a);
  ::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&a), &len);
  return ntohs(a.sin_port);
}

}  // namespace pulse::net
