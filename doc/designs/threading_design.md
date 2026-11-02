# Threading: structured concurrency and work stealing

## Scope

These are generic C++20 task-execution primitives, independent of any particular application.

The three reference use cases, consistently named throughout future
design and benchmarking, are:

- **BDD**: recursive and memoized symbolic operations with structured fork/join.
- **CDCL**: satisfiability-oriented search, including a CDCL-based search
  engine coupled with theory reasoning in an SMT solver. The term CDCL
  labels the family of execution needs; it does not limit the runtime to
  purely propositional solving or prescribe a specific solver architecture.
- **DNN (Deep Neural Networks)**: both network training and inference,
  including CPU data parallelism, reductions and eventual task dependency orchestration.

The Threading library provides shared concurrency mechanisms, not BDD
operations, a CDCL or SMT engine, or neural-network operators. Solver
heuristics, theory propagation, model semantics and accelerators are
owned by their respective application layers.

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

**Status: REJECTED; scheduler changes reverted after local benchmarking.**
Candidate commit: `b88d345`. The following describes what was tried,
not the behavior of the current scheduler.

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

**Verification record and measured outcome:**

- CI at `b88d345`: Ubuntu, macOS, Windows, Coverage and Sanitizers
  (ASan/UBSan, TSan) all passed. No semantic regression was detected by
  these tests. This does not establish a performance improvement.
- The user repeated the Release microbenchmark matrix with 16,384
  forks per sample, nine samples per configuration, 0/1/4 workers
  and 1/128/1024 rounds per branch. Independent median times are
  shown in milliseconds; overhead is the median *paired* difference
  in nanoseconds per fork, not a subtraction of independent medians.
- Before is the preceding local baseline under the same command-line
  parameters. Exact CPU model, system load and compiler invocation
  were not recorded; the runs were not interleaved/randomized.

| Workers | Rounds | Candidate sequential median (ms) | Candidate TaskGroup median (ms) | Before overhead (ns/fork) | After overhead (ns/fork) | Change (ns/fork) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 1 | 0.028 | 6.751 | 399.8 | 410.3 | +10.5 |
| 0 | 128 | 12.329 | 19.951 | 451.8 | 460.5 | +8.7 |
| 0 | 1024 | 115.286 | 122.156 | 469.9 | 436.2 | -33.7 |
| 1 | 1 | 0.026 | 39.493 | 2410.0 | 2408.6 | -1.4 |
| 1 | 128 | 12.335 | 55.725 | 2444.1 | 2649.3 | +205.2 |
| 1 | 1024 | 114.061 | 119.328 | -270.0 | 321.5 | +591.5 |
| 4 | 1 | 0.025 | 48.649 | 2618.4 | 2967.8 | +349.4 |
| 4 | 128 | 12.359 | 91.773 | 5335.3 | 4852.0 | -483.3 |
| 4 | 1024 | 113.214 | 191.474 | 4812.5 | 4776.6 | -35.9 |

Positive change denotes regression. Four-worker work=128 improved
by 483.3 ns/fork (~9% of overhead) and the TaskGroup median
fell from 99.889 to 91.773 ms (~8%). However, four-worker work=1
regressed from 42.925 to 48.649 ms (~13%), and one-worker work=128
regressed from 52.446 to 55.725 ms (~6%). The one-worker 1024-round
baseline was noisy; its single negative overhead is not a reliable
speedup. The results do not support a consistent net gain.

**Decision:** revert `notify_one()` and its dependent `_drain_epoch`
change, restoring the original unified `_epoch` broadcasts, while
retaining the two new correctness regression tests for external bursts
and concurrent resizing. The additional complexity is not justified
without a repeatable benefit. Full external-burst *throughput* and
recursive-cutoff before/after performance runs were not provided;
therefore the result is rejection on current evidence, not a proof
that selective wakeups can never help.

**Next experiment:** isolate the no-worker TaskGroup allocation and
result-handle costs (~400-470 ns/fork baseline) with targeted
instrumentation, then compare against the unchanged micro and
recursive baselines. Preserve safety and semantic coverage; do not
replace task handles with non-owning references solely for speed.


## Revert of the wake-one candidate (2026-11-02)

The worker notification change and the independent drain epoch from
`b88d345` were reverted to the `c2fc26f` implementation.
Two additional correctness tests introduced by the experiment are
retained. This is a source-level rollback, not a new performance
measurement; validation of the rollback commit by CI has completed successfully (Ubuntu,
macOS, Windows, Coverage, ASan/UBSan and TSan).


## Fine-grained TaskGroup cost decomposition (2026-11-02)

A `breakdown` mode was added to the existing Release benchmark so that
fine-grained costs can be compared without competition from worker
threads. It runs a deterministic 0-worker workload with identical
integer work and checksum through five overlapping paths:
sequential; packaged task/shared future run directly; scheduler ticket
and targeted execution without a future; packaged task plus a manual
`TaskHandle` without group bookkeeping; full `TaskGroup::fork/get/join`.
Both warm-up and timed runs check the reference checksum and drained
queue. Per-stage paired differences are relative to sequential, and the
stage order rotates. The stages **overlap**: their differences cannot
be added to infer individual allocation costs. In particular, the
manual-handle minus group difference *suggests*, but does not prove,
the cost of owning/recording a joiner. This benchmark is diagnostic
instrumentation, not a scheduler optimization.

