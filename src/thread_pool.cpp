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

namespace Ariadne {

String construct_thread_name(String prefix, size_t number, size_t max_number) {
    std::ostringstream ss;
    ss << prefix;
    if (max_number > 9 and number <= 9) ss << "0";
    ss << number;
    return ss.str();
}

void ThreadPool::_enqueue_task(VoidFunction task) {
    {
        lock_guard<mutex> lock(_task_availability_mutex);
        _tasks.emplace(std::move(task));
    }
    _task_availability_condition.notify_one();
}

VoidFunction ThreadPool::_task_wrapper_function(size_t i) {
    return [i, this] {
        while (true) {
            VoidFunction task;
            {
                unique_lock<mutex> lock(_task_availability_mutex);
                _task_availability_condition.wait(lock, [=, this] {
                    return _finish_all_and_stop or i>=_num_threads_to_use or not _tasks.empty();
                });
                if (_finish_all_and_stop and _tasks.empty()) return;
                if (i>=_num_threads_to_use) {
                    _num_active_threads--;
                    if (_num_active_threads == _num_threads_to_use) _all_unused_threads_stopped_promise.set_value();
                    return;
                }
                task = std::move(_tasks.front());
                _tasks.pop();
                lock.unlock();
                task();
            }
        }
    };
}

void ThreadPool::_append_thread_range(size_t lower, size_t upper) {
    for (size_t i=lower; i<upper; ++i) {
        _threads.push_back(make_shared<Thread>(ThreadPool::_task_wrapper_function(i), construct_thread_name(_name,i,upper)));
    }
}

ThreadPool::ThreadPool(size_t size, String name)
        : _name(name), _finish_all_and_stop(false), _num_active_threads(size), _num_threads_to_use(size),
          _all_unused_threads_stopped_future(_all_unused_threads_stopped_promise.get_future())
{
    _append_thread_range(0,size);
}

String ThreadPool::name() const {
    return _name;
}

size_t ThreadPool::num_threads() const {
    lock_guard<mutex> lock(_num_threads_mutex);
    return _threads.size();
}

void ThreadPool::set_num_threads(size_t number) {
    lock_guard<mutex> lock(_num_threads_mutex);
    auto old_size = _threads.size();
    if (number < old_size) {
        auto caller_id = std::this_thread::get_id();
        for (auto const& thread : _threads)
            ARIADNE_PRECONDITION(thread->id() != caller_id);
    }
    if (number > old_size) {
        {
            lock_guard<mutex> task_lock(_task_availability_mutex);
            _num_threads_to_use = number;
            _num_active_threads = number;
        }
        _append_thread_range(old_size,number);
    } else if (number < old_size) {
        {
            lock_guard<mutex> task_lock(_task_availability_mutex);
            _num_threads_to_use = number;
        }
        _task_availability_condition.notify_all();
        _all_unused_threads_stopped_future.get();
        _threads.resize(number);
        _all_unused_threads_stopped_promise = promise<void>();
        _all_unused_threads_stopped_future = _all_unused_threads_stopped_promise.get_future();
    }
}

size_t ThreadPool::queue_size() const {
    lock_guard<mutex> lock(_task_availability_mutex);
    return _tasks.size();
}

ThreadPool::~ThreadPool() {
    {
        lock_guard<mutex> task_availability_lock(_task_availability_mutex);
        _finish_all_and_stop = true;
    }
    _task_availability_condition.notify_all();
    _threads.clear();
}

}
