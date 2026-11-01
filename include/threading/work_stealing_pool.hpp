/***************************************************************************
 *            work_stealing_pool.hpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/

/*
 *  This file is part of Threading.
 *
 *  Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#ifndef THREADING_WORK_STEALING_POOL_HPP
#define THREADING_WORK_STEALING_POOL_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <list>
#include <unordered_map>
#include <iterator>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "threading/thread.hpp"

namespace Ariadne {

//! \brief Fixed-size worker pool with local queues and cooperative work stealing.
//! \details External submissions enter a shared FIFO. A task running on this
//! pool submits its descendants to a worker-local deque: its owner takes the
//! newest task (LIFO), while idle workers steal the oldest task (FIFO).
//! TaskGroup<TaskScheduler> can use this scheduler without any change.
//!
//! The pool is non-copyable, has an unbounded queue and does not cancel tasks.
//! It drains outstanding tasks on destruction, including recursively submitted
//! children. Destruction must not race with external submissions, and must not
//! occur from a task executing on this pool. It provides no fairness guarantee.
//! With zero workers, tasks run only through try_run_one_task()/helping waits
//! or on the destroying thread when the pool drains its queue.
//! Logger registration uses the process-wide ThreadManager registry.
class WorkStealingPool {
  public:
    explicit WorkStealingPool(std::size_t workers);
    WorkStealingPool(WorkStealingPool const&) = delete;
    WorkStealingPool& operator=(WorkStealingPool const&) = delete;
    WorkStealingPool(WorkStealingPool&&) = delete;
    WorkStealingPool& operator=(WorkStealingPool&&) = delete;
    ~WorkStealingPool() noexcept;

    //! \brief Submit a task; its future carries the value or captured exception.
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F,Args...>> {
        using Result = std::invoke_result_t<F,Args...>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::bind(std::forward<F>(f),std::forward<Args>(args)...));
        auto future = task->get_future();
        _enqueue([task] { (*task)(); });
        return future;
    }

    //! \brief Enqueue a tracked task and return its queue ticket.
    //! \details TaskGroup uses the ticket to take its own child from the
    //! actual queue rather than executing a duplicate queue wrapper.
    std::size_t enqueue_ticket(VoidFunction task);

    //! \brief Extract and execute a specific queued task when still pending.
    //! \details Hash lookup finds its original queue and stable iterator;
    //! the queue mutex protects against concurrent worker removal.
    //! \return False if another worker or helper has already taken it.
    bool try_run_task(std::size_t ticket);

    //! \brief Help execute one queued task on the calling thread.
    //! \details If called by a worker, attempts its own local work first,
    //! then global work, then steals from other workers. A non-worker
    //! caller takes global work first and can also steal from local queues.
    //! \return True if a task was executed.
    bool try_run_one_task();

    //! \brief Fixed number of dedicated workers (zero is permitted).
    std::size_t num_threads() const noexcept;

    //! \brief Snapshot of tasks still queued, excluding running tasks.
    std::size_t queue_size() const noexcept;

  private:
    struct QueuedTask {
        std::size_t ticket;
        VoidFunction function;
    };
    using TaskQueue=std::list<QueuedTask>;
    struct LocalQueue {
        std::mutex mutex;
        TaskQueue tasks;
    };
    struct TicketLocation {
        std::size_t queue_index; // no_worker denotes global queue.
        TaskQueue::iterator iterator;
    };
    struct WorkerContext {
        WorkStealingPool* pool;
        std::size_t index;
    };
    static thread_local WorkerContext _current;
    static constexpr std::size_t no_worker = static_cast<std::size_t>(-1);

    std::size_t _enqueue(VoidFunction task, bool tracked = false);
    // Caller must hold the corresponding queue mutex.
    void _extract(TaskQueue& queue, TaskQueue::iterator it, VoidFunction& task);
    bool _take(VoidFunction& task);
    bool _take_local(std::size_t index, VoidFunction& task, bool steal);
    bool _take_global(VoidFunction& task);
    void _execute(VoidFunction task);
    void _worker_loop(std::size_t index);

    std::vector<std::unique_ptr<LocalQueue>> _locals;
    TaskQueue _global;
    std::mutex _ticket_mutex;
    std::unordered_map<std::size_t,TicketLocation> _tickets;
    // Queue mutex must precede ticket mutex when both are acquired.
    std::mutex _global_mutex;
    std::vector<std::shared_ptr<Thread>> _workers;

    std::mutex _state_mutex;
    std::condition_variable _availability;
    std::atomic<std::size_t> _pending{0};
    std::size_t _next_ticket{1}; // Guarded by _state_mutex.
    std::size_t _outstanding{0}; // Guarded by _state_mutex.
    bool _stopping{false};       // Guarded by _state_mutex.
};

} // namespace Ariadne

#endif // THREADING_WORK_STEALING_POOL_HPP