The first user-supplied local Release measurement used 16,384 iterations,
work=1, nine samples, and zero workers:

| Diagnostic stage | Median (ms) | Paired delta (ns/iteration) |
| --- | ---: | ---: |
| Sequential | 0.025 | 0.0 |
| Future only | 1.590 | 95.5 |
| Ticket only | 1.318 | 79.0 |
| Handle, no group | 4.010 | 243.2 |
| TaskGroup | 5.799 | 352.4 |

These are medians for the specific local run, not independent additive
component costs. The difference of 109.2 ns/iteration between the complete
group path and the manual handle path is an *upper-level measurement* of
multiple effects including joiner registration, copies, allocation and
differences in generated code. It is not an isolated vector allocation cost.
The pre-existing zero-worker micro baseline remains
399.8/451.8/469.9 ns/fork for work=1/128/1024; it is a different run
and should not be conflated with the breakdown measurement.

**Candidate optimization: one inline joiner (2026-11-02).**
A TaskGroup now stores the first joiner in its own `std::function<void()>`
member and only allocates a vector for subsequent forks. This avoids the
vector's initial buffer allocation in the common fork-one/compute-one/join
pattern used by BDD. The callable wrapper can still allocate heap memory;
no claim is made that the full 109.2 ns is removed.

Semantic invariants: every fork registers an owning joiner; exceptions
are observed in fork order after joining all children; the destructor
joins even when an explicit join was omitted; a group can be joined
repeatedly and used for later forks; a failed joiner registration still
waits for its launched child. Dedicated regression tests cover empty
rejoin/refork, multiple exceptions in order, and multiple children
drained by destruction, in addition to the existing deep recursion,
exact-once, zero-worker and worker-mode coverage.

**Verification at candidate creation:** source changes published and
regression tests added; platform CI and sanitizer outcomes pending.
**Performance:** pending a same-machine Release comparison with
`--mode micro --concurrency 0/1/4` and a new `breakdown` run.
Retain or revert based on semantic verification and reproducible
end-to-end performance, not on the theoretical removal of one
allocation.


## Tracked-ticket handle fast-path experiment (2026-11-02)

The first local comparison after the inline-joiner change (`114f5d4`)
used `--mode breakdown --concurrency 0 --tasks 16384 --work 1
--samples 9`. Reported previous vs inline-joiner paths:

| Stage | Previous median (ms) | Inline median (ms) | Previous delta (ns/iteration) | Inline delta (ns/iteration) |
| --- | ---: | ---: | ---: | ---: |
| Sequential | 0.025 | 0.025 | 0.0 | 0.0 |
| Future only | 1.590 | 1.616 | 95.5 | 97.1 |
| Ticket only | 1.318 | 1.345 | 79.0 | 80.6 |
| Handle, no group | 4.010 | 4.077 | 243.2 | 247.3 |
| TaskGroup | 5.799 | 5.683 | 352.4 | 345.4 |

The full TaskGroup path decreased by 7.0 ns/iteration (~2%).
The TaskGroup-minus-manual-handle difference decreased from
109.2 to 98.1 ns/iteration (11.1 ns), but these were separate runs
without randomized/interleaved comparisons or allocation tracing;
neither reduction can be confidently attributed to avoiding a
specific allocation. **The inline-joiner change is provisional, not
a confirmed performance gain.** Ubuntu, macOS, Coverage and
ASan/UBSan plus TSan passed on `114f5d4` at last check; Windows
was still queued then.

**New candidate:** `TaskGroup::fork` previously wrapped a
`ThreadPool::Ticket` in a capturing `std::function<bool()>`,
which could allocate and was copied when registering its joiner.
`TaskHandle` now stores a direct ticket in a `std::variant` and
calls the same `try_run_task(ticket)` when waiting; no acquisition
callback is needed for ThreadPool/ThreadManager. The public generic
callable constructor and the fallback for custom scheduler ticket
types are retained. An empty ticket remains valid for
ThreadManager's synchronous mode. The manual-handle benchmark stage
now uses the same direct-ticket path as `TaskGroup`.

The change may increase handle object size even while reducing
allocator traffic, so the effect is not predictable from code
inspection alone. Correctness regressions explicitly exercise the
generic constructor, and existing tests cover exception ordering,
worker/zero-worker execution, tickets and deep recursion.

**Status:** benchmark candidate, no new local measurements; new CI
and sanitizers pending. Re-run `breakdown` and the 0/1/4-worker
`micro` matrix before accepting or rejecting this version.


## Direct-ticket TaskHandle results and public umbrella (2026-11-02)

User-reported local Release breakdown for the direct-ticket candidate
(`fa0c13c`): `--mode breakdown --concurrency 0 --tasks 16384 --work 1
--samples 9`. Compare against the preceding inline-joiner version
(`114f5d4`) on the same user's machine, with all figures in their
respective independent runs:

| Stage | Before median (ms) | After median (ms) | Before paired ns/iter | After paired ns/iter |
| --- | ---: | ---: | ---: | ---: |
| Sequential | 0.025 | 0.025 | 0.0 | 0.0 |
| Future only | 1.616 | 1.585 | 97.1 | 95.3 |
| Ticket only | 1.345 | 1.333 | 80.6 | 79.9 |
| Handle, no group | 4.077 | 3.718 | 247.3 | 225.5 |
| TaskGroup | 5.683 | 4.912 | 345.4 | 298.3 |

