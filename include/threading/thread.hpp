/***************************************************************************
 *            thread.hpp
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

/*! \file thread.hpp
 *  \brief A wrapper for smart handling of a thread
 */

#ifndef THREADING_THREAD_HPP
#define THREADING_THREAD_HPP

#include <utility>
#include <thread>
#include <future>
#include <mutex>
#include <atomic>
#include <functional>
#include "utility/macros.hpp"
#include "utility/string.hpp"

namespace Ariadne {

using std::exception_ptr;
using std::thread;
using std::promise;
using std::future;

using VoidFunction = std::function<void(void)>;
using Ariadne::String;

//! \brief A class for handling a thread for a pool in a smarter way.
//! \details It allows to wait for the start of the \a task before extracting the thread id, which is held along with
//! a readable \a name.
class Thread {
  public:

    //! \brief Construct with a \a name and \a active specification
    //! \details The thread will start and store the id if active, otherwise activate() will be needed.
    Thread(VoidFunction task, String name, bool active);

    //! \brief Construct with default active=true and possibly default String name equal to the thread id
    Thread(VoidFunction task, String name = std::string());

    //! \brief Get the thread id
    thread::id id() const;
    //! \brief Get the readable name
    String name() const;

    //! \brief Activate the thread
    //! \details If already active (whether at construction of by a previous call to activate()), will do nothing.
    void activate();

    //! \brief The exception, if it exists
    exception_ptr exception() const;

    //! \brief Destroy the instance
    ~Thread();

  private:
    String _name;
    thread::id _id;
    std::thread _thread;
    promise<void> _got_id_promise;
    future<void> _got_id_future;
    std::atomic<bool> _active;
    promise<void> _ready_for_task_promise;
    future<void> _ready_for_task_future;
    exception_ptr _exception;
    mutable std::mutex _exception_mutex;
};

} // namespace Ariadne

#endif // THREADING_THREAD_HPP
