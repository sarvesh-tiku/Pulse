// pulse: run / replay / inspect market-data sessions through the engine.

#include <cstdio>
#include <exception>
#include <string>

#include "cli.hpp"

namespace {

void usage() {
  std::puts(
      "pulse - low-latency market data, execution & risk engine\n"
      "\n"
      "usage:\n"
      "  pulse run     <session> [options]   process a session (auto-uses <session>.rewind for gap recovery)\n"
      "  pulse replay  <session> [options]   deterministic replay of a recording (no retransmission source)\n"
      "  pulse listen  --udp HOST:PORT [--rewind-tcp HOST:PORT] [options]   consume a live UDP feed\n"
      "  pulse inspect <session> [--dump N]  validate and summarise a session file\n"
      "\n"
      "run `pulse run --help` for options");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  try {
    pulse::Args a(argc, argv, 2);
    if (cmd == "run") return pulse::cli::cmd_run(a, false);
    if (cmd == "replay") return pulse::cli::cmd_run(a, true);
    if (cmd == "inspect") return pulse::cli::cmd_inspect(a);
    if (cmd == "listen") return pulse::cli::cmd_listen(a);
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
      usage();
      return 0;
    }
    std::fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pulse: %s\n", e.what());
    return 1;
  }
}