The full group path decreased by 47.1 ns/iteration (about 13.6%
of the previous overhead) and the tracked handle path by 21.8
ns/iteration (about 8.8%). The TaskGroup minus manual handle
difference decreased from 98.1 to 72.8 ns/iteration, but these
paths overlap and cannot isolate an allocation. Run-to-run
variation, compiler differences between instantiated paths and
different capture/copy costs remain possible. These numbers
support keeping direct tickets as a promising candidate, not
a universal speedup or a completed performance verification.
Ubuntu, macOS, Coverage and Sanitizers had passed for
`fa0c13c` at the last review; Windows was still running.
The recursive-cutoff and 0/1/4-worker micro matrix on this
revision remain unmeasured.

As requested, `include/ariadne-threading.hpp` now exposes the
public Threading headers via one include. It deliberately
includes all twelve headers directly under `include/threading/`
so the aggregate remains auditable as the API grows, and does
not add test-only or private implementation headers. The
configuration's `ariadne_register_public_headers(FILES ...)`
mechanism registers it for the same bundle destination as
`threading/` and its public dependencies. A new CTest
executable includes this header first and checks representative
API usage; Ubuntu Release CI independently compiles a translation
unit against the *installed* umbrella and header bundle.

**Status at commit creation:** umbrella and documentation edits
published, installation and CTest CI results pending.
Next steps are to confirm the CI and re-run the full micro
matrix to determine whether the direct-ticket path improves
the parallel workloads; do not introduce additional scheduler
complexity based solely on the zero-worker result.


## Controlled callback-versus-ticket comparison (2026-11-02)

At `63adf6a`, Ubuntu, macOS, Windows, Coverage and Sanitizers all
completed successfully; the umbrella installed-header compilation
check also passed as part of the Ubuntu Release workflow.
This establishes basic compilation and regression safety, not a
parallel speedup.

The previous before/after `breakdown` measurements compared binaries
run at different times. To reduce this confounder, the benchmark now
offers `--mode handle-compare`, which measures **within the same
binary and warmed pool** the legacy `std::function<bool()>` callback
constructor and the new direct-ticket constructor. Both variants
package the same callable, enqueue one ticket, make one owning
joiner, compute the other branch locally, get the result and join.
A third path uses the complete current `TaskGroup`. Each sample
warms all three variants, rotates their timing order, checks equal
checksums and demands an empty scheduler queue. It reports medians
and medians of *paired* differences in ns/fork; a negative direct
minus callback favors the direct ticket.

The callback variant uses the **current** `TaskHandle` (including its
`std::variant` storage and dispatch), not the historical binary.
The comparison can isolate the current acquisition representation
but not all costs introduced by the historical refactor. The
`TaskGroup` path is separately reported and includes extra group
bookkeeping, so do not attribute its difference entirely to
ticket acquisition.

**Status:** diagnostic benchmark committed; performance readings
and new CI validation pending. No additional scheduler/TaskGroup
optimization is introduced by this change. Benchmark 0/1/4 workers
with `--tasks 16384 --work 1 --samples 9`, then validate at
larger grain size and compare the existing recursive cutoff sweep
before any further performance claim.


## Same-binary callback/ticket comparison (2026-11-02)

The Release `handle-compare` experiment used 16,384 iterations,
work=1, nine samples, and one warmed pool per paired sample.

| Workers | Callback median ms | Direct median ms | TaskGroup median ms | Paired direct - callback ns/fork | Paired group - direct ns/fork |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 4.957 | 4.489 | 5.020 | -28.7 | +31.5 |
| 1 | 35.960 | 32.505 | 36.762 | -225.2 | +335.4 |
| 4 | 52.726 | 48.900 | 50.346 | -173.8 | +52.5 |

Median absolute deviation (ms): callback 0.015/0.835/2.518,
direct 0.010/1.237/1.172, group 0.031/0.928/0.673, respectively
for 0/1/4 workers. Paired differences above are the medians of
within-sample differences, not differences between median times.

All three paired comparisons favor direct-ticket acquisition in the
current binary. The callback path uses the current TaskHandle layout,
not the historical binary; the test does not prove historical
whole-library speedup. Group-versus-handle differences are not
isolated group bookkeeping costs. The CI for `dd18720` passed
Ubuntu, macOS, Windows, Coverage and Sanitizers.

Decision: retain the direct-ticket acquisition path and the generic
callback constructor. Do not assume the differences are solely due to
allocations. Next compare work=128/1024 and recursive cutoffs before
further algorithmic changes.


## Larger-grain handle comparison: mixed outcomes (2026-11-02)

Six further user-supplied Release `handle-compare` runs used 16,384
forks, nine samples, the same benchmark binary and work=128 or 1024
rounds. The table preserves independent elapsed-time medians and
the separately measured *paired* medians of the difference:

| Workers | Rounds | Callback median ms | Direct median ms | Group median ms | Paired direct - callback ns/fork | Paired group - direct ns/fork |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 128 | 19.228 | 18.441 | 18.434 | -44.6 | +1.7 |
| 1 | 128 | 52.150 | 68.948 | 60.612 | +1000.9 | -383.2 |
| 4 | 128 | 105.359 | 105.917 | 104.394 | -47.0 | +18.1 |
| 0 | 1024 | 120.987 | 120.355 | 120.538 | -35.6 | +22.7 |
| 1 | 1024 | 117.800 | 127.965 | 162.551 | +802.9 | +1699.8 |
| 4 | 1024 | 191.125 | 189.352 | 189.107 | -91.8 | -68.1 |

