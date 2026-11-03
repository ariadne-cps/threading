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
#include <variant>

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
          _acquisition(std::in_place_type<std::function<bool()>>,
                       std::move(try_execute)) { }

    //! \brief Direct targeted acquisition for the ThreadPool ticket type.
    //! \details Works with ThreadPool and ThreadManager, including an empty
    //! ticket from the manager's synchronous execution mode.
    TaskHandle(Scheduler& scheduler, std::shared_future<Result> future,
               ThreadPool::Ticket ticket)
        requires requires(Scheduler& s, ThreadPool::Ticket const& t) {
            s.try_run_task(t);
        }
        : _scheduler(&scheduler), _future(std::move(future)),
          _acquisition(std::in_place_type<ThreadPool::Ticket>,
                       std::move(ticket)) { }

    //! \brief Wait for completion, helping the scheduler if work is pending.
    void wait() const {
        using namespace std::chrono_literals;
        if constexpr (requires(Scheduler& s, ThreadPool::Ticket const& t) {
            s.try_run_task(t);
        }) {
            if (auto const* ticket=std::get_if<ThreadPool::Ticket>(&_acquisition)) {
                // The worker and this waiter compete for the same queue entry.
                if (_future.wait_for(0ms) != std::future_status::ready)
                    (void)_scheduler->try_run_task(*ticket);
                _future.wait();
                return;
            }
        }
        if (auto const& try_execute =
                std::get<std::function<bool()>>(_acquisition); try_execute) {
            if (_future.wait_for(0ms) != std::future_status::ready)
                (void)try_execute();
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
    // Store one representation, not both: a pool ticket or a generic
    // type-erased callback, preserving the legacy TaskHandle constructor.
    std::variant<ThreadPool::Ticket,std::function<bool()>> _acquisition;
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
        // Once queued, any failure in the handle or joiner construction
        // must still wait for this child. Keeping a shared future alive also
        // covers a failure before the handle has been fully constructed.
        try {
            // The callback representation retains targeted, exact-once helping.
            // In one-worker tests it lets the caller acquire more short tasks
            // than the direct-ticket representation, avoiding costly handoffs.
            // Keep direct ticket handles available for explicit use and A/B tests.
            auto try_execute=[scheduler=&_scheduler,ticket] {
                return scheduler->try_run_task(ticket);
            };
            TaskHandle<Scheduler,Result> handle(_scheduler,future,try_execute);
            // The common recursive fork/compute/join pattern owns one child.
            // Store its joiner directly; do not allocate a vector buffer.
            if (not _first_joiner)
                _first_joiner=[handle] { static_cast<void>(handle.get()); };
            else
                _joiners.emplace_back([handle] { static_cast<void>(handle.get()); });
            return handle;
        } catch (...) {
            // A launched child must never outlive the owning scope on error.
            (void)_scheduler.try_run_task(ticket);
            future.wait();
            throw;
        }
    }

    //! \brief Join all forked tasks, helping while waiting for their results.
    //! \details All tasks finish before the first error is rethrown.
    void join() {
        std::exception_ptr first_error;
        if (_first_joiner) {
            try { _first_joiner(); }
            catch (...) {
                first_error=std::current_exception();
            }
            _first_joiner=nullptr;
        }
        for (auto& joiner : _joiners) {
            try { joiner(); }
            catch (...) {
                if (not first_error) first_error=std::current_exception();
            }
        }
        _joiners.clear();
        if (first_error) std::rethrow_exception(first_error);
    }

  private:
    Scheduler& _scheduler;
    // One inline joiner avoids the vector allocation in the one-child case.
    // Type-erased joiner storage may still allocate for the captured handle.
    std::function<void()> _first_joiner;
    std::vector<std::function<void()>> _joiners;
};

} // namespace Ariadne

#endif // THREADING_TASK_GROUP_HPP
