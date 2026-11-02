# Threading: structured concurrency and work stealing

## Scope

These are generic C++20 task-execution primitives, independent of any particular application.

## Execution facilities

- Thread: a managed single-shot thread.
- BufferedThread: a single worker with a bounded queue.
- ThreadPool: a unified pool with FIFO or work-stealing policy, optional helping and administrative resizing.
- ThreadManager: process-wide concurrency control; concurrency zero executes synchronously.
- StaticWorkload and DynamicWorkload: sequential or worker-based processing, including dynamic task generation.
- ThreadPool with work stealing: fixed-size scheduler with local queues and cooperative task stealing.

## ThreadPool cooperative-helping order

The FIFO policy of `ThreadPool` has a mutex-protected list queue. Its dedicated
workers take queued tasks from the front in FIFO order, as before. During
a structured wait, `try_run_one_task()` deliberately selects the newest
queued task from the back (LIFO). This avoids a breadth-first execution
chain in which each nested helping wait starts an older, unrelated subtree
and the call stack grows with the number of queued tasks.

LIFO helping does not guarantee bounded stack usage for arbitrary user
tasks, prevent deadlocks caused by application locks, or establish a
general fairness or completion-order guarantee.

## Targeted helping for structured dependencies

A `TaskHandle` returned by `TaskGroup::fork()` may acquire its **own
pending task** through the scheduler's ticket-based queue operations:
`enqueue_ticket()` inserts the task into the normal queue, while
`try_run_task(ticket)` removes that *same* task under the appropriate queue
mutex and executes it using the scheduler's normal execution context. The
ticket is an opaque scheduler-local identifier; zero denotes an already
executed task in `ThreadManager`'s synchronous mode.

If a worker has already taken the task, targeted acquisition returns false,
and the handle waits for that worker to finish. Mutual exclusion at dequeue
ensures the task executes exactly once; there are no redundant no-op wrappers
left in the queue. Work-stealing tasks still enter the appropriate local
or shared queue, with all normal worker and thief choices preserved.

Unlike generic `try_run_one_task()`, targeted waits do not recurse through
unrelated queued siblings. Arbitrary user recursion and cyclic dependencies
can still exhaust a stack or deadlock. Pending tracked tasks return a stable handle to their original list entry.
A targeted wait locks only the originating queue, checks whether the entry
is still pending, and removes it by its iterator. Workers invalidate the
handle under that same queue mutex when taking the task normally. There is
no global ticket index or shared ticket mutex across worker-local queues.
Ordinary tasks do not allocate a handle. Direct queue handles avoid linear
ticket scans and redundant wrappers, but list-node allocation and cache
locality remain potential costs and require benchmarking.


## TaskGroup and TaskHandle

Include threading/task_group.hpp. Construct TaskGroup<Scheduler> with a scheduler supporting enqueue and try_run_one_task. Each group.fork(callable) returns TaskHandle<Scheduler, Result>. handle.get() waits cooperatively and returns the result or rethrows the captured exception. group.join() waits for every fork before rethrowing the first exception in fork order. The group destructor joins outstanding tasks without throwing. Call join explicitly when errors matter.

The group is owned by a single thread; do not concurrently fork or join on the same group. Nested calls can create their own groups. The scheduler must outlive all handles and groups.

## Recursive fork/join example

~~~cpp
#include "threading/task_group.hpp"
#include "threading/thread_pool.hpp"
#include <functional>

Ariadne::ThreadPool scheduler(4);
std::function<unsigned(unsigned)> compute;
compute = [&](unsigned n) -> unsigned {
    if (n == 0) return 1;
    Ariadne::TaskGroup group(scheduler);
    auto child = group.fork([&,n] { return compute(n-1); });
    unsigned local = compute(n-1);
    unsigned answer = child.get() + local;
    group.join();
    return answer;
};
unsigned result = compute(8);
~~~

Fork only sufficiently large tasks: extremely fine-grained recursion may cost more in scheduling than it saves in parallel execution.

## ThreadPool with work stealing scheduling semantics

Include threading/thread_pool.hpp and construct ThreadPool(number_of_workers); worker count remains fixed and may be zero. enqueue(fn,args...) returns std::future<Result>, which carries task exceptions. External callers submit to a global FIFO. Tasks already running on a pool worker submit their descendants to that worker's local deque. The owner takes newest tasks from the back (LIFO); idle workers take oldest tasks from other deques (FIFO stealing). All queue operations are mutex-protected; there is no lock-free guarantee.

try_run_one_task() executes at most one queued task in the calling thread. A worker tries its own local queue, then global queue, then other workers' queues. External helpers try global work and steal from workers. A helping wait may execute arbitrary nested task bodies. Application locks must not be held across helping waits if those tasks might need the same locks.