Per-stage median absolute deviations (callback / direct / group, ms):

| Workers | Rounds | Callback MAD | Direct MAD | Group MAD |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 128 | 0.116 | 0.070 | 0.049 |
| 1 | 128 | 0.236 | 0.555 | 2.527 |
| 4 | 128 | 0.393 | 0.877 | 0.240 |
| 0 | 1024 | 0.262 | 0.211 | 0.139 |
| 1 | 1024 | 2.767 | 1.760 | 7.497 |
| 4 | 1024 | 0.270 | 0.742 | 0.537 |

**Interpretation and change to the previous decision:**
- With zero workers, direct acquisition wins both further
  comparisons, by 44.6 and 35.6 ns/fork; no competing worker can
  change execution ownership.
- With one worker, direct acquisition loses substantially:
  +1000.9 ns/fork at 128 rounds and +802.9 ns/fork at 1024 rounds.
  The independent direct medians regress by 32.2% and 8.6%
  against the callback implementation, respectively.
- With four workers, the direct path has modestly favorable paired
  differences, but at 128 rounds its independent elapsed-time
  median (105.917 ms) is slightly *higher* than the callback median
  (105.359 ms). This is mathematically possible: median of paired
  differences is not the difference of medians. Do not erase this
  discordance or cherry-pick either summary.
- The full group path varies greatly relative to the manual direct
  handle at one worker (+1699.8 ns/fork at 1024 rounds); its residual
  is not an isolated joiner/allocation measurement.

**Hypothesis, not demonstrated:** the slower one-worker outcomes
may reflect a changed race between a worker dequeuing the task and
the calling thread attempting targeted acquisition, with different
allocation/dispatch timing changing which side runs the left
branch. The current benchmark reports no execution-ownership
counts, so it cannot prove this mechanism. Scheduler contention
and system load are alternative explanations.

**Decision revised:** keep the direct-ticket implementation in
place temporarily as a *candidate*, not as a demonstrated universal
improvement. The prior work=1 results favored it in all three
worker configurations, but the six new runs refute a blanket
speedup claim. No production policy change or further scheduler
optimization is justified yet. Repeat the one-worker cases with
additional independent runs; collect evidence about whether tasks
run on the caller or a worker, and compare realistic recursive
cutoff workloads. Consider rollback of the fast path if these
regressions reproduce. Preserve the generic callback constructor
and correctness coverage regardless of performance choice.


## Repeated one-worker comparisons and ownership hypothesis (2026-11-02)

Three independent local Release repeats per grain size, each with
nine paired samples and 16,384 forks, produced:

| Work rounds | Repeat | Callback median (ms) | Direct median (ms) | TaskGroup median (ms) | Paired direct - callback (ns/fork) | Paired TaskGroup - direct (ns/fork) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | 1 | 52.475 | 66.496 | 68.305 | +855.7 | +110.5 |
| 128 | 2 | 52.241 | 66.689 | 62.639 | +905.2 | -253.9 |
| 128 | 3 | 47.913 | 64.242 | 69.289 | +1073.3 | +292.3 |
| 1024 | 1 | 116.541 | 149.112 | 163.212 | +1985.7 | +972.5 |
| 1024 | 2 | 159.239 | 155.335 | 148.026 | -191.9 | +382.6 |
| 1024 | 3 | 116.765 | 155.598 | 160.988 | +2052.6 | +219.6 |

The median of the three *run-level paired medians* is +905.2
ns/fork for work=128 and +1985.7 ns/fork for work=1024.
These are not medians of all 27 raw sample differences,
which were not supplied. At work=128, all three repeated
runs strongly favor the callback; at 1024, two favor the
callback while one reverses the sign. The callback time in
the inverted 1024-round repeat rises to 159.239 ms versus
116.541/116.765 ms in the other two, evidence of substantial
run-to-run state changes. The regression is repeatable for
128 rounds but the effect at 1024 is not stable enough to
estimate precisely. The previous claim that direct-ticket
acquisition is broadly preferable is no longer supported.

**Status:** the direct-ticket path remains a candidate.
No production code was changed to explain away the regression.
The one-worker case suggests an execution-ownership or
contention effect, but neither the callback nor direct
timing samples report who executes the forked task.

A new diagnostic `--mode ownership` now counts task-body executions
on the submitting thread versus pool workers, for the same
single-fork/compute-one/join structure with callback and direct
handles. It reports individual trial counts and paired caller-share
differences, with warm-up and correctness checks and one fresh
pool per trial. Because it adds `std::this_thread::get_id()`
and relaxed atomic increments to task bodies, these results
**are not measurements of execution ownership in the uninstrumented
benchmark**. They can inform, but cannot conclusively establish,
the scheduling-race hypothesis. These counts are deliberately
untimed and must not be mixed with the existing speed results.

**Next verification:** execute `--mode ownership --concurrency 1
--tasks 16384 --samples 9` with work=128 and work=1024;
then evaluate whether measured ownership shifts correlate with
the observed slowdown. A rollback remains a legitimate
decision if the one-worker end-to-end regression persists.


