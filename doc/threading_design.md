# Threading: usage guide and architectural choices

Threading is a C++20 library for CPU task execution, structured concurrency,
coordination and observable workloads. It supplies scheduling mechanisms, not
application algorithms: BDD symbolic manipulation, CDCL/SMT solving and DNN
training/inference remain the responsibility of their own libraries.

## Usage guide

### Choose the right abstraction

| Goal | Prefer | Why / when not to use it |
| --- | --- | --- |
| Many independent CPU items | `parallel_for` | Bounded, static partitioning; group cheap items rather than submitting one task per tiny operation. |
| Parallel mapping and reduction | `parallel_reduce` | Good when partial results can be combined; arithmetic grouping may depend on worker count. |
| Repeatable reduction grouping | `parallel_reduce_fixed_blocks` | Fixed contiguous blocks and block-order combination, independent of worker count; additional partial storage. |
| Recursive BDD-like divide-and-conquer | `TaskGroup` + `TaskHandle` | Fork one sufficiently costly branch, compute other work locally, then `get()`/`join()`. |
| Several cancellable, long-running tasks | `StopTaskGroup` | Shared cooperative `std::stop_token`; callbacks must check it. |
| Find the first conclusive candidate | `parallel_first_result` | CDCL/SMT-style portfolio coordination; first published success wins, not necessarily the lowest candidate index. |
| Operations with explicit prerequisites | `TaskGraph` | Dependency-directed dispatch without barriers between entire topological levels. |
| Bounded producer/consumer communication | `BoundedChannel<T>` | Backpressure, close/drain semantics and stop-aware waits. |
| Limit extra spawning across nested algorithms | `ConcurrencyBudget` via `ParallelOptions::spawn_budget` | Shared nonblocking permits for additional lanes; not a global CPU quota. |
| Work items with progress reporting and Logging | `StaticWorkload` / `DynamicWorkload` | Higher-level legacy APIs; dynamic tasks may enqueue more work. |
| Read progress without a Logging dependency in observers | `WorkloadObservable::snapshot()` | Coherent waiting/processing/completed counts; completed does not mean successful. |
| Submit tasks directly or control dedicated workers | `ThreadPool` / `ThreadManager` | Prefer the explicit pool for isolated algorithms; the manager owns process-wide configuration. |
| One managed thread or a dedicated buffered worker | `Thread` / `BufferedThread` | Use when a single worker's lifecycle or queued execution matters more than work stealing. |

Headers can be included individually as `threading/<name>.hpp`, or via
`ariadne-threading.hpp`. Link against the compiled Threading library; some
non-template functionality lives in `.cpp` files.

### Basic CPU data parallelism

```cpp
#include "threading/parallel.hpp"
#include "threading/thread_pool.hpp"
#include <vector>

Ariadne::ThreadPool pool(4);  // Four dedicated workers, plus the caller.
std::vector<int> values(4096, 1);
Ariadne::parallel_for(pool, 0u, values.size(),
    [&](std::size_t i) { values[i] *= 2; },
    Ariadne::ParallelOptions{64u});
```

`ParallelOptions::grain_size` limits how finely the input is partitioned;
`max_tasks` limits execution lanes **including the calling thread** (zero
chooses automatically, up to `pool.num_threads()+1`). Measure a meaningful
task size: scheduling hundreds of very short callbacks can cost more than
doing their work sequentially. Callbacks used concurrently must be
thread-safe and avoid conflicting writes.

### Structured work and dependencies

Use `TaskGroup` for a recursive fork/compute/join structure. A fork returns
a result-bearing `TaskHandle`; `get()` cooperatively waits and may execute
pending work. Call `join()` explicitly to observe failures from *all* forked
tasks: the destructor joins but does not propagate exceptions. Keep the
scheduler alive until groups and handles have finished.

Use `TaskGraph` when *ordering between operations* is the main concern.
Create nodes with `add_task(fn)`, connect them with
`add_dependency(prerequisite,successor)`, and call `run(pool)`. The graph
checks for cycles before running actions and can be reused; do not mutate
or run the same graph concurrently. Dependencies establish execution order,
not tensor/message transport or automatic data-race protection.

For independent batch elements with several short stages, prefer a
`parallel_for` callback that completes every stage of an element or group.
Use a DAG when different tasks genuinely need cross-task prerequisites.
The two forms have different scheduling and cache-locality costs.

### Cancellation, budgets and numerical results

- `parallel_for`, `parallel_reduce` and `parallel_reduce_fixed_blocks`
  accept an optional `std::stop_token`. For a stopped nonempty operation,
  active lanes join and `ParallelOperationCancelled` is thrown instead of
  returning a partial result. Running callbacks are never forcibly stopped.
- `TaskGraph::run(pool, options, stop)` also accepts a token, but returns
  **normally** after cooperative cancellation; not every node need have
  executed. Check the token or track application-level completion if needed.
- `StopTaskGroup` exposes `request_stop()` and shares its token with
  submitted jobs; a stop request is not automatic when one job succeeds.