queue_size() is an instantaneous count of queued tasks, not currently running tasks. num_threads() reports the dedicated worker count. No guarantees are made about task completion order, affinity, fairness or speedup.

## Shutdown and lifetime

The destructor rejects new external submissions, permits already executing tasks to enqueue descendants, and drains outstanding tasks before joining dedicated workers. A zero-worker pool drains its queue on the destroying thread. Tasks must eventually terminate. Do not destroy the scheduler from one of its own tasks. Destruction must not overlap external submissions or other API calls.

The outstanding-task count includes queued and running tasks; the pending-task count covers only queued work. Separate mutexes protect local queues, the global injection queue and shutdown state. Worker-local scheduling context is temporarily set while executing a task, then restored after nested helping.

TaskGroup destructor cannot cancel stalled user tasks; it always joins. With zero workers, submission alone does not start execution: use a helping wait, try_run_one_task(), or let the pool destructor drain the tasks.

## Verification and current limitations

tests/test_task_group.cpp validates helping and structured joins. tests/test_work_stealing_policy.cpp checks external submission, nested recursion with one worker, value propagation, cooperative stealing, exception draining and safe shutdown, including zero-worker descendants. Tests use the Ariadne Testing suite macros.

Work stealing here is based on mutex-protected deques and scans, not a lock-free algorithm. Benchmarking and scheduler fairness improvements are separate future optimizations; the initial priority is correct task conservation, exception handling and termination.

## Reproducible performance comparison

`benchmarks/benchmark_thread_pool.cpp` compares the resizable FIFO
`ThreadPool` with the fixed-size `ThreadPool with work stealing` under identical loads.
Both use the same `TaskGroup` and `TaskHandle` API.

- `flat`: many independent integer-mixing tasks, submitted by the caller.
- `recursive`: a binary fork/join tree, forking one child and computing the
  other locally, so helping waits and local scheduling are exercised.
  `--cutoff-depth N` computes each remaining subtree sequentially once
  its depth is at most N, without new task submissions; the same seed
  transformations, leaf kernels and combines are used. It defaults to 0,
  and requires N <= the total `--depth`. The binary tree produces
  `2^(depth-cutoff)-1` forked children, with zero children at full cutoff.
  Checksums are compared to the reference evaluated without any cutoff.
  `--cutoff-sweep` measures cutoffs 0,2,...,depth (including the endpoint
  when depth is odd) at fixed workers and work; it reports only recursive
  samples and cannot be mixed with the worker/work `--sweep`.
- Only one pool is alive at a time. Each paired sample creates, warms,
  measures and destroys each pool sequentially. This prevents idle workers
  in the competing pool from affecting measurements.
- Pool creation, full warm-up and destruction are excluded from measured
  time, while fork, task execution and joining are included.
- Every warm-up and timed result is verified against a sequential checksum.
  The queue is checked for leftover entries after each run.
- Alternating order across pairs limits fixed ordering bias. For each pool
  the report gives median, minimum, maximum and median absolute deviation
  (MAD), measured in milliseconds.
- The FIFO/stealing ratio is calculated separately for each pair, then
  summarized with those same statistics; values above one favor stealing.
- `--sweep` runs the 1/2/4/8 worker x 16/128/1024 work matrix. It does
  not accept `--concurrency` or `--work` alongside it; `--mode` can
  restrict the scenarios. It can be more costly than a single comparison.

The benchmark extends the existing `ThreadingCommandLineInterface`, with
the standard logging options. The inherited `-c`/`--concurrency` selects
the number of dedicated workers for each compared pool. The configured
global `ThreadManager` is returned to concurrency zero before measuring
so it does not retain unused workers competing for CPU time. When `-c` is
omitted, the default is min(4, max(1, hardware_concurrency)).
`--tasks`, `--work`, `--depth`, `--samples` and `--mode` select the workload.

These numbers measure microbenchmark throughput and nested-task execution,
not scheduler startup, fairness, end-to-end application costs or general
scalability. They depend on system load, CPU topology, compiler flags and
worker count. Do not interpret a single result as a universal performance
advantage for either scheduler.

## Unified scheduler

`ThreadPool` selects immutable `work stealing` (default) or `work stealing` at construction. Both policies share tickets, execution, shutdown, cooperative helping and worker management. The separate `WorkStealingPool` class is removed intentionally. `ThreadManager::set_concurrency()` remains an administrative configuration operation for the process-wide pool, not a nested task budget; algorithms control fork granularity and share the same scheduler.


## Atomic activity coordination

