#!/usr/bin/env bash
# Builds and tests Pulse on Linux inside Docker, mirroring CI:
#   gcc Release, clang Release, clang ASan, clang UBSan, clang TSan.
# Usage: scripts/linux_check.sh [config ...]   (default: all)
# Useful on macOS, where Apple clang's ASan/TSan runtimes hang at startup on
# recent macOS releases (the runtime spins in get_dyld_hdr during shadow init).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIGS=("${@:-gcc clang asan ubsan tsan}")
IMAGE=pulse-ci:ubuntu24

docker build -q -t "$IMAGE" - >/dev/null <<'EOF'
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential clang cmake git ninja-build ca-certificates >/dev/null && rm -rf /var/lib/apt/lists/*
EOF

docker run --rm -v "$ROOT":/src:ro "$IMAGE" bash -c '
set -uo pipefail
status=0
for cfg in '"${CONFIGS[*]}"'; do
  case $cfg in
    gcc)   cc=gcc   cxx=g++     san="" ;;
    clang) cc=clang cxx=clang++ san="" ;;
    asan)  cc=clang cxx=clang++ san=address ;;
    ubsan) cc=clang cxx=clang++ san=undefined ;;
    tsan)  cc=clang cxx=clang++ san=thread ;;
  esac
  b=/tmp/build-$cfg
  echo "=== $cfg ($cxx${san:+, -fsanitize=$san}) ==="
  if CC=$cc CXX=$cxx cmake -S /src -B $b -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPULSE_WERROR=ON \
        -DPULSE_SANITIZER=$san -DPULSE_BUILD_BENCHMARKS=OFF >/tmp/$cfg.log 2>&1 &&
     cmake --build $b >>/tmp/$cfg.log 2>&1; then
    if (cd $b && TSAN_OPTIONS=halt_on_error=1 ASAN_OPTIONS=detect_leaks=1 ./tests/pulse_tests --gtest_brief=1 >>/tmp/$cfg.log 2>&1); then
      echo "$cfg: PASS ($(grep -Eo "[0-9]+ tests? from [0-9]+ test suites? ran" /tmp/$cfg.log | tail -1))"
    else
      echo "$cfg: TESTS FAILED"; tail -60 /tmp/$cfg.log; status=1
    fi
  else
    echo "$cfg: BUILD FAILED"; grep -E "error|warning" /tmp/$cfg.log | head -40; status=1
  fi
done
exit $status
'
