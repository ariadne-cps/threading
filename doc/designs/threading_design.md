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