## Ownership diagnostic evidence and TaskGroup default rollback (2026-11-02)

The user ran the untimed `--mode ownership --concurrency 1 --tasks
16384 --samples 9` benchmark at work=128 and work=1024 on the
direct-ticket candidate `6f43f6b`. Each row below counts exactly
16,384 executions in either the callback or direct variant.
The instrumented code checks the executing thread ID and increments
a relaxed atomic in each task, so these counts **do not directly
describe the ownership distribution of uninstrumented timed runs**.

| Rounds | Sample | Callback caller | Callback worker | Direct caller | Direct worker | Direct - callback caller share (pp) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | 1 | 4989 | 11395 | 991 | 15393 | -24.4 |
| 128 | 2 | 8400 | 7984 | 282 | 16102 | -49.5 |
| 128 | 3 | 8033 | 8351 | 404 | 15980 | -46.6 |
| 128 | 4 | 7845 | 8539 | 469 | 15915 | -45.0 |
| 128 | 5 | 8052 | 8332 | 270 | 16114 | -47.5 |
| 128 | 6 | 8512 | 7872 | 1612 | 14772 | -42.1 |
| 128 | 7 | 8138 | 8246 | 186 | 16198 | -48.5 |
| 128 | 8 | 7931 | 8453 | 295 | 16089 | -46.6 |
| 128 | 9 | 7859 | 8525 | 279 | 16105 | -46.3 |
| 1024 | 1 | 3881 | 12503 | 361 | 16023 | -21.5 |
| 1024 | 2 | 3648 | 12736 | 2624 | 13760 | -6.2 |
| 1024 | 3 | 4433 | 11951 | 1689 | 14695 | -16.7 |
| 1024 | 4 | 3853 | 12531 | 2174 | 14210 | -10.2 |
| 1024 | 5 | 3819 | 12565 | 638 | 15746 | -19.4 |
| 1024 | 6 | 4645 | 11739 | 297 | 16087 | -26.5 |
| 1024 | 7 | 3914 | 12470 | 2545 | 13839 | -8.4 |
| 1024 | 8 | 4165 | 12219 | 276 | 16108 | -23.7 |
| 1024 | 9 | 4283 | 12101 | 2329 | 14055 | -11.9 |

Reported median caller ownership is 49.0% for callback versus
1.8% for direct at 128 rounds, a paired median change of -46.6
percentage points; at 1024 rounds it is 23.9% versus 10.3%,
paired median change -16.7 percentage points. All 18 instrumented
pairs shift work away from the caller with direct tickets. The
large shift for work=128 is consistent with its three independent
timed repeats showing a direct-ticket slowdown
(+855.7/+905.2/+1073.3 ns/fork), but causality is **not** proven:
the instrumented race differs from the timed race, and worker
handoff, scheduler contention and system conditions remain
entangled. The 1024-round timings varied substantially and one
repeat favored direct, despite the ownership shift.

**Decision:** conservatively restore the callback representation
as the default for `TaskGroup::fork()`, without changing the
ticket-based queue, its exact-once semantics, `TaskHandle::wait()`,
the single inline group joiner, or the scheduler. Keep the explicit
direct-ticket `TaskHandle` constructor, the `handle-compare`
and `ownership` diagnostic modes, and a regression test for the
explicit direct-ticket path. This preserves an easy reversible
performance experiment while avoiding the repeatable one-worker
regression for the default path under the tested workload.

This is **not** a claim that the callback representation is faster
for all workloads: work=1 and the 0/4-worker comparisons often
favored direct. The rollback is a conservative choice given the
currently observed one-worker losses and a still-missing realistic
BDD recursive workload. As of this edit, post-rollback local
performance measurements and the new CI results remain pending.
Re-run the full `micro` matrix and recursive cutoff comparison
before considering the default performance decision final.


## Recursive cutoff sweep after restoring callback acquisition (2026-11-02)

The user ran the Release recursive cutoff sweep after the
`TaskGroup::fork()` default acquisition rollback (`d8b0710`):
`--mode recursive --cutoff-sweep --depth 9 --work 128 --samples 9`,
with 0, 1, and 4 dedicated worker threads. The workload has
512 leaf evaluations in every case; only how deeply the
recursion forks changes. Each sample uses a fresh warmed pool.
Pool startup/teardown is not timed. These are *post-rollback*
measurements only, not a historical before/after A/B benchmark.

| Workers | Cutoff | Forks | Median ms | Min ms | Max ms | MAD ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 511 | 0.790 | 0.688 | 0.886 | 0.009 |
| 0 | 2 | 127 | 0.398 | 0.377 | 0.409 | 0.002 |
| 0 | 4 | 31 | 0.303 | 0.267 | 0.305 | 0.000 |
| 0 | 6 | 7 | 0.253 | 0.211 | 0.263 | 0.001 |
| 0 | 8 | 1 | 0.208 | 0.194 | 0.208 | 0.000 |
| 0 | 9 | 0 | 0.193 | 0.193 | 0.194 | 0.000 |
| 1 | 0 | 511 | 0.310 | 0.298 | 0.473 | 0.010 |
| 1 | 2 | 127 | 0.157 | 0.151 | 0.161 | 0.001 |
| 1 | 4 | 31 | 0.128 | 0.120 | 0.136 | 0.003 |
| 1 | 6 | 7 | 0.132 | 0.124 | 0.138 | 0.006 |
| 1 | 8 | 1 | 0.135 | 0.132 | 0.157 | 0.000 |
| 1 | 9 | 0 | 0.226 | 0.226 | 0.226 | 0.000 |
| 4 | 0 | 511 | 0.430 | 0.346 | 0.482 | 0.025 |
| 4 | 2 | 127 | 0.164 | 0.144 | 0.190 | 0.016 |
| 4 | 4 | 31 | 0.082 | 0.070 | 0.122 | 0.005 |
| 4 | 6 | 7 | 0.075 | 0.066 | 0.093 | 0.009 |
| 4 | 8 | 1 | 0.109 | 0.107 | 0.119 | 0.002 |
| 4 | 9 | 0 | 0.202 | 0.201 | 0.202 | 0.000 |

