# Ariadne Threading

[![macOS Status](https://github.com/ariadne-cps/threading/actions/workflows/macos.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/macos.yml)
[![Ubuntu Status](https://github.com/ariadne-cps/threading/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/ubuntu.yml)
[![Windows](https://github.com/ariadne-cps/threading/actions/workflows/win.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/win.yml)
[![Sanitizers](https://github.com/ariadne-cps/threading/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/sanitizers.yml)
[![Coverage](https://github.com/ariadne-cps/threading/actions/workflows/coverage.yml/badge.svg)](https://github.com/ariadne-cps/threading/actions/workflows/coverage.yml)
[![codecov](https://codecov.io/gh/ariadne-cps/threading/branch/main/graph/badge.svg)](https://codecov.io/gh/ariadne-cps/threading)

A C++20 library for thread management, concurrent workloads and CPU task scheduling, integrated with [Logging](https://github.com/ariadne-cps/logging).
Although independently maintained and usable, this repository is also part of the [Ariadne repository structure](https://github.com/ariadne-cps/ariadne/blob/main/doc/structure.md).

The three reference use cases are **BDD (Binary Decision Diagrams)** for recursive symbolic computations, **CDCL (Conflict-Driven Clause Learning)** for satisfiability search including SMT solving, and **DNN (Deep Neural Networks)** for both training and inference. Threading provides common execution mechanisms rather than application-specific algorithms.

## Features

- Managed threads, buffered workers, and synchronous or asynchronous workload processing.
- Work-stealing `ThreadPool` with worker-local queues, a global submission queue and adjustable worker count.
- `ThreadManager` for process-wide concurrency settings.
- Structured fork/join through `TaskGroup` and `TaskHandle`, including cooperative waiting and exception propagation.
- Cooperative cancellation through `StopTaskGroup` and `std::stop_token`, and first-result search coordination.
- CPU `parallel_for`, `parallel_reduce`, and fixed-block reductions with bounded work partitioning and scoped per-lane rounding modes.
- Dependency-aware CPU task graphs through `TaskGraph`, dispatching nodes as their prerequisites finish and supporting cooperative stop.
- Bounded FIFO communication through `BoundedChannel`, including closure and stop-aware waits.
- Shared nonblocking spawning limits through RAII `ConcurrencyBudget` permits.
- Thread-safe workload progress tracking and integration with Logging.
- Public headers available individually or through `ariadne-threading.hpp`.
- Standalone benchmarks use [Benchmarking](https://github.com/ariadne-cps/benchmarking) as an opt-in development dependency; it is not linked into the production Threading library.

## Build

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/ariadne-cps/threading.git
cd threading
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel --target ariadne-threading
```

A C++20 compiler, CMake and pthread support are required.