- `ParallelOptions::spawn_budget` can point to a shared
  `ConcurrencyBudget`. Algorithms try to reserve permits for *additional*
  queued lanes and fall back to caller-only execution when exhausted.
  The budget must outlive its users; avoid blocking `acquire()` inside a
  bounded pool when the releaser may be waiting for a worker.
- `ParallelOptions::rounding_mode` optionally sets thread-local rounding
  for execution lanes; `ScopedRoundingMode` restores the previous mode.
  Strict compiler and floating-point backend behavior is required for
  numerical guarantees. Ordinary `parallel_reduce` can regroup arithmetic;
  `parallel_reduce_fixed_blocks` keeps the combination order stable across
  worker counts, but promises neither correctly rounded whole sums nor
  identical results across platforms.

`ThreadPool(0)` has no dedicated workers. Work can still execute through
the calling thread in cooperative algorithms or helping waits; a standalone
`enqueue()` does not itself start a worker. `ThreadManager` concurrency
zero executes its managed submissions synchronously. Administrative worker
resizing is not a substitute for per-operation task-size or lane limits.

## Architectural choices

### One work-stealing scheduler

`ThreadPool` uses worker-local LIFO queues for spawned descendants, FIFO
stealing by other workers and a global FIFO submission queue for external
work. A worker periodically checks global submissions even when local work
exists. Queue operations use mutexes, not lock-free structures; ordering,
affinity and fairness are not hard real-time guarantees.

Structured waits can help execute queued tasks. Targeted task tickets allow
a waiting `TaskHandle` to claim its own pending task exactly once, avoiding
unrelated recursive helping where possible. These mechanisms improve nested
fork/join progress, but do not prevent deadlocks from cycles, held
application locks or blocking callbacks. Pool shutdown drains outstanding
tasks, assuming the callbacks terminate; resizing is administrative and
must not be performed from within a pool task.

### Separate execution, dependencies and observation

The low-level `TaskGroup`, `parallel_*` algorithms and `TaskGraph` do
not require a Logging-facing progress API. A task graph first validates
dependencies, then dispatches newly ready nodes using a bounded number of
lanes, with no global per-layer barrier. On an action failure it stops
dispatching additional nodes, joins active work and rethrows. This is a
mutex-coordinated ready queue, not an accelerator or lock-free DAG runtime.

`StaticWorkload` and `DynamicWorkload` deliberately retain their
`ThreadManager` and Logging integration for applications that want
progress indicators and established processing behavior. Their separate,
read-only `WorkloadSnapshot` contract supplies coherent progress counts
without forcing Logging into the low-level scheduler. Snapshots are not
completion-success certificates; dynamic workloads may grow after a read.

Generic algorithm templates remain in headers. Non-template runtime
implementations such as `TaskGraph`, `ConcurrencyBudget`,
`ScopedRoundingMode` and `WorkloadCore` live in compiled sources to avoid
unnecessary implementation exposure and repeated compilation. The public
headers document ownership, lifetime and cancellation contracts.

### Workload boundaries

BDD-oriented recursion benefits from sufficiently coarse fork thresholds
and disciplined shared-table synchronization. CDCL/SMT portfolio search
benefits from first-result coordination and cooperative cancellation;
clause learning, proof validation and theory reasoning are outside the
runtime. CPU DNN batches commonly favor grouped `parallel_for` work, while
explicit cross-operation dependencies favor `TaskGraph`. None of these
primitives provide GPU execution or domain-specific optimization.

## Benchmark observations

Measurements are illustrative **Release** runs supplied by a user, not
portable performance claims. The DNN analogue is a small 32–32–8 CPU
network, with independent batch elements and no weight update/optimizer.
At 1,024 elements, a batch grain of 64, four dedicated pool workers and
31 samples, the reported *median* times were:

| DNN mode | Sequential | `TaskGraph` | `parallel_for` | Graph speedup | For speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| Forward | 0.49804 ms | 0.15762 ms | 0.15350 ms | 3.16× | 3.24× |
| Gradient | 0.55688 ms | 0.22379 ms | 0.19067 ms | 2.49× | 2.92× |

The graph used 32 forward or 48 gradient stage nodes; `parallel_for`
used 16 fused batch-group callbacks. Its advantage may reflect fewer
scheduling operations **and** improved cache locality, not DAG overhead
alone. At 128 elements, fine-grained graph scheduling was often slower
than sequential execution; the crossover depends on hardware, workload,
task grouping and system load. Different runs also showed substantial
changes in sequential timings.

The synthetic CDCL candidate-search example (128 candidates, 2,048 mixing
rounds, four workers, seven samples) reported 1.47017 ms sequential
versus 0.26921 ms parallel (5.46×). It is **not** a CDCL solver: parallel
early stopping can change the amount of completed work. These results
cannot establish application-level SMT or DNN training speedups.

For repeatable measurements use `benchmarks/benchmark_use_cases.cpp`
(`--mode`, `--cases`, `--batch-grain`, `--workers`, `--samples`; 
`--work` applies only to CDCL) and `benchmark_thread_pool` for pool
throughput/recursive-task experiments. Compare identical computations,
check result fingerprints and queue drain, vary grain and worker counts,
and report variability as well as median ratios.