A single `_activity` atomic combines a stop flag and an outstanding task count. Submission reserves activity before inserting into a physical queue, rolling back on insertion failure. Once shutdown is flagged, external submissions are rejected but existing tasks can submit descendants. Finishing a task decrements activity. An atomic wake-up generation (`_epoch`) replaces the shared state mutex on the per-task hot path. Queue mutexes continue to protect ticket handles and exact-once extraction; work-stealing's topology lock remains separate. Resizing is still administrative and may wait until outstanding work drains.


## FIFO notification optimization (November 2026)

The unified scheduler uses the same atomic activity reservation and task queues as before. Queue publication increments the wake epoch; FIFO signals one waiting worker rather than all of them, avoiding unnecessary contention on its single global queue. Work-stealing publication retains wake-all behavior. Normal task completion does not introduce new runnable work: it now increments the wake epoch and broadcasts only when the outstanding activity count transitions to zero, which is required for shutdown and administrative shrinking. Failed task insertions release their reserved activity through the same completion helper. Targeted ticket acquisition and worker queue removal remain serialized by each queue's mutex. The idle-burst and zero-worker descendant-drain tests exercise wakeups and lifecycle transitions for both policies. These changes require renewed local Release benchmarking; CI tests do not establish microbenchmark performance.


## Single scheduler and fairness (November 2026)

`ThreadPool` no longer exposes scheduling policies. It uses local LIFO queues, FIFO victim stealing, and a global FIFO injection queue. A worker checks global work before its ninth consecutive local dequeue, limiting preference for descendants of one invocation. This is a bound on dispatch choices, not on task execution time or response latency. Cooperative targeted ticket acquisition still executes dependencies exactly once. The benchmark now measures only this scheduler and retains the flat and recursive scenarios and cutoff sweep. `ThreadManager` continues to own the process-wide pool and its administrative concurrency configuration.

## BDD-like fine-grained task cost baseline (November 2026)

The standalone `benchmark_thread_pool --mode micro` measures a single
structured fork plus one local branch, followed by targeted `TaskHandle::get()`
and `TaskGroup::join()`, repeated `--tasks` times. Each iteration computes
exactly the same deterministic result as a sequential baseline. Both paths
use the same integer kernel; the checksum is checked on warm-up and on every
timed sample. A fresh pool is warmed for each paired sample; measurement
order alternates. Pool construction and destruction are excluded from timing.
The reported median paired difference normalized by fork count includes
packaged-task/future allocation, queue insertion, helping or dispatch, and
joining, as well as effects from parallel execution. It must not be labeled
a pure ticket or allocator cost.

The micro workload isolates the short-task regime without replacing the
existing flat-throughput and recursive-cutoff experiments. Run it in a
Release build with `--mode micro --tasks 16384 --work 1 --samples 9` and
`--concurrency 0`, `1`, and `4`. Compare across machines and compiler
settings only with caution. CI smoke runs short zero/two-worker cases to
check compilation and deterministic results, not speed.

Prior to the measurements below, no local performance figures had been recorded.
BDD must choose a sufficiently coarse fork threshold and remove
its table-contention bottleneck.

## Measured microbenchmark baseline and optimization log (2026-11-02)

Status: baseline measurements supplied from a local Release benchmark run;
no scheduler optimization has yet been validated against them. The exact
machine, compiler version/flags beyond Release, system load and raw per-sample
timings were not recorded. These results are *observations*, not portable
performance promises or an isolated cost for any individual component.

The run used `--mode micro --tasks 16384 --samples 9` with three worker
counts and three kernel workloads. All times below are the reported medians
in milliseconds; `delta` is the benchmark's median *paired* difference
in nanoseconds per fork, so it should not be recomputed by subtracting
independently reported medians.

| Workers | Work rounds | Sequential median (ms) | TaskGroup median (ms) | Paired delta (ns/fork) |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 1 | 0.028 | 6.577 | +399.8 |
| 0 | 128 | 12.276 | 19.732 | +451.8 |
| 0 | 1024 | 113.991 | 121.611 | +469.9 |
| 1 | 1 | 0.026 | 39.512 | +2410.0 |
| 1 | 128 | 12.331 | 52.446 | +2444.1 |
| 1 | 1024 | 114.159 | 109.950 | -270.0 |
| 4 | 1 | 0.026 | 42.925 | +2618.4 |
| 4 | 128 | 12.332 | 99.889 | +5335.3 |
| 4 | 1024 | 113.503 | 192.351 | +4812.5 |

A preceding four-worker, work=1 run reported 45.548 ms (TaskGroup)
and +2778.5 ns/fork, versus 42.925 ms and +2618.4 ns/fork in the
matrix above; retain both observations to avoid concealing run-to-run
variation.

