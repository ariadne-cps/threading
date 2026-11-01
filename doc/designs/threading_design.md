# Threading: structured concurrency and work stealing

## Scope

These are generic C++20 task-execution primitives, independent of any particular application.

## Execution facilities

- Thread: a managed single-shot thread.
- BufferedThread: a single worker with a bounded queue.
- ThreadPool: a resizable FIFO pool with optional one-task helping.
- ThreadManager: process-wide concurrency control; concurrency zero executes synchronously.
- StaticWorkload and DynamicWorkload: sequential or worker-based processing, including dynamic task generation.
- WorkStealingPool: fixed-size scheduler with local queues and cooperative task stealing.

## ThreadPool cooperative-helping order

The resizable `ThreadPool` has a mutex-protected deque. Its dedicated
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
#include "threading/work_stealing_pool.hpp"
#include <functional>

Ariadne::WorkStealingPool scheduler(4);
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

## WorkStealingPool scheduling semantics

Include threading/work_stealing_pool.hpp and construct WorkStealingPool(number_of_workers); worker count remains fixed and may be zero. enqueue(fn,args...) returns std::future<Result>, which carries task exceptions. External callers submit to a global FIFO. Tasks already running on a pool worker submit their descendants to that worker's local deque. The owner takes newest tasks from the back (LIFO); idle workers take oldest tasks from other deques (FIFO stealing). All queue operations are mutex-protected; there is no lock-free guarantee.

try_run_one_task() executes at most one queued task in the calling thread. A worker tries its own local queue, then global queue, then other workers' queues. External helpers try global work and steal from workers. A helping wait may execute arbitrary nested task bodies. Application locks must not be held across helping waits if those tasks might need the same locks.

queue_size() is an instantaneous count of queued tasks, not currently running tasks. num_threads() reports the dedicated worker count. No guarantees are made about task completion order, affinity, fairness or speedup.

## Shutdown and lifetime

The destructor rejects new external submissions, permits already executing tasks to enqueue descendants, and drains outstanding tasks before joining dedicated workers. A zero-worker pool drains its queue on the destroying thread. Tasks must eventually terminate. Do not destroy the scheduler from one of its own tasks. Destruction must not overlap external submissions or other API calls.

The outstanding-task count includes queued and running tasks; the pending-task count covers only queued work. Separate mutexes protect local queues, the global injection queue and shutdown state. Worker-local scheduling context is temporarily set while executing a task, then restored after nested helping.

TaskGroup destructor cannot cancel stalled user tasks; it always joins. With zero workers, submission alone does not start execution: use a helping wait, try_run_one_task(), or let the pool destructor drain the tasks.

## Verification and current limitations

tests/test_task_group.cpp validates helping and structured joins. tests/test_work_stealing_pool.cpp checks external submission, nested recursion with one worker, value propagation, cooperative stealing, exception draining and safe shutdown, including zero-worker descendants. Tests use the Ariadne Testing suite macros.

Work stealing here is based on mutex-protected deques and scans, not a lock-free algorithm. Benchmarking and scheduler fairness improvements are separate future optimizations; the initial priority is correct task conservation, exception handling and termination.

## Reproducible performance comparison

`benchmarks/benchmark_thread_pool.cpp` compares the resizable FIFO
`ThreadPool` with the fixed-size `WorkStealingPool` under identical loads.
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