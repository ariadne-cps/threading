/***************************************************************************
 *            thread_pool.cpp
 *
 *  Copyright  2022  Luca Geretti
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
 *  along with Threading.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "threading/thread_pool.hpp"

#include <iterator>
#include <sstream>
#include <stdexcept>

namespace Ariadne {

thread_local ThreadPool::WorkerContext ThreadPool::_current{nullptr,no_worker};

String construct_thread_name(String prefix,size_t number,size_t max_number) {
    std::ostringstream ss;
    ss << prefix;
    if(max_number>9 && number<=9)ss << "0";
    ss << number;
    return ss.str();
}

ThreadPool::ThreadPool(size_t workers,String name,SchedulingPolicy policy)
    : _name(std::move(name)),_policy(policy) {
    set_num_threads(workers);
}

ThreadPool::~ThreadPool() noexcept {
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _stopping=true;
    }
    _availability.notify_all();
    if(_workers.empty()) {
        // Tasks may submit further tasks while the zero-worker pool drains.
        while(try_run_one_task()) { }
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
    if(_current.pool==this)
        throw std::logic_error("cannot resize a pool from one of its tasks");
    std::lock_guard<std::mutex> resize_lock(_resize_mutex);
    const size_t old=_workers.size();
    if(number==old)return;

    if(number<old) {
        // Historical ThreadPool shrink is a synchronization barrier:
        // drain existing queued and running tasks before retiring threads.
        {
            std::unique_lock<std::mutex> lock(_state_mutex);
            _availability.wait(lock,[this] { return _outstanding==0; });
            _desired_workers.store(number,std::memory_order_release);
        }
        _availability.notify_all();
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
            for(size_t i=_locals.size();i<number;++i)
                _locals.push_back(std::make_unique<LocalQueue>());
        }
        _workers.reserve(number);
        _desired_workers.store(number,std::memory_order_release);
        try {
            for(size_t i=old;i<number;++i) {
                _workers.push_back(std::make_shared<Thread>(
                    VoidFunction([this,i] { _worker_loop(i); }),
                    construct_thread_name(_name,i,number)));
            }
        } catch (...) {
            _desired_workers.store(old,std::memory_order_release);
            _availability.notify_all();
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

ThreadPool::Ticket ThreadPool::_enqueue_task(VoidFunction task,bool tracked) {
    Ticket ticket=tracked ? std::make_shared<TaskTicket>() : Ticket{};
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        // Already-running tasks may produce descendants during shutdown.
        if(_stopping && _current.pool!=this)
            throw StoppedThreadPoolException{};

        auto insert=[&](TaskQueue& queue,std::mutex& queue_mutex) {
            std::lock_guard<std::mutex> lock(queue_mutex);
            const auto it=queue.insert(queue.end(),QueuedTask{ticket,std::move(task)});
            if(ticket) {
                ticket->owner=this;
                ticket->queue=&queue;
                ticket->queue_mutex=&queue_mutex;
                ticket->position=it;
                ticket->pending=true;
            }
        };
        if(_policy==SchedulingPolicy::WorkStealing && _current.pool==this) {
            std::shared_lock<std::shared_mutex> topology_lock(_topology_mutex);
            if(_current.index<_desired_workers.load(std::memory_order_acquire)) {
                auto& local=*_locals[_current.index];
                insert(local.tasks,local.mutex);
            } else {
                insert(_global,_global_mutex);
            }
        } else {
            insert(_global,_global_mutex);
        }
        ++_outstanding;
        _pending.fetch_add(1,std::memory_order_release);
    }
    _availability.notify_all();
    return ticket;
}

ThreadPool::Ticket ThreadPool::enqueue_ticket(VoidFunction task) {
    return _enqueue_task(std::move(task),true);
}

void ThreadPool::_extract(TaskQueue& queue,TaskQueue::iterator it,VoidFunction& task) {
    task=std::move(it->function);
    if(it->ticket)it->ticket->pending=false;
    queue.erase(it);
    _pending.fetch_sub(1,std::memory_order_acq_rel);
}

bool ThreadPool::try_run_task(Ticket const& ticket) {
    if(!ticket || ticket->owner!=this)return false;
    VoidFunction task;
    {
        std::lock_guard<std::mutex> lock(*ticket->queue_mutex);
        if(!ticket->pending)return false;
        _extract(*ticket->queue,ticket->position,task);
    }
    _execute(std::move(task));
    return true;
}

bool ThreadPool::_take_global(VoidFunction& task,bool fifo) {
    std::lock_guard<std::mutex> lock(_global_mutex);
    if(_global.empty())return false;
    const auto it=fifo ? _global.begin() : std::prev(_global.end());
    _extract(_global,it,task);
    return true;
}

bool ThreadPool::_take_local(size_t index,VoidFunction& task,bool steal) {
    auto& local=*_locals[index];
    std::lock_guard<std::mutex> lock(local.mutex);
    if(local.tasks.empty())return false;
    const auto it=steal ? local.tasks.begin() : std::prev(local.tasks.end());
    _extract(local.tasks,it,task);
    return true;
}

bool ThreadPool::_take(VoidFunction& task,bool worker_dispatch) {
    if(_policy==SchedulingPolicy::FIFO)
        return _take_global(task,worker_dispatch);
    std::shared_lock<std::shared_mutex> topology_lock(_topology_mutex);
    const size_t own=_current.pool==this ? _current.index : no_worker;
    const size_t active=_desired_workers.load(std::memory_order_acquire);
    if(own<active && _take_local(own,task,false))return true;
    if(_take_global(task,true))return true;
    for(size_t offset=0;offset<active;++offset) {
        const size_t victim=own<active
            ? (own+1+offset)%active : offset;
        if(victim!=own && _take_local(victim,task,true))return true;
    }
    return false;
}

void ThreadPool::_execute(VoidFunction task) {
    WorkerContext previous=_current;
    const size_t index=previous.pool==this ? previous.index : no_worker;
    _current={this,index};
    // enqueue and TaskGroup futures own user exceptions.
    try { task(); }
    catch (...) { }
    _current=previous;
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        --_outstanding;
    }
    _availability.notify_all();
}

bool ThreadPool::try_run_one_task() {
    VoidFunction task;
    if(!_take(task,false))return false;
    _execute(std::move(task));
    return true;
}

void ThreadPool::_worker_loop(size_t index) {
    const WorkerContext previous=_current;
    _current={this,index};
    while(index<_desired_workers.load(std::memory_order_acquire)) {
        VoidFunction task;
        if(_take(task,true)) {
            _execute(std::move(task));
            continue;
        }
        std::unique_lock<std::mutex> lock(_state_mutex);
        _availability.wait(lock,[this,index] {
            return index>=_desired_workers.load(std::memory_order_acquire)
                || _pending.load(std::memory_order_acquire)!=0
                || (_stopping && _outstanding==0);
        });
        if((_stopping && _outstanding==0)
           || index>=_desired_workers.load(std::memory_order_acquire))
            break;
    }
    _current=previous;
}

} // namespace Ariadne