Interpretation and uncertainty:

- Zero-worker mode shows approximately 0.40-0.47 microseconds of added
  end-to-end cost per fork, without competing worker threads. This path
  includes allocation and future/ticket/bookkeeping costs, not just a
  scheduling operation.
- One or four workers are substantially more expensive for fine work.
  Four workers at 128 rounds cost +5.335 microseconds/fork, while one
  worker costs +2.444 microseconds/fork. This suggests investigating
  wake-up contention and execution handoff, but does not establish
  either as the bottleneck.
- The one-worker, 1024-round sample shows a small negative paired
  difference. Do not infer a reliable speedup from nine samples.
- The sequential and fork/join paths can be optimized differently by the
  compiler; the microbenchmark uses serial per-iteration join and
  permits little outstanding parallel work. It is useful for overhead
  diagnosis, not sufficient to select a BDD cutoff or claim scaling.
- AddressSanitizer/UBSan, ThreadSanitizer and the platform CI jobs passed
  for the benchmark introduction (`f6ce44b`). CI does not establish
  the performance figures above.

Optimization/verification sequence (not yet completed):

1. **Wake-up experiment:** evaluate replacing global-queue
   `notify_all()` on each external submission with `notify_one()`,
   while preserving broadcasts for worker-local descendants that may
   benefit from stealing, shutdown and idle-zero transitions. Compare
   all nine micro configurations against this baseline, and also test
   external burst throughput and recursive cutoff performance. Do not
   adopt solely because a single micro case improves.
2. **Correctness gates:** maintain exact-once execution, bounded global
   dispatch preference, nested helping, zero-worker draining,
   concurrency resize, and exception semantics. Require Ubuntu,
   macOS, Windows, ASan/UBSan and TSan CI before declaring a candidate
   acceptable. In particular, test burst submissions and worker wakeups
   for missed notifications and lost parallelism.
3. **Allocation profile:** if the zero-worker path remains expensive,
   instrument `packaged_task`, `shared_future`, ticket and joiner
   construction. Optimize only components identified by measurements;
   do not weaken the safe lifetime of task handles.
4. **BDD readiness:** compare 0/1/4 workers with multiple `--work`
   sizes and the existing recursive `--cutoff-sweep`, then benchmark
   representative ROBDD ITE workloads on sharded tables. BDD owns
   grain-size/cutoff policy and should not fork subproblems whose
   useful work is below the measured break-even region.

Record each accepted or rejected experiment with its own before/after
measurements, CI status, semantic regressions if any, and resulting
decision. No performance optimization is considered completed by this
baseline documentation update.

## External injection wake-one candidate (2026-11-02)

**Status: implemented as a candidate; runtime and performance verification
pending on this commit.** This follows the 0/1/4-worker, work=1/128/1024
microbenchmark baseline above. Do not report any speedup until a comparable
post-change measurement is available.

Implementation:
- Global injection (external producer, or worker without an active local
  queue) now calls `_epoch.notify_one()` per published task, instead of
  waking every worker. Locally queued descendants continue using
  `_epoch.notify_all()` so idle workers can steal them. Each publication
  still increments the monotonic worker epoch before notifying.
- **Correctness dependency:** worker wakeups and administrative shrinking
  previously shared `_epoch`. With wake-one, a shrinking thread could
  consume the notification intended for a sleeping worker while waiting
  for outstanding activity, potentially leaving queued tasks stalled.
  A separate monotonic `_drain_epoch` now notifies shrink waiters only
  when outstanding activity reaches zero. Worker epoch broadcasts remain
  for pool resizing, shutdown and zero-activity transitions. This avoids
  an ABA hazard that a simple wait on the outstanding-activity count
  could introduce if it returns to the same nonzero value.
- Regression cases check burst parallelism across four idle workers
  and draining of queued external submissions during a reduction from
  two workers to one, retaining the existing exact-once, fairness,
  nested-helping, zero-worker and cancellation-free shutdown tests.

Potential benefit: reduce unnecessary wakeups and worker contention
for externally submitted fine-grained tasks. Potential cost: a single
external publication wakes only one worker, while fast bursts may take
time to distribute across all workers; mixed local/global workloads
may still cause contention. Neither outcome is established by CI.

**Verification record (to be filled only from actual results):**
- Build, platform tests, ASan/UBSan and TSan: pending.
- Post-change `micro` matrix, all nine combinations, same machine:
  pending.
- External burst throughput and `recursive --cutoff-sweep`: pending.
- Decision to retain/revert optimization based on measured benefit:
  pending. A green CI is a safety prerequisite, not a performance result.