Findings:
- Zero workers: the no-fork cutoff=9 case is fastest (0.193 ms);
  creating 511 tasks takes 0.790 ms, about 4.1 times as long.
- One worker: the observed minimum median is cutoff=4
  (31 forks, 0.128 ms). Cutoff=6 (7 forks, 0.132 ms) is only
  0.004 ms slower, comparable to sample MADs, so a precise
  optimum has not been established.
- Four workers: the observed minimum median is cutoff=6
  (7 forks, 0.075 ms). Cutoff=4 (31 forks, 0.082 ms) is
  0.007 ms slower; the cutoff=6 MAD is 0.009 ms.
  Relative to 511 forks at cutoff=0 (0.430 ms), cutoff=6
  is about 5.7 times faster. Relative to no forking on
  the same four-worker pool (0.202 ms), it is about 2.7
  times faster. These are ratios of measured medians,
  not confidence intervals or general BDD speedups.
- All measured runs pass the benchmark reference checksum.
  Cutoff=9 performs the same purely sequential computation
  but its medians differ across the three runs (0.193,
  0.226, 0.202 ms). Worker availability, system state,
  ordering and measurement duration can confound
  cross-configuration comparisons; do not assume a fixed
  machine-independent sequential baseline.
- Each scenario finishes in fractions of a millisecond,
  and only nine samples were collected. Do not infer a
  robust exact grain threshold from the lowest single median.

**Decision:** retain the callback-based TaskGroup default after
the one-worker ticket regression; the current recursive results
demonstrate a strong need for coarse-grained spawning, but do
not compare callback and direct-ticket defaults on the same
recursive workload. No scheduler or TaskGroup code change
is warranted from this sweep alone.

**Next experiment:** repeat the recursive cutoff sweep at
depth=13 with the same work=128 for 0, 1 and 4 workers, to
increase the total useful work from 512 to 8192 leaves and
make sub-millisecond timing noise less dominant. Compare
the broad cutoff plateau before selecting a BDD cutoff.
A true ROBDD ITE benchmark is still required: memoization
and sharded-table contention are absent from this synthetic
integer recurrence.

CI status verified at rollback `d8b0710`: Ubuntu, macOS,
Windows, Coverage and Sanitizers all passed.


## General-purpose CPU primitives for BDD, CDCL and DNN (2026-11-02)

The intended uses now include BDD fork/join, independent long-lived
CDCL search workers (also usable within SMT solvers), and DNN training
and inference through CPU data-parallel operations. This does not establish that BDD is performance-complete:
realistic BDD ITE with memoization and contention is still unmeasured.

**Implementation, first increment:**
- `StopTaskGroup<Scheduler>` is a scope for cooperative portfolio
  cancellation, built over the existing `TaskGroup`. Each forked
  callable receives a `std::stop_token`; `request_stop()` is safe
  from a worker, the owning thread performs fork/join, and the
  destructor requests stop before joining. There is no forceful
  cancellation, and the stop state is intentionally not resettable.
  Ordinary task exceptions still propagate at explicit `join()`.
- `parallel_for(ThreadPool&,first,last,fn,ParallelOptions)` partitions
  an index interval into bounded contiguous lanes. The caller is one
  lane; up to `pool.num_threads()` additional lanes execute via the
  existing pool, subject to `max_tasks` and `grain_size`. A zero-worker
  pool executes sequentially. All lanes join even on exceptions.
- `parallel_reduce(ThreadPool&,first,last,identity,map,reduce,options)`
  reduces within static contiguous lanes, then combines lane
  partials in a fixed order. Reduction requires an associative
  operator and two-sided identity; FP summation may differ across
  lane counts. Functions used from several lanes must be thread-safe.

These building blocks deliberately avoid a new scheduler, a task
graph engine, clause-exchange protocol, GPU backend, automatic
performance claims or implicit nested parallelism quotas.
`max_tasks` limits one call rather than reserving global CPU
capacity. Cancellation is cooperative rather than preemptive;
a solver loop that never checks the token can still block join.

**Verification plan:** `tests/test_parallel.cpp` covers serial and
worker execution, exact-once loop indices, reductions, invalid
and empty ranges, exception draining, CDCL-like first-winner
stop propagation and scoped joins. The umbrella and installed
header compilation CI ensure headers are exported. Platform
CI outcomes are pending at commit creation. Future work should
introduce explicit concurrency budgets, bounded communication
channels, and benchmarks against realistic CDCL and numerical
workloads before expanding policy complexity. Actual GPU
execution belongs in an optional integration layer, not the
core CPU scheduler.


