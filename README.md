# Ariadne Threading

[![macOS Status](https://github.com/ariadne-cps/threading/actions/workflows/macos.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/macos.yml)
[![Ubuntu Status](https://github.com/ariadne-cps/threading/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/ubuntu.yml)
[![Windows](https://github.com/ariadne-cps/threading/actions/workflows/win.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/win.yml)
[![Sanitizers](https://github.com/ariadne-cps/threading/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/sanitizers.yml)
[![Coverage](https://github.com/ariadne-cps/threading/actions/workflows/coverage.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/coverage.yml)
[![codecov](https://codecov.io/gh/ariadne-cps/threading/branch/main/graph/badge.svg)](https://codecov.io/gh/ariadne-cps/threading)

A small C++20 library for thread management, thread pools, buffered workers and concurrent workloads, with integration with [Logging](https://github.com/ariadne-cps/logging).

## Features

- `Thread`: managed single-shot threads with activation and exception reporting.
- `BufferedThread`: a worker thread with a bounded task queue.
- `ThreadPool`: one scheduler with FIFO and work-stealing policies, asynchronous submission and administrative resizing.
- `ThreadManager`: process-wide concurrency management.
- `StaticWorkload` and `DynamicWorkload`: serial or concurrent workload processing.
- Thread-safe workload progress tracking.
- Structured fork/join via `TaskGroup` and `TaskHandle`, using cooperative
  waits to execute pending work from the same scheduler. Task groups join
  outstanding tasks on scope exit and propagate exceptions on explicit join.
- `SchedulingPolicy::WorkStealing`: worker-local LIFO queues and FIFO
  victim stealing, selected with `ThreadPool(workers,SchedulingPolicy::WorkStealing)`.
  See [the concurrency design](doc/designs/threading_design.md).
- Coverage support on Ubuntu/GCC and macOS/AppleClang.

## Command-line interface

`ThreadingCommandLineInterface` derives from `LoggingCommandLineInterface`,
providing logging scheduler, theme, verbosity and threading concurrency options.

## Build

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/ariadne-cps/threading.git
cd threading
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel --target ariadne-threading
```

A C++20 compiler, CMake and pthread support are required.

## Tests

All ten CTest executables use [Ariadne Testing](https://github.com/ariadne-cps/testing),
pinned in `ss/tst` and linked only to test targets. The suite contains
100 independently reported cases, preserving the original test order,
expected exceptions, smoke operations and concurrency scenarios.
The CTest command uses `--concise`, while direct execution defaults to full diagnostics.

Build and run the C++ tests separately in Debug mode:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel --target tests
ctest --test-dir build-debug --output-on-failure
```

## Benchmark

Build the standalone benchmark (outside CTest) in Release mode:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel --target benchmark_thread_pool
./build-release/benchmarks/benchmark_thread_pool --concurrency 4 --tasks 4096 --work 128 --depth 9 --samples 9
```

The command line extends `ThreadingCommandLineInterface`, including logging
scheduler, theme, verbosity and `-c`/`--concurrency` options. The concurrency
option selects the number of workers in **each** compared pool (including zero).
The global ThreadManager is reset to zero before timing, to avoid competing
background worker threads. Without the option, up to four workers are used.
`--mode flat|recursive|both` chooses the scenarios; `--help` lists the options.

Both scheduling policies run identical work and verify their results against a
deterministic reference checksum. Each measured iteration constructs and
warms one pool, measures it alone, and then destroys it before creating
the other pool. The benchmark alternates their order across paired samples
and reports median, minimum, maximum and median absolute deviation (MAD)
in milliseconds, plus the median of per-pair FIFO/stealing ratios.
Pool construction, warm-up and destruction are excluded from timing.
`--sweep` measures 1, 2, 4 and 8 workers at work=16, 128 and 1024;
it cannot be combined with `--concurrency` or `--work`.
Use `--mode recursive` or `--mode flat` to limit the sweep.
For the recursive workload, `--cutoff-depth N` stops spawning tasks when
the remaining tree depth is at most `N` (default 0 preserves the original
fully forked workload). The same leaves and combination operations run
regardless of the cutoff, so the checksum is unchanged. At depth `D`, the
number of forked tasks is `2^(D-N)-1`; cutoff `D` is purely sequential.
`--cutoff-sweep` compares even cutoffs from 0 through `D`, including the
endpoint when `D` is odd, at fixed workers and work. It reports only the
recursive workload and cannot be combined with `--sweep` or a nonzero
`--cutoff-depth`. Use, for example:
```bash
./build-release/benchmarks/benchmark_thread_pool --concurrency 4 --work 128 --depth 14 --samples 9 --mode recursive --cutoff-sweep
```
The benchmark is built and run manually; it is not a CI or CTest gate.
See [threading_design.md](doc/designs/threading_design.md) for the methodology.
## Coverage

Configure a separate Debug build with coverage enabled:

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DCOVERAGE=ON
cmake --build build-coverage --parallel --target coverage
```

On Ubuntu the report is generated with GCC/lcov, and on macOS with AppleClang/LLVM coverage tools. CI uploads the macOS coverage report to Codecov.

## Sanitizers

The Sanitizers workflow runs two independent Debug jobs on Ubuntu with Clang:
ThreadSanitizer (TSan) for data races, and AddressSanitizer with
UndefinedBehaviorSanitizer (ASan + UBSan) for memory errors and undefined behavior.

To run TSan locally, use a separate build:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DTHREAD_SANITIZER=ON
cmake --build build-tsan --parallel 2 --target tests
ctest --test-dir build-tsan --output-on-failure
```

Coverage and sanitizer builds are intentionally separate.

## License

Threading is released under the GNU General Public License v3.0.
