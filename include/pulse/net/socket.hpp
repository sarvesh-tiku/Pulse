#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pulse::net {

struct Endpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
};

// "host:port" or ":port" / "port".
Endpoint parse_endpoint(const std::string& s);

// RAII file descriptor.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& o) noexcept : fd_(o.release()) {}
  Fd& operator=(Fd&& o) noexcept;
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd();
  int get() const noexcept { return fd_; }
  int release() noexcept {
    const int f = fd_;
    fd_ = -1;
    return f;
  }
  explicit operator bool() const noexcept { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

// Unicast UDP. Market data in production is usually multicast; the receive
// path is identical (a bound datagram socket), so unicast keeps the demo
// runnable on any laptop.
class UdpSocket {
 public:
  static UdpSocket bind(const Endpoint& ep, int rcvbuf_bytes = 8 << 20);
  static UdpSocket connect(const Endpoint& ep, int sndbuf_bytes = 4 << 20);

  // Returns bytes received, 0 on timeout. Throws on error.
  std::size_t recv(std::span<std::byte> buf, std::chrono::milliseconds timeout);
  // Returns false if the kernel dropped it (ENOBUFS) -- itself a realistic loss.
  bool send(std::span<const std::byte> data);
  int actual_rcvbuf() const;

 private:
  Fd fd_;
};

class TcpStream {
 public:
  static TcpStream connect(const Endpoint& ep);
  explicit TcpStream(Fd fd);
  TcpStream() = default;

  bool write_all(std::span<const std::byte> data);
  bool read_exact(std::span<std::byte> out);
  explicit operator bool() const noexcept { return static_cast<bool>(fd_); }

 private:
  Fd fd_;
};

class TcpListener {
 public:
  static TcpListener listen(const Endpoint& ep);
  // Waits up to `timeout` for a connection; empty stream on timeout.
  TcpStream accept(std::chrono::milliseconds timeout);
  std::uint16_t port() const;

 private:
  Fd fd_;
};

}  // namespace pulse::net
