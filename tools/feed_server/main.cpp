// pulse-feed: publishes a session over UDP and serves retransmissions over TCP.
//
//   pulse-feed session.pulse --udp 127.0.0.1:9000 --rewind-port 9001 --rate 200k
//              [--batch-bytes 1400] [--faults drop=1e-4,...] [--linger-ms 3000]
//
// Frames are packed into datagrams of up to --batch-bytes (MoldUDP-style).
// The rewind server serves the clean stream (<session>.rewind if it exists,
// otherwise the session itself).

#include <sys/stat.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

#include "pulse/concurrency/thread.hpp"
#include "pulse/core/args.hpp"
#include "pulse/core/clock.hpp"
#include "pulse/market_data/fault_injector.hpp"
#include "pulse/market_data/rewind.hpp"
#include "pulse/net/retransmit.hpp"

using namespace pulse;

int main(int argc, char** argv) {
  try {
    Args a(argc, argv);
    if (a.has("help") || a.positional().empty()) {
      std::puts(
          "usage: pulse-feed <session> [--udp HOST:PORT] [--rewind-port N] [--rate MSGS_PER_SEC]\n"
          "                  [--batch-bytes N] [--faults SPEC] [--linger-ms N]");
      return a.has("help") ? 0 : 2;
    }
    const std::string path = a.positional()[0];
    const auto udp_ep = net::parse_endpoint(a.str("udp", "127.0.0.1:9000"));
    const double rate = a.num("rate", 200'000);
    const auto batch_bytes = static_cast<std::size_t>(a.num("batch-bytes", 1400));

    md::FaultConfig fc;
    if (a.has("faults")) {
      std::string err;
      if (!md::parse_faults(a.str("faults"), fc, err)) {
        std::fprintf(stderr, "--faults: %s\n", err.c_str());
        return 2;
      }
    }

    struct stat st {};
    const std::string rewind_path = ::stat((path + ".rewind").c_str(), &st) == 0 ? path + ".rewind" : path;
    auto store = md::SessionRewindStore::open(rewind_path);
    net::RetransmitServer server(*store, {"127.0.0.1", static_cast<std::uint16_t>(a.num("rewind-port", 9001))});
    conc::Worker rewind_thread("pulse-rewind", -1, [&](const std::atomic<bool>& stop) { server.serve(stop); });

    auto reader = md::SessionReader::open(path);
    auto sock = net::UdpSocket::connect(udp_ep);
    std::printf("publishing %s -> udp %s:%u at %.0f msgs/s (datagrams <= %zu B), rewind tcp :%u from %s\n",
                path.c_str(), udp_ep.host.c_str(), udp_ep.port, rate, batch_bytes, server.port(),
                rewind_path.c_str());
    if (fc.any()) std::printf("injecting faults: %s\n", md::describe(fc).c_str());

    std::vector<std::byte> dgram;
    dgram.reserve(batch_bytes + 64);
    std::uint64_t sent_frames = 0, datagrams = 0, send_drops = 0;
    auto flush = [&] {
      if (dgram.empty()) return;
      if (!sock.send(dgram)) ++send_drops;
      ++datagrams;
      dgram.clear();
    };
    md::FaultInjector inj(fc);
    auto emit = [&](std::span<const std::byte> f) {
      if (dgram.size() + f.size() > batch_bytes) flush();
      dgram.insert(dgram.end(), f.begin(), f.end());
    };

    const auto t0 = now_ns();
    md::Record rec;
    while (reader.next(rec)) {
      // Pace per frame; flush the datagram whenever we are about to wait.
      const auto due = t0 + static_cast<std::uint64_t>(static_cast<double>(sent_frames) * 1e9 / rate);
      if (now_ns() < due) {
        flush();
        while (now_ns() < due) cpu_relax();
      }
      inj.process(rec.frame, emit);
      ++sent_frames;
    }
    inj.flush(emit);
    flush();
    const double secs = static_cast<double>(now_ns() - t0) * 1e-9;
    std::printf("sent %llu frames in %llu datagrams (%.2fs, %.0f frames/s); local send drops %llu\n",
                static_cast<unsigned long long>(sent_frames), static_cast<unsigned long long>(datagrams), secs,
                static_cast<double>(sent_frames) / secs, static_cast<unsigned long long>(send_drops));
    if (fc.any()) {
      const auto& s = inj.stats();
      std::printf("faults: dropped %llu duplicated %llu reordered %llu corrupted %llu truncated %llu\n",
                  static_cast<unsigned long long>(s.dropped), static_cast<unsigned long long>(s.duplicated),
                  static_cast<unsigned long long>(s.reordered), static_cast<unsigned long long>(s.corrupted),
                  static_cast<unsigned long long>(s.truncated));
    }
    // Keep the rewind service up while the receiver finishes recovering.
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(a.num("linger-ms", 3000))));
    std::printf("rewind requests served: %llu\n", static_cast<unsigned long long>(server.requests()));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pulse-feed: %s\n", e.what());
    return 1;
  }
}
