/***************************************************************************
 *            thread_pool.cpp
 *
 *  Copyright  2022  Luca Geretti
 *
 ****************************************************************************/

/*
 *  This file is part of Ariadne Threading.
 *
 *  Ariadne Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Ariadne Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Ariadne Threading. If not, see <https://www.gnu.org/licenses/>.
 */

#include "threading/thread_pool.hpp"

#include <iterator>
#include <sstream>
#include <stdexcept>

namespace Ariadne {

thread_local ThreadPool::WorkerContext ThreadPool::_current{nullptr, no_worker, 0u};

namespace {

// Guard only adjusts the per-thread scope level, never process-wide
// verbosity. Nested tasks and cooperative helping restore the previous
// caller/worker context even when a callback throws.
class ScopedTaskLogLevel {
  public:
    explicit ScopedTaskLogLevel(std::optional<unsigned int> inherited)
        : _logger(Logger::instance()), _previous(_logger.try_current_level()) {
        if (inherited && _previous)
            this->_set(*inherited);
    }

    ScopedTaskLogLevel(const ScopedTaskLogLevel&) = delete;
    ScopedTaskLogLevel& operator=(const ScopedTaskLogLevel&) = delete;

    ~ScopedTaskLogLevel() noexcept {
        if (_previous) {
            try {
                this->_set(*_previous);
            } catch (...) {
                // The scheduler must still account for task completion.
            }
        }
    }

  private:
    void _set(unsigned int level) {
        // An unregistered caller or executor has no associated scheduler
        // state. Never dereference a missing registration.
        const auto current = _logger.try_current_level();
        if (!current)
            return;
        if (level > *current)
            _logger.increase_level(level - *current);
        else if (*current > level)
            _logger.decrease_level(*current - level);
    }

