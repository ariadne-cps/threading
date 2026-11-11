/***************************************************************************
 *            thread_pool.hpp
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

/*! \file thread_pool.hpp
 *  \brief Shared task scheduler with selectable dispatch policy.
 */
#ifndef THREADING_THREAD_POOL_HPP
#define THREADING_THREAD_POOL_HPP

#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "logging/logging.hpp"
#include "utility/container.hpp"
#include "threading/thread.hpp"
#include "threading/templates.hpp"
#include "threading/using.hpp"

namespace Ariadne {

using std::make_shared;
using Ariadne::List;

const String THREAD_POOL_DEFAULT_NAME = "thr";

class StoppedThreadPoolException : public std::exception { };

//! \brief Shared task scheduler for independent and nested task groups.
//! \details Local LIFO queues, FIFO stealing and a global FIFO injection queue.
//! Workers periodically service global work even when local tasks are ready.
class ThreadPool {
  private:
    struct QueuedTask;
    using TaskQueue=std::list<QueuedTask>;

  public:
    struct TaskTicket {
      private:
        friend class ThreadPool;
        ThreadPool* owner=nullptr;
        TaskQueue* queue=nullptr;
        std::mutex* queue_mutex=nullptr;
        TaskQueue::iterator position{};
        bool pending=false;
    };
    using Ticket=std::shared_ptr<TaskTicket>;

    explicit ThreadPool(size_t num_threads,
                        String name=THREAD_POOL_DEFAULT_NAME);
    ThreadPool(ThreadPool const&)=delete;
    ThreadPool& operator=(ThreadPool const&)=delete;
    ThreadPool(ThreadPool&&)=delete;
    ThreadPool& operator=(ThreadPool&&)=delete;
    ~ThreadPool() noexcept;

    template<class F,class... AS>
    auto enqueue(F&& f,AS&&... args) -> future<ResultOf<F(AS...)>>;

    String name() const;
    bool try_run_one_task();
    Ticket enqueue_ticket(VoidFunction task);
    bool try_run_task(Ticket const& ticket);
    size_t queue_size() const noexcept;
    size_t num_threads() const;

    //! \brief Administratively resize the worker set.
    //! \details Shrinking waits until existing queued and running tasks
    //! finish, then joins retired workers. Do not resize from a pool task.
    void set_num_threads(size_t number);

  private:
    struct QueuedTask {
        Ticket ticket;
        VoidFunction function;
    };
    struct LocalQueue {
        std::mutex mutex;
        TaskQueue tasks;
    };
    struct WorkerContext {
        ThreadPool* pool;
        size_t index;
        unsigned local_picks;
    };
    static thread_local WorkerContext _current;
    static constexpr size_t no_worker=static_cast<size_t>(-1);

    Ticket _enqueue_task(VoidFunction task,bool tracked=false);
    // Caller holds the queue's mutex.
    void _extract(TaskQueue& queue,TaskQueue::iterator it,VoidFunction& task);
    bool _take(VoidFunction& task);
    bool _take_global(VoidFunction& task);
    bool _take_local(size_t index,VoidFunction& task,bool steal);
    void _execute(VoidFunction task);
    void _worker_loop(size_t index);
    void _finish_activity() noexcept;

    const String _name;
    static constexpr unsigned max_local_picks=8u;
    std::vector<std::unique_ptr<LocalQueue>> _locals;
    TaskQueue _global;
    std::mutex _global_mutex;
    std::vector<std::shared_ptr<Thread>> _workers;

    // Shared reads allow parallel queue operations; resize is exclusive.
    mutable std::shared_mutex _topology_mutex;
    mutable std::mutex _resize_mutex;
    std::atomic<size_t> _pending{0};
    std::atomic<size_t> _desired_workers{0};
    // The upper bit closes external submission; low bits count outstanding
    // tasks, including reservations that are not yet in a queue.
    static constexpr size_t stopping_bit=size_t{1} << (sizeof(size_t)*8u-1u);
    std::atomic<size_t> _activity{0};
    std::atomic<size_t> _epoch{0}; // Atomic wait/notify wake-up generation.
};

template<class F,class... AS>
auto ThreadPool::enqueue(F&& f,AS&&... args) -> future<ResultOf<F(AS...)>> {
    using ReturnType=ResultOf<F(AS...)>;
    auto task=make_shared<packaged_task<ReturnType()>>(
        std::bind(std::forward<F>(f),std::forward<AS>(args)...));
    future<ReturnType> result=task->get_future();
    _enqueue_task([task] { (*task)(); });
    return result;
}

String construct_thread_name(String prefix,size_t number,size_t max_number);

} // namespace Ariadne
#endif // THREADING_THREAD_POOL_HPP
