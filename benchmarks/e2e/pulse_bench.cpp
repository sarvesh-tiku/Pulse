// pulse-bench: end-to-end pipeline benchmark matrix.
//
//   pulse-bench --events 5M --instruments 8 --repeat 3 --suite all
//               [--rate 1M] [--profile balanced] [--md results.md] [--json results.json]
//
// Suites:
//   books    inline pipeline, max speed: book implementation vs throughput
//   queues   threaded pipeline, max speed: queue implementation vs throughput
//   latency  fixed offered load (--rate) below saturation: per-stage latency
//
// Sessions are generated in memory up front (not timed). Each configuration
// runs --repeat times; the run with the median throughput is reported. Every
// run's digest must match the reference or the benchmark aborts: a faster
// but different pipeline is a bug, not a result.

#include <sys/utsname.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "../alloc_counter.hpp"
#include "pulse/core/args.hpp"
#include "pulse/engine/pipeline.hpp"
#include "pulse/market_data/generator.hpp"
#include "pulse/protocol/codec.hpp"

using namespace pulse;
using namespace pulse::engine;

namespace {

std::string cpu_model() {
#if defined(__APPLE__)
  char buf[256];
  std::size_t n = sizeof(buf);
  if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0) return buf;
#elif defined(__linux__)
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
#endif
  return "unknown";
}

std::string environment() {
  utsname u{};
  uname(&u);
  std::ostringstream o;
  o << "- CPU: " << cpu_model() << " (" << std::thread::hardware_concurrency() << " hw threads)\n";
  o << "- OS: " << u.sysname << " " << u.release << " " << u.machine << "\n";
#if defined(__clang__)
  o << "- Compiler: clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
#elif defined(__GNUC__)
  o << "- Compiler: gcc " << __GNUC__ << "." << __GNUC_MINOR__;
#endif
#if defined(NDEBUG)
  o << ", optimized (-O3 -DNDEBUG)\n";
#else
  o << ", NOT optimized\n";
#endif
  return o.str();
}

struct Row {
  std::string name;
  RunReport rep;
  std::uint64_t allocs = 0;
};

std::string fmt_rate(double eps) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.2fM/s", eps / 1e6);
  return b;
}
std::string fmt_ns(std::uint64_t ns) { return telemetry::format_ns(static_cast<double>(ns)); }

}  // namespace