## Terminology clarification for the three use cases (2026-11-02)

Use **BDD**, **CDCL**, and **DNN** as the canonical use-case labels for
future requirements, implementation notes, and benchmarks. In
particular, **CDCL** means concurrent satisfiability-search workloads,
including the CDCL-based search layer of an SMT solver; it must not
be used as shorthand for a particular propositional solver product.
Theory integration is an application-layer responsibility. **DNN**
means Deep Neural Networks and covers both training and inference,
initially on the CPU; GPU-specific execution remains a distinct
integration topic.
These names do not imply that the three applications have the same
scheduling or cancellation needs.

This is documentation/terminology alignment only; it does not modify
scheduling, cancellation behavior, public APIs, or benchmark results.


## Terminology update: DNN replaces DL (2026-11-02)

The canonical use cases are now **BDD**, **CDCL**, and **DNN**.
The latter expands to **Deep Neural Networks**, explicitly covering
both **training** and **inference** of deep networks. This is a naming
change only; it does not constrain the DL algorithms, add neural
network functionality, or modify scheduler behavior. The broader
term *deep learning* may still be used descriptively where useful,
but **DNN** is the reference use-case label.


## CDCL and DNN coordination primitives (2026-11-02)

Added two generic C++20 primitives, independent of application logic:
- `BoundedChannel<T>`: mutex-protected bounded FIFO with nonblocking
  `try_send`/`try_receive`, blocking `send`/`receive` accepting
  `std::stop_token`, and idempotent `close`. A closed channel retains
  its buffered messages for draining; cancellation does not consume
  buffered items. Move-only payloads are supported. Callers must avoid
  pool starvation by not blocking all workers on mutually dependent
  channels. Destruction requires all users to have finished.
- `ConcurrencyBudget`: a shared counting admission budget with move-only
  RAII `Permit`, nonblocking `try_acquire`, and stop-aware blocking
  `acquire`. It does not cap the scheduler's physical workers or reserve
  CPU cores. Blocking on permits from a bounded pool can deadlock when
  queued tasks must release them; use `try_acquire` from pool tasks.

These serve CDCL portfolio communication and common CPU capacity
coordination for BDD, CDCL and DNN, without specifying clause formats
or numerical operators. The first tests cover FIFO order, close and
drain, stop, move-only messages, independent producer/consumer tasks,
permit release and invalid capacities. Follow up with concurrency
stress, timeouts, and realistic workload benchmarks before treating
them as production performance results.

README policy: the public README contains only the introduction,
feature summary, and build instructions. Examples, benchmarks, tests,
implementation decisions and tutorial material belong outside README.
The introduction spells out BDD (Binary Decision Diagrams), CDCL
(Conflict-Driven Clause Learning) and DNN (Deep Neural Networks).


## First-completion and static dependency execution (2026-11-02)

Added two independent CPU task abstractions for the BDD/CDCL/DNN
use-case family:
- `parallel_first_result(pool,candidates,fn,max_lanes,external_stop)`
  accepts a callback `fn(index,stop_token) -> optional<T>`. A
  shared atomic counter distributes candidates to at most
  `pool.num_threads()+1` lanes, including the caller; the first
  successful result *published* under a mutex wins. Other lanes
  receive a cooperative stop request. An external stop request
  propagates to the internal source. All lanes complete before
  return, even when callbacks throw; callback exceptions propagate.
  This is a CDCL-style **portfolio coordination primitive**, not
  clause learning, a SAT/SMT engine, or a theorem about the
  consistency of two solver results. Candidate functions must
  be thread-safe and stop-aware.
- `TaskGraph` stores void actions and dependency edges and validates
  references, duplicate edges, self-dependencies, and cycles.
  `run(pool,options)` computes a full topological layering
  *before* executing any action; tasks within a layer execute through
  bounded `parallel_for` lanes, with a barrier before the next layer.
  This is appropriate as a basic DNN CPU operations DAG and as a
  testable foundation, not a high-performance dynamic ready queue.
  On exceptions a layer drains then later layers are skipped.
  Graph edits/runs must be externally serialized; tasks may not
  mutate the graph while it executes. Shared tensors and data races
  are the caller's responsibility.

The public umbrella exposes both headers; new `test_advanced`
exercises zero/one/multiple workers, first-success and no-success,
external stop, exception draining, DAG ordering, cycle rejection,
invalid dependencies, and graph reuse. No scheduler policy changed.

Limitations: `parallel_first_result` may require additional semantics
for solver-specific errors or proof validation; a stop cannot interrupt
a noncooperative solver. `TaskGraph` currently does not overlap
layers or dynamically schedule nodes as soon as their predecessors
finish. Before optimizing either path, collect benchmark evidence
from actual CDCL and DNN components. README remains deliberately
restricted to introduction, features and build.


## Scoped per-task rounding support (2026-11-02)

`ScopedRoundingMode` is a C++20 RAII wrapper that queries the current
thread's active floating-point rounding mode, switches to an explicitly
requested mode, and restores it on leaving the scope (including via
exceptions). The `ParallelOptions::rounding_mode` optional integer,
whose values should be standard `FE_TONEAREST`, `FE_DOWNWARD`,
`FE_UPWARD`, or `FE_TOWARDZERO`, applies a separate scope on
every execution lane in `parallel_for` and `parallel_reduce`.
The latter also scopes the sequential combination of partials,
which runs on the calling thread. The default remains unchanged.

