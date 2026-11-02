# Ariadne Threading

[![macOS Status](https://github.com/ariadne-cps/threading/actions/workflows/macos.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/macos.yml)
[![Ubuntu Status](https://github.com/ariadne-cps/threading/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/ubuntu.yml)
[![Windows](https://github.com/ariadne-cps/threading/actions/workflows/win.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/win.yml)
[![Sanitizers](https://github.com/ariadne-cps/threading/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/sanitizers.yml)
[![Coverage](https://github.com/ariadne-cps/threading/actions/workflows/coverage.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/coverage.yml)
[![codecov](https://codecov.io/gh/ariadne-cps/threading/branch/main/graph/badge.svg)](https://codecov.io/gh/ariadne-cps/threading)

A small C++20 library for thread management, thread pools, buffered workers and concurrent workloads, with integration with [Logging](https://github.com/ariadne-cps/logging).

The three reference use cases are **BDD**, **CDCL**, and **DL**:
- **BDD**: structured recursive computations and fork/join.
- **CDCL**: concurrent satisfiability search, including use as the search engine in SMT solvers; theory reasoning and clause exchange belong to the solver.
- **DL**: deep learning training and inference, initially CPU data parallelism and reductions.

These are use cases for the library, not application-specific algorithms implemented by Threading.

## Features

- `Thread`: managed single-shot threads with activation and exception reporting.
- `BufferedThread`: a worker thread with a bounded task queue.
- `ThreadPool`: one work-stealing scheduler with local LIFO queues, FIFO stealing, global FIFO injection, asynchronous submission and administrative resizing.
- `ThreadManager`: process-wide concurrency management.
- `StaticWorkload` and `DynamicWorkload`: serial or concurrent workload processing.
- Thread-safe workload progress tracking.
- CPU data parallelism via `parallel_for` / `parallel_reduce` with bounded lanes and deterministic partition ordering.
- Cooperative portfolio cancellation via `StopTaskGroup` and `std::stop_token` for independent long-lived jobs.
- Structured fork/join via `TaskGroup` and `TaskHandle`, using cooperative
  waits to execute pending work from the same scheduler. Task groups join
  outstanding tasks on scope exit and propagate exceptions on explicit join.
- Work submitted from external threads enters a global FIFO, periodically serviced by workers to reduce starvation of independent callers. See [the concurrency design](doc/designs/threading_design.md).
- Coverage support on Ubuntu/GCC and macOS/AppleClang.

## Public headers

A single umbrella header includes the complete Threading public API:

```cpp
#include "ariadne-threading.hpp"
```

Individual headers under `threading/` remain available for minimal
dependencies. The umbrella is installed alongside the `threading/`
directory under `include/ariadne-threading/`.

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

The CTest executables use [Ariadne Testing](https://github.com/ariadne-cps/testing),
pinned in `ss/tst` and linked only to test targets. The suite preserves the existing test cases and adds a public-header
smoke test, retaining the original test order,
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
option selects the number of workers in the benchmark pool (including zero).
The global ThreadManager is reset to zero before timing, to avoid competing
background worker threads. Without the option, up to four workers are used.
`--mode flat|recursive|micro|breakdown|handle-compare|ownership|both` chooses the scenarios; `both` covers flat and recursive. `--help` lists the options.

The scheduler runs identical work and verify their results against a
deterministic reference checksum. Each measured iteration constructs and warms a fresh pool, measures it,
then destroys it. The benchmark reports median, minimum, maximum and median
absolute deviation (MAD) in milliseconds.
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
For BDD-like short recursive work, micro mode measures one forked branch
and one independent locally computed branch per iteration, compared with a
sequential evaluation of the identical checksum. Each paired sample uses one
freshly warmed pool, alternates measurement order, and reports the median
time difference in ns/fork (TaskGroup minus sequential). This is an end-to-end
cost difference, **not** a measurement of the cost of one isolated scheduler
primitive. A negative difference is possible when useful parallel work
outweighs scheduling overhead.

```bash
./build-release/benchmarks/benchmark_thread_pool --mode micro --concurrency 0 --tasks 16384 --work 1 --samples 9
./build-release/benchmarks/benchmark_thread_pool --mode micro --concurrency 1 --tasks 16384 --work 1 --samples 9
./build-release/benchmarks/benchmark_thread_pool --mode micro --concurrency 4 --tasks 16384 --work 1 --samples 9
```

Full performance experiments are manual. Ubuntu Release CI builds the
benchmark and runs small smoke cases at zero and two workers for functional
verification only; it makes no performance assertions.
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

### Isolating the zero-worker fork/join costs

The `breakdown` benchmark compares five variants of the **same**
checksum at `--concurrency 0`: direct sequential evaluation, a packaged
task and shared future executed inline (`Future only`), a targeted queue
ticket without a future (`Ticket only`), a full `TaskHandle` without a
`TaskGroup`, and `TaskGroup`. Results show per-stage medians and paired
differences relative to sequential execution. Stages overlap and are
**not** independent additive allocations or exact costs of individual
library primitives.

```bash
./build-release/benchmarks/benchmark_thread_pool --mode breakdown --concurrency 0 --tasks 16384 --work 1 --samples 9
```

Each sample warms and verifies all stages, runs them in a rotating order
against one zero-worker pool, and checks for leftover tasks. Do not
interpret these results as evidence of multithreaded speedup.

### Comparing the direct ticket with the legacy callback path

To compare the two acquisition representations in the **same current
binary**, including with running workers:

```bash
./build-release/benchmarks/benchmark_thread_pool --mode handle-compare --concurrency 4 --tasks 16384 --work 1 --samples 9
```

For 0, 1 and 4 workers, change `--concurrency` and rerun. Each
paired sample creates one warmed pool, runs the legacy callback
constructor, the direct ticket constructor, and the complete
`TaskGroup` path in rotating order, and checks all checksums.
A negative paired `direct - callback` number favors the direct
ticket. The comparison isolates acquisition *representation* under
the present `TaskHandle` implementation; it does **not** compare
the entire old and new repository binaries. The `TaskGroup` result
contains additional group bookkeeping and is not a third
interchangeable handle variant. After the ownership diagnostic
identified a repeated one-worker regression, `TaskGroup::fork()`
uses the callback acquisition path by default; the direct-ticket
`TaskHandle` constructor is retained for explicit use and for
the A/B diagnostic. The experiments should not be interpreted as
a universal ranking of acquisition mechanisms.

### Diagnosing task ownership

When a one-worker handle comparison behaves differently for work=128
or work=1024, run this **untimed** diagnostic to count which thread
actually executes the forked branch:

```bash
./build-release/benchmarks/benchmark_thread_pool --mode ownership --concurrency 1 --tasks 16384 --work 128 --samples 9
```

For every sample, it reports the caller/worker task counts separately
for the callback and direct-ticket representations, and the paired
change in the caller-executed percentage. Warm-up runs are excluded
from the counts; all paths verify the checksum and empty queue.
Because thread-ID checks and atomic counting **change scheduling
timing**, these counts cannot be assumed to describe the uninstrumented
timed benchmark. Use them to test the scheduling-race hypothesis,
not to claim speedups.

## Parallel CPU loops and cooperative portfolio jobs

Include `ariadne-threading.hpp`, `threading/parallel.hpp`, or
`threading/stop_task_group.hpp`.

```cpp
Ariadne::ThreadPool pool(4);
std::vector<double> values(10000,1.0);
Ariadne::parallel_for(pool,0u,values.size(),
    [&](std::size_t i) { values[i]*=2.0; },
    Ariadne::ParallelOptions{256u,5u});
double total=Ariadne::parallel_reduce(pool,0u,values.size(),0.0,
    [&](std::size_t i) { return values[i]; },
    [](double a,double b) { return a+b; });
```

`ParallelOptions` controls the minimum elements per lane and the maximum
number of execution lanes **including the caller**; `max_tasks=0`
selects at most `pool.num_threads()+1` lanes. The implementation
uses contiguous static partitions, does not spawn one task per
element, and waits for all lanes even if an exception occurs.
Input callbacks must be safe to invoke concurrently. The reducer
must be associative and have the given two-sided identity for a
well-defined parallel reduction. Floating-point rounding may change
with lane count.

For CDCL portfolio jobs:

```cpp
Ariadne::StopTaskGroup group(pool);
auto first=group.fork([](std::stop_token stop) {
    while(!stop.stop_requested()) {
        // Run a bounded portion of solver work and check for a result.
    }
});
group.request_stop();
group.join();
```

Jobs must **cooperate** with cancellation; there is no forced
interruption. `StopTaskGroup` is scoped and non-reusable after a stop
request; the owning thread submits and joins jobs, while other
threads may call `request_stop()`. The destructor requests stop
before joining. This does not implement a satisfiability or SMT solver, clause
exchange, work placement, GPU execution, DAG dependencies,
numeric tensor kernels or dynamic CPU quotas; they remain
application-specific or later design work.
