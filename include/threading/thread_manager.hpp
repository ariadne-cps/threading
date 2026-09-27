/***************************************************************************
 *            task_manager.hpp
 *
 *  Copyright  2022 Luca Geretti
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

/*! \file task_manager.hpp
 *  \brief Singleton class for managing tasks across the library
 */

#ifndef THREADING_THREAD_MANAGER_HPP
#define THREADING_THREAD_MANAGER_HPP

#include <algorithm>
#include <atomic>
#include "logging/logging.hpp"
#include "logging/thread_registry_interface.hpp"
#include "threading/thread_pool.hpp"
#include "threading/templates.hpp"

namespace Ariadne {

using Ariadne::ThreadRegistryInterface;
using Ariadne::Logger;

//! \brief Manages threads based on concurrency availability.
class ThreadManager : public ThreadRegistryInterface {
  private:
    ThreadManager();
  public:
    ThreadManager(ThreadManager const&) = delete;
    void operator=(ThreadManager const&) = delete;

    virtual ~ThreadManager() = default;

    //! \brief The singleton instance of this class
    static ThreadManager& instance();

    //! \brief Whether threads have already been registered
    bool has_threads_registered() const override;

    //! \brief Get the maximum concurrency allowed by this machine
    size_t maximum_concurrency() const;
    //! \brief Get the preferred concurrency to be used
    //! \details A concurrency of zero is allowed, meaning that a task
    //! will be run sequentially
    size_t concurrency() const;

    //! \brief Synchronised method for updating the preferred concurrency to be used
    void set_concurrency(size_t value);

    //! \brief Set the concurrency to the maximum allowed by this machine
    void set_maximum_concurrency();

    //! \brief Set the Logger scheduler to the immediate one
    //! \details Fails if the concurrency is not zero
    void set_logging_immediate_scheduler() const;
    //! \brief Set the Logger scheduler to the blocking one
    //! \details Fails if the concurrency is not zero
    void set_logging_blocking_scheduler() const;
    //! \brief Set the Logger scheduler to the nonblocking one
    //! \details Fails if the concurrency is not zero
    void set_logging_nonblocking_scheduler() const;

    //! \brief Enqueue a task for execution, returning the future handler
    //! \details The is no limits on the number of tasks to enqueue. If concurrency is zero,
    //! then the task is executed sequentially with no threads involved
    template<class F, class... AS> auto enqueue(F &&f, AS &&... args) -> future<ResultOf<F(AS...)>>;

  private:
    const size_t _maximum_concurrency;
    std::atomic<size_t> _concurrency;
    mutable mutex _concurrency_change_mutex;

    ThreadPool _pool;
};

template<class F, class... AS> auto ThreadManager::enqueue(F &&f, AS &&... args) -> future<ResultOf<F(AS...)>> {
    if (_concurrency.load() == 0) {
        using ReturnType = ResultOf<F(AS...)>;
        auto task = packaged_task<ReturnType()>(std::bind(std::forward<F>(f), std::forward<AS>(args)...));
        future<ReturnType> result = task.get_future();
        task();
        return result;
    }
    return _pool.enqueue(std::forward<F>(f),std::forward<AS>(args)...);
}

} // namespace Ariadne

#endif // THREADING_THREAD_MANAGER_HPP
