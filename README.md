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
- `ThreadPool`: dynamically resizable pool with asynchronous task submission.
- `ThreadManager`: process-wide concurrency management.
- `StaticWorkload` and `DynamicWorkload`: serial or concurrent workload processing.
- Thread-safe workload progress tracking.
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

Build and run the C++ tests separately in Debug mode:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug --parallel --target tests
ctest --test-dir build-debug --output-on-failure
```

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
