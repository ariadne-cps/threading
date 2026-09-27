/***************************************************************************
 *            buffered_thread.hpp
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

/*! \file buffered_thread.hpp
 *  \brief A wrapper for smart handling of a thread with a buffer for incoming tasks
 */

#ifndef THREADING_BUFFERED_THREAD_HPP
#define THREADING_BUFFERED_THREAD_HPP

#include <utility>
#include <thread>
#include <future>
#include <mutex>
#include <atomic>
#include <functional>
#include "utility/string.hpp"
#include "threading/templates.hpp"
#include "threading/buffer.hpp"
#include "threading/using.hpp"

namespace Ariadne {

using Ariadne::String;

//! \brief A class for handling a thread that accepts multiple tasks to be enqueued.
//! \details It allows to wait for the start of the \a task before extracting the thread id, which is held along with
//! a readable \a name. The thread can execute only one task at a time. Compared with Thread, this is meant to be used in
//! isolation, not in pool. It is functionally equivalent to a ThreadPool of one Thread only.
class BufferedThread {
  public:

    //! \brief Construct with a name.
    //! \details The thread will start and store the id. The name will be the id if left empty
    BufferedThread(String name = String());

    //! \brief Enqueue a task for execution, returning the future handler
    //! \details If the buffer is full, successive calls will block until an execution is started.
    template<class F, class... AS>
    auto enqueue(F&& f, AS&&... args) -> future<ResultOf<F(AS...)>>;

    //! \brief Get the thread id
    thread::id id() const;
    //! \brief Get the readable name
    String name() const;

    //! \brief The current size of the queue
    size_t queue_size() const;
    //! \brief The capacity of the tasks to execute
    size_t queue_capacity() const;
    //! \brief Change the queue capacity
    //! \details Capacity cannot be changed to a value lower than the current size
    void set_queue_capacity(size_t capacity);

    //! \brief Destroy the instance
    ~BufferedThread();

  private:
    String _name;
    thread::id _id;
    std::thread _thread;
    Buffer<std::function<void(void)>> _task_buffer;
    promise<void> _got_id_promise;
    future<void> _got_id_future;
};

template<class F, class... AS> auto BufferedThread::enqueue(F&& f, AS&&... args) -> future<ResultOf<F(AS...)>>
{
    using ReturnType = ResultOf<F(AS...)>;

    auto task = std::make_shared<packaged_task<ReturnType()>>(std::bind(std::forward<F>(f), std::forward<AS>(args)...));
    future<ReturnType> result = task->get_future();
    _task_buffer.push([task](){ (*task)(); });
    return result;
}

} // namespace Ariadne

#endif // THREADING_BUFFERED_THREAD_HPP
