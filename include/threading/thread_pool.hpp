/***************************************************************************
 *            thread_pool.hpp
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

/*! \file thread_pool.hpp
 *  \brief A pool of threads
 */

#ifndef THREADING_THREAD_POOL_HPP
#define THREADING_THREAD_POOL_HPP

#include <deque>
#include <algorithm>
#include "logging/logging.hpp"
#include "utility/container.hpp"
#include "threading/thread.hpp"
#include "threading/templates.hpp"
#include "threading/using.hpp"

namespace Ariadne {

using std::make_shared;
using Ariadne::List;

const String THREAD_POOL_DEFAULT_NAME = "thr";

//! \brief Exception for stopping a thread pool
class StoppedThreadPoolException : public std::exception { };

//! \brief A pool of Thread objects managed internally given a (variable) number of threads
//! \details Differently from managing a single BufferedThread, the task queue for a pool is not upper-bounded, i.e., BufferedThread
//! objects use a buffer of one element, which receives once the wrapped task that consumes elements from the task queue.
class ThreadPool {
  public:
    //! \brief Construct from a given number of threads and possibly a name
    ThreadPool(size_t num_threads, String name = THREAD_POOL_DEFAULT_NAME);

    //! \brief Enqueue a task for execution, returning the future handler
    //! \details The is no limits on the number of tasks to enqueue
    template<class F, class... AS> auto enqueue(F &&f, AS &&... args) -> future<ResultOf<F(AS...)>>;

    //! \brief The name of the pool
    String name() const;

    //! \brief Run the newest queued task on the calling thread.
    //! \details Dedicated workers retain FIFO dispatch; cooperative helping
    //! uses LIFO so recursive waits favour recently queued descendants instead
    //! of nesting across breadth-first sibling work. The task is extracted
    //! under the queue mutex and executed after releasing the mutex.
    //! \return True iff a pending task was taken and executed.
    bool try_run_one_task();

    //! \brief Enqueue a tracked task, returning an opaque queue ticket.
    //! \details A fork/join handle can reclaim this exact queued task.
    //! This is an internal scheduling primitive; use TaskGroup for fork/join.
    size_t enqueue_ticket(VoidFunction task);

    //! \brief Extract and execute a particular still-queued task.
    //! \return False if a worker or other helper already removed the task.
    bool try_run_task(size_t ticket);

    //! \brief The size of the tasks queue
    size_t queue_size() const;

    //! \brief The number of threads
    size_t num_threads() const;

    //! \brief Set the number of threads
    //! \details If reducing the current number, this method will block until
    //! all the previous tasks are completed, previous threads are destroyed
    //! and new threads are spawned
    void set_num_threads(size_t number);

    ~ThreadPool();

  private:

    //! \brief The function wrapper handling the extraction from the queue
    //! \details Takes \a i as the index of the thread in the list, for identification when stopping selectively
    VoidFunction _task_wrapper_function(size_t i);
    //! \brief Append threads in the given range
    void _append_thread_range(size_t lower, size_t upper);
    //! \brief Enqueue an already type-erased task
    size_t _enqueue_task(VoidFunction task, bool tracked = false);
    struct QueuedTask {
        size_t ticket;
        VoidFunction function;
    };

  private:
    const String _name;
    List<shared_ptr<Thread>> _threads;
    std::deque<QueuedTask> _tasks;
    size_t _next_ticket{1}; // Guarded by _task_availability_mutex.

    mutable mutex _task_availability_mutex;
    condition_variable _task_availability_condition;
    bool _finish_all_and_stop; // Wait till the queue is empty before stopping the thread, used for destruction
    size_t _num_active_threads; // Down-counter for checking whether all the threads to stop have been stopped
    size_t _num_threads_to_use; // Reference on the number of threads to use: if lower than the threads size, the last threads will stop
    mutable mutex _num_threads_mutex;
    promise<void> _all_unused_threads_stopped_promise;
    future<void> _all_unused_threads_stopped_future;
};

template<class F, class... AS>
auto ThreadPool::enqueue(F &&f, AS &&... args) -> future<ResultOf<F(AS...)>> {
    using ReturnType = ResultOf<F(AS...)>;

    auto task = make_shared<packaged_task<ReturnType()> >(std::bind(std::forward<F>(f), std::forward<AS>(args)...));
    future<ReturnType> result = task->get_future();
    _enqueue_task([task]{ (*task)(); });
    return result;
}

//! \brief Utility function to construct a thread name from a \a prefix and a \a number,
//! accounting for a maximum number of threads given by \a max_number
String construct_thread_name(String prefix, size_t number, size_t max_number);

}

#endif // THREADING_THREAD_POOL_HPP