**Limits and numerical rigor:** This controls only the current
thread's floating-point environment; pool worker state is restored
after each execution. Correct directed rounding additionally requires
the numerical backend and compiler options to honor dynamic rounding
and to prohibit reassociation/fast-math transformations. A correctly
rounded *individual operation* does not make a changed parallel
reduction tree reproduce the same bits, and interval inclusion
requires correct outward rounding for all relevant operations.
The API makes no claim to compute a correctly rounded *whole*
reduction or exact reproducible accumulator. Non-default rounding
support on a platform is checked via `fesetround`; unsupported modes
fail with an exception.

Regression checks exercise the mode inside worker/caller task bodies
and reduction combiners on 0/1/4 worker pools, plus caller restoration.
They do not substitute for numerical validation of an interval
arithmetic library, strict-mode compiler flags, or proof obligations.


## Dynamic ready-node DAG dispatch (2026-11-02)

Replaced the first `TaskGraph::run()` global topological level
barriers with dependency-driven scheduling. Graph construction is
unchanged. Before executing actions, a Kahn pass validates the whole
graph (a cycle fails without running any node). During execution,
the ready queue, remaining predecessor counters, completed count
and first failure are coordinated by a mutex/condition variable.
A bounded number of CPU lanes (including the caller) take ready
nodes, run outside the lock, and publish each newly unblocked
successor. Independent nodes need not wait for unrelated slow
nodes at the same topological depth.

On the first observed task exception, no additional ready nodes
are dispatched, active tasks finish, and the error is rethrown
only after all lanes have joined. Each action is invoked at most
once per successful run. `ParallelOptions::rounding_mode` is also
scoped on every graph node so numerical kernels run under an
explicit thread-local rounding policy when requested.

Limits: this is a shared-lock ready queue, not lock-free or a
high-throughput accelerator runtime. Lane count is fixed per run;
nested graphs, tasks that block waiting for other tasks on the
same bounded pool, or externally held application locks may
deadlock. Task callbacks must not mutate the graph during run.
A regression test checks that an independent slow root does not
block a dependent child of a different ready root.


## Correction: parallel_reduce lane rounding not installed (2026-11-02)

The first scoped-rounding implementation (`b9d81e3`) contained an
RTL-independent **library bug**: `parallel_for` installed
`ScopedRoundingMode(options.rounding_mode)` in each lane, and
`parallel_reduce` installed it for the final serial combination,
but the latter **omitted the scope in its per-lane map/reduce loop**.
The defect was confirmed by macOS `test_parallel`
`rounding_mode_is_scoped_per_lane` (test 5/7) failing its
`reduction_checks.load()>=128u` assertion. The reported
`pool.queue_size()==0` was the last successful check and was not
the failed assertion.

Fix: install `ScopedRoundingMode` at the beginning of each
`parallel_reduce` lane, preserving the existing final-combination
scope. Strengthen the test to count the expected `FE_UPWARD`
observations separately: exactly 128 maps and 128+(lanes-1)
reductions, including the serial final combination, with 0/1/4
pool workers. Also test restoration after a map exception.
Do **not** weaken or delete the test to make the suite pass.

Strict compiler flags and a conforming arithmetic backend remain
necessary for mathematical guarantees; this fix establishes the
documented thread-local rounding scope, **not** a correctly
rounded whole reduction or bitwise worker-count independence.


## Fixed-block reductions and cooperative DAG cancellation (2026-11-02)

The reduction implementation now offers a distinct algorithm,
\`parallel_reduce_fixed_blocks\`, whose arithmetic grouping is independent
of worker count and per-call \`max_tasks\`. A fixed \`grain_size\` partitions
the input into contiguous blocks. Each block reduces in index order under
its own scoped rounding environment; partials are combined serially in
block-index order under the same explicitly configured rounding mode.
The number of concurrently executing *lanes* may vary without altering
the block boundaries or combining order. This uses O(number of blocks)
memory and is not intended for ultra-fine-grained elements.

**Numerical contract:** fixed grouping is weaker than a correctly
rounded reduction, exact accumulation, or bitwise agreement between
architectures/compilers. Bitwise repeatability on the same platform
assumes pure map/reduce operations, identical input, a compatible
rounding environment, and compiler/FP backend compliance with strict
floating-point semantics. Arbitrary parallel \`parallel_reduce\` remains
available when worker-dependent grouping is acceptable. This option is
useful for DNN experiments and scientific workloads where comparing
results across worker counts matters, without forcing a numeric policy
on the rest of the threading system.

\`TaskGraph::run(pool,options,stop_token)\` now accepts an optional external
cancellation token. Once stop is requested, waiting executor lanes wake
and no further graph nodes are dispatched. Currently executing actions
are **not** forcibly interrupted, and run returns only after they have
joined. A subsequent run with a fresh token is supported. Graph
validation still occurs before executing application callbacks, even
when cancellation is pre-requested. To avoid lost wake-ups, the stop
callback synchronizes with the same mutex used for the ready queue.
This feature is a scheduling-level stop, not cancellation of a DNN
kernel that has already begun.

Tests cover 0/1/4 worker counts, fixed-block floating-point bit
patterns across concurrency limits, map/reduce rounding scopes,
exceptions and restoration, pre-requested stop and stop requested
from an executing graph node. No performance claims are inferred.
