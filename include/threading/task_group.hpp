/***************************************************************************
 *            task_group.hpp
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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Threading. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef THREADING_TASK_GROUP_HPP
#define THREADING_TASK_GROUP_HPP

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <future>
#include <type_traits>
#include <utility>
#include <vector>

#include "threading/thread_manager.hpp"
#include "threading/thread_pool.hpp"

namespace Ariadne {

//! \brief Future-like result of a forked task, with cooperative waiting.
//! \details A handle produced by TaskGroup may extract its own pending task
//! from its scheduler's queue. Another worker can win the same dequeue race;
//! in that case, the handle waits for that worker to finish the exact task.
//! A handle built directly from a future retains generic scheduler helping.
//! For non-void results, get() returns a const reference valid while the
//! handle (or another owner of its shared future) is alive.
//! The scheduler must outlive every handle and task group referring to it.
template<class Scheduler, class Result>
class TaskHandle {
  public:
    TaskHandle(Scheduler& scheduler, std::shared_future<Result> future,
               std::function<bool()> try_execute = {})
        : _scheduler(&scheduler), _future(std::move(future)),
          _try_execute(std::move(try_execute)) { }

    //! \brief Wait for completion, helping the scheduler if work is pending.
    void wait() const {
        using namespace std::chrono_literals;
        if (_try_execute) {
            // Remove and execute this exact dependency from the scheduler.
            // A worker can win the dequeue race; then wait for its result.
            // No redundant task wrapper remains in either scheduler's queue.
            if (_future.wait_for(0ms) != std::future_status::ready)
                (void)_try_execute();
            _future.wait();
            return;
        }
        // Backward-compatible generic handling for externally built handles.
        while (_future.wait_for(0ms) != std::future_status::ready) {
            if (not _scheduler->try_run_one_task())
                _future.wait_for(1ms);
        }
    }

    //! \brief Obtain the result or rethrow the task exception.
    decltype(auto) get() const {
        wait();
        return _future.get();
    }

  private:
    Scheduler* _scheduler;
    std::shared_future<Result> _future;
    std::function<bool()> _try_execute;
};

//! \brief Group of forked tasks, joined even when the caller leaves by exception.
//! \details Forks are registered by the owning thread; concurrent fork/join
//! calls on a single group are not supported. Each nested recursive task can
//! create its own group. join() waits for *all* tasks before propagating the
//! first task exception, in fork order. The destructor also joins all tasks
//! but cannot throw. The scheduler must outlive the group.
template<class Scheduler>
class TaskGroup {
  public:
    explicit TaskGroup(Scheduler& scheduler) noexcept : _scheduler(scheduler) { }
    TaskGroup(TaskGroup const&) = delete;
    TaskGroup& operator=(TaskGroup const&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;

    ~TaskGroup() noexcept {
        try { join(); }
        catch (...) { } // Explicit join() is required to observe exceptions.
    }

    //! \brief Fork a callable onto the scheduler, returning a result handle.
    template<class Callable>
    auto fork(Callable&& callable) {
        using Result = std::invoke_result_t<std::decay_t<Callable>&>;
        auto task = std::make_shared<std::packaged_task<Result()>>(
            std::forward<Callable>(callable));
        auto future = task->get_future().share();
        // Ticket-based acquisition ensures that both a waiting handle and
        // a worker use the scheduler's single physical queue entry.
        const auto ticket=_scheduler.enqueue_ticket([task] { (*task)(); });
        auto try_execute=[scheduler=&_scheduler,ticket] {
            return scheduler->try_run_task(ticket);
        };
        TaskHandle<Scheduler, Result> handle(_scheduler,std::move(future),try_execute);
        try {
            _joiners.emplace_back([handle] { static_cast<void>(handle.get()); });
        } catch (...) {
            // A launched child must never outlive the owning scope on error.
            handle.wait();
            throw;
        }
        return handle;
    }

    //! \brief Join all forked tasks, helping while waiting for their results.
    //! \details All tasks finish before the first error is rethrown.
    void join() {
        std::exception_ptr first_error;
        for (auto& joiner : _joiners) {
            try { joiner(); }
            catch (...) {
                if (not first_error) first_error = std::current_exception();
            }
        }
        _joiners.clear();
        if (first_error) std::rethrow_exception(first_error);
    }

  private:
    Scheduler& _scheduler;
    std::vector<std::function<void()>> _joiners;
};

} // namespace Ariadne

#endif // THREADING_TASK_GROUP_HPP
