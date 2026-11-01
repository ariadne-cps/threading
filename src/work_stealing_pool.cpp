/***************************************************************************
 *            work_stealing_pool.cpp
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

#include "threading/work_stealing_pool.hpp"
#include "threading/thread_manager.hpp"

#include <string>
#include <algorithm>
#include <iterator>

namespace Ariadne {

thread_local WorkStealingPool::WorkerContext WorkStealingPool::_current{nullptr,no_worker};

WorkStealingPool::WorkStealingPool(std::size_t workers) {
    _locals.reserve(workers);
    for (std::size_t i=0; i<workers; ++i) {
        _locals.push_back(std::make_unique<LocalQueue>());
    }
    _workers.reserve(workers);
    // Thread objects register with Logger; it needs a registry before startup.
    (void)ThreadManager::instance();
    try {
        for (std::size_t i=0; i<workers; ++i) {
            _workers.push_back(std::make_shared<Thread>(
                VoidFunction([this,i] { _worker_loop(i); }),
                "steal" + std::to_string(i)));
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            _stopping = true;
        }
        _availability.notify_all();
        _workers.clear();
        throw;
    }
}

WorkStealingPool::~WorkStealingPool() noexcept {
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _stopping = true;
    }
    _availability.notify_all();
    if (_workers.empty()) {
        // No worker exists to drain queued work. Helping is still possible.
        while (try_run_one_task()) { }
    } else {
        _workers.clear(); // Thread destruction joins all dedicated workers.
    }
}

std::size_t WorkStealingPool::num_threads() const noexcept {
    return _locals.size();
}

std::size_t WorkStealingPool::queue_size() const noexcept {
    return _pending.load(std::memory_order_acquire);
}

std::size_t WorkStealingPool::_enqueue(VoidFunction task, bool tracked) {
    std::size_t ticket=0;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        // Descendants of an already running task are allowed during drain.
        if (_stopping && _current.pool != this) {
            throw std::runtime_error("WorkStealingPool is stopping");
        }
        if (tracked) ticket=_next_ticket++;
        QueuedTask entry{ticket,std::move(task)};
        // Consistent order: state -> queue -> ticket index. Dequeue takes
        // queue -> ticket index; targeted lookup releases the index mutex
        // before acquiring any queue mutex.
        auto insert=[&](TaskQueue& queue,std::mutex& queue_mutex,std::size_t index) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            auto it=queue.insert(queue.end(),std::move(entry));
            if (tracked) {
                try {
                    std::lock_guard<std::mutex> index_lock(_ticket_mutex);
                    _tickets.emplace(ticket,TicketLocation{index,it});
                } catch (...) {
                    queue.erase(it);
                    throw;
                }
            }
        };
        if (_current.pool == this && _current.index < _locals.size()) {
            auto& local=*_locals[_current.index];
            insert(local.tasks,local.mutex,_current.index);
        } else {
            insert(_global,_global_mutex,no_worker);
        }
        ++_outstanding;
        _pending.fetch_add(1,std::memory_order_release);
    }
    // All idle workers may need to steal from the newly populated local queue.
    _availability.notify_all();
    return ticket;
}

std::size_t WorkStealingPool::enqueue_ticket(VoidFunction task) {
    return _enqueue(std::move(task),true);
}

void WorkStealingPool::_extract(TaskQueue& queue,
                                 TaskQueue::iterator it, VoidFunction& task) {
    // This helper is called with the queue lock held. Ordinary enqueues
    // have ticket zero and do not touch the shared ticket index.
    task=std::move(it->function);
    if (it->ticket!=0) {
        std::lock_guard<std::mutex> lock(_ticket_mutex);
        _tickets.erase(it->ticket);
    }
    queue.erase(it);
    _pending.fetch_sub(1,std::memory_order_acq_rel);
}

bool WorkStealingPool::try_run_task(std::size_t ticket) {
    if (ticket==0) return false;
    std::size_t queue_index;
    {
        std::lock_guard<std::mutex> lock(_ticket_mutex);
        auto found=_tickets.find(ticket);
        if (found==_tickets.end()) return false;
        queue_index=found->second.queue_index;
    }
    // Never acquire a queue mutex while holding the index mutex.
    // Another executor may have removed the task since the first lookup,
    // so recheck after locking its original queue.
    VoidFunction task;
    auto acquire=[&](TaskQueue& queue,std::mutex& queue_mutex) -> bool {
        std::lock_guard<std::mutex> lock(queue_mutex);
        std::lock_guard<std::mutex> index_lock(_ticket_mutex);
        const auto found=_tickets.find(ticket);
        if (found==_tickets.end()) return false;
        // Iterator is valid because the queue mutex prevents erase.
        const auto it=found->second.iterator;
        task=std::move(it->function);
        queue.erase(it);
        _tickets.erase(found);
        _pending.fetch_sub(1,std::memory_order_acq_rel);
        return true;
    };
    bool acquired=false;
    if (queue_index==no_worker) {
        acquired=acquire(_global,_global_mutex);
    } else {
        acquired=acquire(_locals[queue_index]->tasks,_locals[queue_index]->mutex);
    }
    if (not acquired) return false;
    _execute(std::move(task)); // No mutex is held while invoking task code.
    return true;
}

bool WorkStealingPool::_take_global(VoidFunction& task) {
    std::lock_guard<std::mutex> lock(_global_mutex);
    if (_global.empty()) return false;
    _extract(_global,_global.begin(),task);
    return true;
}

bool WorkStealingPool::_take_local(std::size_t index, VoidFunction& task, bool steal) {
    auto& queue = *_locals[index];
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.tasks.empty()) return false;
    _extract(queue.tasks,steal ? queue.tasks.begin() : std::prev(queue.tasks.end()),task);
    return true;
}

bool WorkStealingPool::_take(VoidFunction& task) {
    std::size_t own = _current.pool == this ? _current.index : no_worker;
    if (own < _locals.size() && _take_local(own,task,false)) return true;
    if (_take_global(task)) return true;
    // External helpers can also steal; all deque operations are synchronized.
    for (std::size_t offset=0; offset<_locals.size(); ++offset) {
        std::size_t victim = own < _locals.size()
            ? (own+1+offset)%_locals.size() : offset;
        if (victim != own && _take_local(victim,task,true)) return true;
    }
    return false;
}

void WorkStealingPool::_execute(VoidFunction task) {
    WorkerContext previous = _current;
    // External helping does not claim a worker-local queue.
    std::size_t index = previous.pool == this ? previous.index : no_worker;
    _current = {this,index};
    // Tasks submitted through enqueue are packaged_tasks: user exceptions
    // are stored in the future, not allowed to escape the worker loop.
    try { task(); }
    catch (...) { }
    _current = previous;
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        --_outstanding;
    }
    _availability.notify_all();
}

bool WorkStealingPool::try_run_one_task() {
    VoidFunction task;
    if (not _take(task)) return false;
    _execute(std::move(task));
    return true;
}

void WorkStealingPool::_worker_loop(std::size_t index) {
    WorkerContext previous = _current;
    _current = {this,index};
    while (true) {
        if (try_run_one_task()) continue;
        std::unique_lock<std::mutex> lock(_state_mutex);
        _availability.wait(lock,[this] {
            return _pending.load(std::memory_order_acquire) != 0
                || (_stopping && _outstanding == 0);
        });
        if (_stopping && _outstanding == 0) break;
    }
    _current = previous;
}

} // namespace Ariadne