    Logger& _logger;
    std::optional<unsigned int> _previous;
};

} // namespace

String construct_thread_name(String prefix, size_t number, size_t max_number) {
    std::ostringstream ss;
    ss << prefix;
    if (max_number > 9 && number <= 9)
        ss << "0";
    ss << number;
    return ss.str();
}

ThreadPool::ThreadPool(size_t workers, String name)
    : _name(std::move(name)) {
    set_num_threads(workers);
}

ThreadPool::~ThreadPool() noexcept {
    _activity.fetch_or(stopping_bit, std::memory_order_acq_rel);
    _epoch.fetch_add(1, std::memory_order_release);
    _epoch.notify_all();
    if (_workers.empty()) {
        // Tasks may submit further tasks while the zero-worker pool drains.
        while (try_run_one_task()) {
        }
    } else {
        _workers.clear(); // Thread destruction joins the worker loops.
    }
}

String ThreadPool::name() const { return _name; }

size_t ThreadPool::num_threads() const {
    std::lock_guard<std::mutex> lock(_resize_mutex);
    return _workers.size();
}

size_t ThreadPool::queue_size() const noexcept {
    return _pending.load(std::memory_order_acquire);
}

void ThreadPool::set_num_threads(size_t number) {
    // A task running on this pool cannot join its own worker.
    if (_current.pool == this)
        throw std::logic_error("cannot resize a pool from one of its tasks");
    std::lock_guard<std::mutex> resize_lock(_resize_mutex);
    const size_t old = _workers.size();
    if (number == old)
        return;

    if (number < old) {
        // Historical ThreadPool shrink is a synchronization barrier:
        // drain existing queued and running tasks before retiring threads.
        while ((_activity.load(std::memory_order_acquire) & ~stopping_bit) != 0u) {
            const size_t stamp = _epoch.load(std::memory_order_acquire);
            if ((_activity.load(std::memory_order_acquire) & ~stopping_bit) != 0u)
                _epoch.wait(stamp, std::memory_order_acquire);
        }
        _desired_workers.store(number, std::memory_order_release);
        _epoch.fetch_add(1, std::memory_order_release);
        _epoch.notify_all();
        _workers.resize(number); // Retiring threads are joined here.
        {
            std::unique_lock<std::shared_mutex> lock(_topology_mutex);
            // Keep retired local queue storage alive: existing ticket
            // handles still point at its mutex, even when not pending.
        }
    } else {
        {
            std::unique_lock<std::shared_mutex> lock(_topology_mutex);
            _locals.reserve(number);
            for (size_t i = _locals.size(); i < number; ++i)
                _locals.push_back(std::make_unique<LocalQueue>());
        }
        _workers.reserve(number);
        _desired_workers.store(number, std::memory_order_release);
        try {
            for (size_t i = old; i < number; ++i) {
                _workers.push_back(std::make_shared<Thread>(
                    VoidFunction([this, i] { _worker_loop(i); }),
                    construct_thread_name(_name, i, number)));
            }
        } catch (...) {
            _desired_workers.store(old, std::memory_order_release);
            _epoch.fetch_add(1, std::memory_order_release);
            _epoch.notify_all();
            _workers.resize(old);
            {
                std::unique_lock<std::shared_mutex> lock(_topology_mutex);
                // Existing and newly allocated queue objects remain
                // stable for any ticket handles that escaped the pool.
            }
            throw;
        }
    }
}

ThreadPool::Ticket ThreadPool::_enqueue_task(VoidFunction task, bool tracked) {
    // Each task carries its submitting thread's log scope depth. Worker
    // registration snapshots only the scope at worker creation and cannot
    // represent later nested submissions. Unregistered external callers
    // contribute no context rather than accessing absent logger state.
    const auto inherited_log_level = Logger::instance().try_current_level();
    Ticket ticket = tracked ? std::make_shared<TaskTicket>() : Ticket{};
    // Reserve activity before making the queue entry visible: destruction
    // must never mistake a submission in progress for an idle scheduler.
    size_t observed = _activity.load(std::memory_order_acquire);
    for (;;) {
        if ((observed & stopping_bit) != 0u && _current.pool != this)
            throw StoppedThreadPoolException{};
        if ((observed & ~stopping_bit) == stopping_bit - 1u)
            throw std::overflow_error("too many outstanding tasks");
        if (_activity.compare_exchange_weak(observed, observed + 1u,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
            break;
    }
    try {
        auto insert = [&](TaskQueue& queue, std::mutex& queue_mutex) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            const auto it = queue.insert(queue.end(), QueuedTask{ticket, std::move(task), inherited_log_level});
            if (ticket) {
                ticket->owner = this;
                ticket->queue = &queue;
                ticket->queue_mutex = &queue_mutex;
                ticket->position = it;
                ticket->pending = true;
            }
            _pending.fetch_add(1, std::memory_order_release);
        };
        if (_current.pool == this) {
            std::shared_lock<std::shared_mutex> topology_lock(_topology_mutex);
            if (_current.index < _desired_workers.load(std::memory_order_acquire)) {
                auto& local = *_locals[_current.index];
                insert(local.tasks, local.mutex);
            } else {
                insert(_global, _global_mutex);
            }
        } else {
            insert(_global, _global_mutex);
        }
    } catch (...) {
        // Release a reservation that never became executable. Only the
        // transition to zero can unblock a concurrent drain/retirement.
        _finish_activity();
        throw;
    }
    _epoch.fetch_add(1, std::memory_order_release);
    _epoch.notify_all();
    return ticket;
}

ThreadPool::Ticket ThreadPool::enqueue_ticket(VoidFunction task) {
    return _enqueue_task(std::move(task), true);
}

void ThreadPool::_extract(TaskQueue& queue, TaskQueue::iterator it, QueuedTask& task) {
    // Mark pending before moving: the moved-from queue entry no longer
    // owns its ticket. A ticket can never claim this task twice.
    if (it->ticket)
        it->ticket->pending = false;
    task = std::move(*it);
    queue.erase(it);
    _pending.fetch_sub(1, std::memory_order_acq_rel);
}

bool ThreadPool::try_run_task(const Ticket& ticket) {
    if (!ticket || ticket->owner != this)
        return false;
    QueuedTask task;
    bool extracted = false;
    {
        std::lock_guard<std::mutex> lock(*ticket->queue_mutex);
        if (ticket->pending) {
            _extract(*ticket->queue, ticket->position, task);
            extracted = true;
        }
    }
    if (extracted)
        _execute(std::move(task));
    return extracted;
}

bool ThreadPool::_take_global(QueuedTask& task) {
    std::lock_guard<std::mutex> lock(_global_mutex);
    if (_global.empty())
        return false;
    _extract(_global, _global.begin(), task);
    return true;
}

bool ThreadPool::_take_local(size_t index, QueuedTask& task, bool steal) {
    auto& local = *_locals[index];
    std::lock_guard<std::mutex> lock(local.mutex);
    if (local.tasks.empty())
        return false;
    const auto it = steal ? local.tasks.begin() : std::prev(local.tasks.end());
    _extract(local.tasks, it, task);
    return true;
}

bool ThreadPool::_take(QueuedTask& task) {
    std::shared_lock<std::shared_mutex> topology_lock(_topology_mutex);
    const size_t own = _current.pool == this ? _current.index : no_worker;
    const size_t active = _desired_workers.load(std::memory_order_acquire);
    // A worker must probe externally injected work after eight local picks.
    // This bounds local dispatch preference, not task execution latency.
    if (own < active && _current.local_picks >= max_local_picks) {
        _current.local_picks = 0u;
        if (_take_global(task))
            return true;
    }
    if (own < active && _take_local(own, task, false)) {
        ++_current.local_picks;
        return true;
    }
    if (own < active)
        _current.local_picks = 0u;
    if (_take_global(task))
        return true;
    for (size_t offset = 0; offset < active; ++offset) {
        const size_t victim = own < active
                                ? (own + 1 + offset) % active
                                : offset;
        if (victim != own && _take_local(victim, task, true))
            return true;
    }
    return false;
}

void ThreadPool::_execute(QueuedTask task) {
    WorkerContext previous = _current;
    const size_t index = previous.pool == this ? previous.index : no_worker;
    _current = {this, index, 0u};

    // A claimed task may run on a worker, a stealing worker or the caller
    // helping a join. Preserve the executor's prior level in every case,
    // including callbacks that throw or change their log scope depth.
    // enqueue and TaskGroup futures own user exceptions.
    try {
        ScopedTaskLogLevel inherited(task.inherited_log_level);
        task.function();
    } catch (...) {
    }
    _current = previous;
    _finish_activity();
}

void ThreadPool::_finish_activity() noexcept {
    const size_t before = _activity.fetch_sub(1, std::memory_order_acq_rel);
    // Running tasks cannot produce further work after this count reaches
    // zero; wake both a draining resize and workers exiting at shutdown.
    // Ordinary completions do not expose new work and need no wake-up.
    if ((before & ~stopping_bit) == 1u) {
        _epoch.fetch_add(1, std::memory_order_release);
        _epoch.notify_all();
    }
}

bool ThreadPool::try_run_one_task() {
    QueuedTask task;
    if (!_take(task))
        return false;
    _execute(std::move(task));
    return true;
}

void ThreadPool::_worker_loop(size_t index) {
    const WorkerContext previous = _current;
    _current = {this, index, 0u};
    for (;;) {
        if (index >= _desired_workers.load(std::memory_order_acquire))
            break;
        QueuedTask task;
        if (_take(task)) {
            _execute(std::move(task));
            continue;
        }
        const size_t stamp = _epoch.load(std::memory_order_acquire);
        if (index >= _desired_workers.load(std::memory_order_acquire))
            break;
        const size_t activity = _activity.load(std::memory_order_acquire);
        if ((activity & stopping_bit) != 0u && (activity & ~stopping_bit) == 0u)
            break;
        if (_pending.load(std::memory_order_acquire) != 0u)
            continue;
        _epoch.wait(stamp, std::memory_order_acquire);
    }
    _current = previous;
}

} // namespace Ariadne
