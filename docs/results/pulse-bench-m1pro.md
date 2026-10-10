## Environment

- CPU: Apple M1 Pro (8 hw threads)
- OS: Darwin 25.6.0 arm64
- Compiler: clang 17.0.0, optimized (-O3 -DNDEBUG)
- Workload: 5000000 events, 8 instruments, profile `balanced`, strategy `market-maker`, median of 5 runs

## Book implementations (inline pipeline, max speed)

| Book | Throughput | ns/event | Heap allocs/event | Digest |
|---|---:|---:|---:|---|
| `map` | 6.63M/s | 150.8 | 0.904 | `c92ed408a31c91d6` |
| `array-stdindex` | 9.03M/s | 110.8 | 0.416 | `c92ed408a31c91d6` |
| `array` | 11.01M/s | 90.8 | 0.008 | `c92ed408a31c91d6` |

## Queue implementations (threaded pipeline, array book, max speed)

| Pipeline | Batch | Throughput | Feed-queue full spins | Digest |
|---|---:|---:|---:|---|
| inline (1 thread) | 1 | 11.04M/s | 0 | `c92ed408a31c91d6` |
| mutex+condvar | 1 | 2.57M/s | 0 | `c92ed408a31c91d6` |
| mutex+condvar | 64 | 4.17M/s | 0 | `c92ed408a31c91d6` |
| spsc, no index cache | 1 | 4.77M/s | 781459141 | `c92ed408a31c91d6` |
| spsc, unpadded | 1 | 3.14M/s | 750652479 | `c92ed408a31c91d6` |
| spsc | 1 | 4.30M/s | 684773811 | `c92ed408a31c91d6` |
| spsc | 64 | 11.29M/s | 78647674 | `c92ed408a31c91d6` |

## Latency at a fixed offered load of 1.00M msgs/s (below saturation)

Wall-clock, measured from frame receipt (after pacing) to the core finishing the resulting update.

| Pipeline | feed→book p50 | book→core p50 | end-to-end p50 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| inline (1 thread) (batch 1) | 83ns | - | 125ns | 251ns | 2.58us | 145.92us |
| mutex+condvar (batch 1) | 27.78us | 4.05us | 33.54us | 142.34us | 602.11us | 1.66ms |
| spsc, no index cache (batch 1) | 374ns | 418ns | 876ns | 14.40us | 129.54us | 2.65ms |
| spsc (batch 1) | 458ns | 374ns | 916ns | 1.58us | 31.62us | 2.06ms |
| spsc (batch 64) | 540ns | 374ns | 916ns | 3.49ms | 9.24ms | 11.12ms |

