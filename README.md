# Threading

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Unix](https://github.com/ariadne-cps/threading/actions/workflows/unix.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/unix.yml)
[![Windows](https://github.com/ariadne-cps/threading/actions/workflows/win.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/win.yml)
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

## Build

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/ariadne-cps/threading.git
cd threading
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
ctest --output-on-failure
```

A C++20 compiler, CMake and pthread support are required.

## Coverage

Configure a Debug build with coverage enabled:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCOVERAGE=ON
cmake --build . --parallel --target coverage
```

On Ubuntu the report is generated with GCC/lcov. On macOS it is generated with AppleClang/LLVM coverage tools. CI uploads the Ubuntu coverage report to Codecov.

## ThreadSanitizer

For race detection, use a separate build configured with:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DTHREAD_SANITIZER=ON
cmake --build . --parallel
ctest --output-on-failure
```

Coverage and ThreadSanitizer builds are intentionally separate.

## License

Threading is released under the GNU General Public License v3.0.