int main(int argc, char** argv) {
  try {
    Args a(argc, argv);
    if (a.has("help")) {
      std::puts("usage: pulse-bench [--events N] [--instruments N] [--profile P] [--repeat N] "
                "[--suite books|queues|latency|all] [--rate N] [--strategy S] [--md FILE] [--json FILE]");
      return 0;
    }
    const auto events = static_cast<std::uint64_t>(a.num("events", 5e6));
    const auto instruments = static_cast<std::size_t>(a.num("instruments", 8));
    const int repeat = static_cast<int>(a.num("repeat", 3));
    const double rate = a.num("rate", 1e6);
    const std::string suite = a.str("suite", "all");
    md::GeneratorConfig gc;
    gc.events = events;
    gc.instruments = instruments;
    gc.seed = static_cast<std::uint64_t>(a.num("seed", 42));
    if (a.has("profile") && !md::parse_profile(a.str("profile"), gc.profile)) throw std::invalid_argument("profile");

    std::fprintf(stderr, "generating %llu events (%s, %zu instruments)...\n", static_cast<unsigned long long>(events),
                 md::to_string(gc.profile), instruments);
    const auto uni = InstrumentUniverse::synthetic(instruments);
    std::vector<std::byte> session;
    {
      md::FeedGenerator gen(gc, uni);
      md::MemorySessionBuilder b({uni, gc.seed, 0, gc.start_ts});
      std::array<std::byte, protocol::kMaxFrameSize> buf{};
      MarketEvent ev;
      while (gen.next(ev)) b.write({buf.data(), protocol::encode(ev, buf)});
      session = std::move(b).finish();
    }

    PipelineConfig base;
    base.strategies = {a.str("strategy", "market-maker")};
    base.queue_capacity = static_cast<std::size_t>(a.num("capacity", 16384));

    std::uint64_t ref_digest = 0;
    auto run_once = [&](const std::string& name, PipelineConfig cfg) {
      std::vector<Row> runs;
      for (int i = 0; i < repeat; ++i) {
        auto reader = md::SessionReader::from_memory(session);
        SessionSource src(reader);
        const auto a0 = bench::allocations();
        Row r{name, run_pipeline(uni, src, cfg), 0};
        r.allocs = bench::allocations() - a0;
        if (ref_digest == 0) ref_digest = r.rep.digest;
        if (r.rep.digest != ref_digest) {
          std::fprintf(stderr, "DIGEST MISMATCH in %s: %016llx vs %016llx\n", name.c_str(),
                       static_cast<unsigned long long>(r.rep.digest), static_cast<unsigned long long>(ref_digest));
          std::exit(3);
        }
        std::fprintf(stderr, "  %-28s run %d: %s\n", name.c_str(), i + 1, fmt_rate(r.rep.events_per_sec()).c_str());
        runs.push_back(std::move(r));
      }
      std::sort(runs.begin(), runs.end(),
                [](const Row& x, const Row& y) { return x.rep.events_per_sec() < y.rep.events_per_sec(); });
      return runs[runs.size() / 2];
    };

    std::ostringstream md;
    std::ostringstream js;
    js << "{\"events\":" << events << ",\"instruments\":" << instruments << ",\"profile\":\"" << md::to_string(gc.profile)
       << "\",\"results\":[";
    bool first_js = true;
    auto emit_json = [&](const std::string& suite_name, const Row& r) {
      js << (first_js ? "" : ",") << "{\"suite\":\"" << suite_name << "\",\"name\":\"" << r.name
         << "\",\"allocs\":" << r.allocs << ",\"report\":" << report_json(r.rep) << "}";
      first_js = false;
    };

    md << "## Environment\n\n" << environment();
    md << "- Workload: " << events << " events, " << instruments << " instruments, profile `"
       << md::to_string(gc.profile) << "`, strategy `" << base.strategies[0] << "`, median of " << repeat << " runs\n\n";

    if (suite == "all" || suite == "books") {
      md << "## Book implementations (inline pipeline, max speed)\n\n";
      md << "| Book | Throughput | ns/event | Heap allocs/event | Digest |\n|---|---:|---:|---:|---|\n";
      for (auto b : {BookKind::Map, BookKind::ArrayStdIndex, BookKind::Array}) {
        PipelineConfig c = base;
        c.threaded = false;
        c.book = b;
        c.measure_latency = false;
        const auto r = run_once(std::string("inline/") + to_string(b), c);
        char line[256];
        std::snprintf(line, sizeof(line), "| `%s` | %s | %.1f | %.3f | `%016llx` |\n", to_string(b),
                      fmt_rate(r.rep.events_per_sec()).c_str(), 1e9 / r.rep.events_per_sec(),
                      static_cast<double>(r.allocs) / static_cast<double>(r.rep.events),
                      static_cast<unsigned long long>(r.rep.digest));
        md << line;
        emit_json("books", r);
      }
      md << "\n";
    }

    if (suite == "all" || suite == "queues") {
      md << "## Queue implementations (threaded pipeline, array book, max speed)\n\n";
      md << "| Pipeline | Batch | Throughput | Feed-queue full spins | Digest |\n|---|---:|---:|---:|---|\n";
      struct Q {
        const char* label;
        bool threaded;
        QueueKind q;
        std::size_t batch;
      };
      const Q qs[] = {{"inline (1 thread)", false, QueueKind::Spsc, 1},
                      {"mutex+condvar", true, QueueKind::Mutex, 1},
                      {"mutex+condvar", true, QueueKind::Mutex, 64},
                      {"spsc, no index cache", true, QueueKind::SpscNoCache, 1},
                      {"spsc, unpadded", true, QueueKind::SpscUnpadded, 1},
                      {"spsc", true, QueueKind::Spsc, 1},
                      {"spsc", true, QueueKind::Spsc, 64}};
      for (const auto& q : qs) {
        PipelineConfig c = base;
        c.threaded = q.threaded;
        c.queue = q.q;
        c.batch = q.batch;
        c.measure_latency = false;
        const auto r = run_once(std::string(q.label) + "/b" + std::to_string(q.batch), c);
        char line[256];
        std::snprintf(line, sizeof(line), "| %s | %zu | %s | %llu | `%016llx` |\n", q.label, q.batch,
                      fmt_rate(r.rep.events_per_sec()).c_str(),
                      static_cast<unsigned long long>(r.rep.feed_queue_full_spins),
                      static_cast<unsigned long long>(r.rep.digest));
        md << line;
        emit_json("queues", r);
      }
      md << "\n";
    }

    if (suite == "all" || suite == "latency") {
      char hdr[256];
      std::snprintf(hdr, sizeof(hdr), "## Latency at a fixed offered load of %.2fM msgs/s (below saturation)\n\n",
                    rate / 1e6);
      md << hdr;
      md << "Wall-clock, measured from frame receipt (after pacing) to the core finishing the resulting update.\n\n";
      md << "| Pipeline | feed→book p50 | book→core p50 | end-to-end p50 | p99 | p99.9 | max |\n"
            "|---|---:|---:|---:|---:|---:|---:|\n";
      struct L {
        const char* label;
        bool threaded;
        QueueKind q;
        std::size_t batch;
      };
      const L ls[] = {{"inline (1 thread)", false, QueueKind::Spsc, 1},
                      {"mutex+condvar", true, QueueKind::Mutex, 1},
                      {"spsc, no index cache", true, QueueKind::SpscNoCache, 1},
                      {"spsc", true, QueueKind::Spsc, 1},
                      {"spsc", true, QueueKind::Spsc, 64}};
      for (const auto& l : ls) {
        PipelineConfig c = base;
        c.threaded = l.threaded;
        c.queue = l.q;
        c.batch = l.batch;
        c.rate = rate;
        const auto r = run_once(std::string("latency/") + l.label + "/b" + std::to_string(l.batch), c);
        const auto& e = r.rep.end_to_end;
        char line[320];
        std::snprintf(line, sizeof(line), "| %s (batch %zu) | %s | %s | %s | %s | %s | %s |\n", l.label, l.batch,
                      fmt_ns(r.rep.feed_to_book.percentile(50)).c_str(),
                      l.threaded ? fmt_ns(r.rep.book_to_core.percentile(50)).c_str() : "-",
                      fmt_ns(e.percentile(50)).c_str(), fmt_ns(e.percentile(99)).c_str(),
                      fmt_ns(e.percentile(99.9)).c_str(), fmt_ns(e.max()).c_str());
        md << line;
        emit_json("latency", r);
      }
      md << "\n";
    }
    js << "]}";

    std::fputs(md.str().c_str(), stdout);
    if (a.has("md")) std::ofstream(a.str("md")) << md.str();
    if (a.has("json")) std::ofstream(a.str("json")) << js.str() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pulse-bench: %s\n", e.what());
    return 1;
  }
}
