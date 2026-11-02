/***************************************************************************
 *            workload.hpp
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

/*! \file workload.hpp
 *  \brief A stack of objects to work on, supplied with a function to process them
 */

#ifndef THREADING_WORKLOAD_HPP
#define THREADING_WORKLOAD_HPP

#include <functional>
#include "utility/container.hpp"
#include "logging/progress_indicator.hpp"
#include "threading/workload_interface.hpp"
#include "threading/thread_manager.hpp"
#include "threading/workload_advancement.hpp"

namespace Ariadne {

using Ariadne::ProgressIndicator;
using Ariadne::LogScopeManager;
using Ariadne::Logger;

using Ariadne::List;
using Ariadne::make_lpair;

using std::mutex;
using std::unique_lock;
using std::lock_guard;
using std::condition_variable;

/** @brief Non-template execution and Logging engine of workload classes.
 *  @details StaticWorkload and DynamicWorkload share this implementation;
 *  template binding of element types stays in the header.
 *  The workload must outlive processing and all worker callbacks.
 */
class WorkloadCore {
  protected:
    using CompletelyBoundFunctionType = std::function<void(void)>;

    /** @brief Construct shared observable and logging state. */
    WorkloadCore();

    /** @brief Process until no work is queued or running.
     *  @details Only one process() call may be active at a time; concurrent
     *  calls are rejected. Preserves the historical breadth-first serial
     *  mode, ThreadManager scheduling and Logging scopes.
     *  @throws Propagates processing exceptions after outstanding work joins.
     */
    void _process();

    /** @brief Number of queued work elements (not total outstanding work). */
    size_t _size() const;

    /** @brief Append work to the serial queue and notify the processor. */
    void _append_bound(CompletelyBoundFunctionType task,
                       CompletelyBoundFunctionType progress_acknowledge);

    /** @brief Submit directly when concurrent, otherwise append. */
    void _enqueue_bound(CompletelyBoundFunctionType task,
                        CompletelyBoundFunctionType progress_acknowledge);

    WorkloadAdvancement _advancement;
    shared_ptr<ProgressIndicator> _progress_indicator;

  private:
    bool _using_concurrency() const;
    void _concurrent_task_wrapper(CompletelyBoundFunctionType const& task,
                                  CompletelyBoundFunctionType const& progress_acknowledge);
    void _print_hold();

    std::queue<std::pair<CompletelyBoundFunctionType,CompletelyBoundFunctionType>> _sequential_queue;
    unsigned int _logger_level;
    shared_ptr<LogScopeManager> _log_scope_manager;
    mutable mutex _element_availability_mutex;
    mutex _process_mutex;
    mutex _progress_mutex;
    condition_variable _element_availability_condition;
    exception_ptr _exception;
};

//! \brief Base class implementation
template<class E, class... AS>
class WorkloadBase : public WorkloadInterface<E,AS...>, protected WorkloadCore {
  protected:
    WorkloadBase() : _progress_acknowledge_func(std::bind_front(&WorkloadBase::_default_progress_acknowledge, this)) { }
  public:
    using TaskFunctionType = std::function<void(E const &)>;
    using ProgressAcknowledgeFunctionType = std::function<void(E const &, shared_ptr<ProgressIndicator>)>;
    using CompletelyBoundFunctionType = std::function<void(void)>;

    void process() override { this->_process(); }

    size_t size() const override { return this->_size(); }

    WorkloadSnapshot snapshot() const override { return this->_advancement.snapshot(); }

    WorkloadInterface<E,AS...>& append(E const& e) override {
        auto task = std::bind(std::forward<TaskFunctionType const>(_task_func), std::forward<E const&>(e));
        auto progress_acknowledge = std::bind(std::forward<ProgressAcknowledgeFunctionType const>(_progress_acknowledge_func),
                                              std::forward<E const&>(e), this->_progress_indicator);
        this->_append_bound(std::move(task),std::move(progress_acknowledge));
        return *this;
    }

    WorkloadInterface<E,AS...>& append(List<E> const& es) override { for (auto e : es) append(e); return *this; }

  private:

    void _default_progress_acknowledge(E const&, shared_ptr<ProgressIndicator> indicator) {
        indicator->update_current(static_cast<double>(this->_advancement.completed()));
        indicator->update_final(static_cast<double>(this->_advancement.total()));
    }

  protected:

    void _enqueue(E const& e) {
        auto task = std::bind(std::forward<TaskFunctionType const>(_task_func), std::forward<E const&>(e));
        auto progress_acknowledge = std::bind(std::forward<ProgressAcknowledgeFunctionType const>(_progress_acknowledge_func),
                                              std::forward<E const&>(e), this->_progress_indicator);
        this->_enqueue_bound(std::move(task),std::move(progress_acknowledge));
    }

  protected:

    TaskFunctionType _task_func;
    ProgressAcknowledgeFunctionType _progress_acknowledge_func;
};

/** @brief Observable batch of known work items, integrated with Logging.
 *  @tparam E Element type stored by value in the task binding.
 *  @tparam AS Additional callable parameters, forwarded at construction.
 *  @note Append all elements before process() to execute a static workload.
 */
template<class E, class... AS>
class StaticWorkload : public WorkloadBase<E,AS...> {
public:
    using TaskFunctionType = std::function<void(E const&, AS...)>;

    StaticWorkload(TaskFunctionType f, AS... as) : WorkloadBase<E, AS...>() {
        this->_task_func = std::bind(std::forward<TaskFunctionType const>(f), std::placeholders::_1, std::forward<AS>(as)...);
    }
};

/** @brief Observable dynamic work queue; callbacks may append more work.
 *  @details Processing completes only when queued and in-flight elements
 *  are exhausted. Uses ThreadManager and the historic Logging integration.
 *  @tparam E Element type.
 *  @tparam AS Additional task parameters.
 */
template<class E, class... AS>
class DynamicWorkload : public WorkloadBase<E,AS...> {
  public:
    /** @brief Restricted producer capability given to task callbacks.
     *  @note Access refers to its parent; it cannot outlive that workload.
     */
    class Access {
        friend DynamicWorkload;
    protected:
        Access(DynamicWorkload& parent) : _load(parent) { }
    public:
        void append(E const &e) { _load._enqueue(e); }
    private:
        DynamicWorkload& _load;
    };
  public:
    using TaskFunctionType = std::function<void(Access&, E const&, AS...)>;
    using ProgressAcknowledgeFunctionType = std::function<void(E const&, shared_ptr<ProgressIndicator>)>;

    DynamicWorkload(ProgressAcknowledgeFunctionType p, TaskFunctionType t, AS... as) : WorkloadBase<E, AS...>(), _access(Access(*this)) {
        this->_task_func = std::bind(std::forward<TaskFunctionType const>(t),
                                     std::forward<Access const&>(_access),
                                     std::placeholders::_1,
                                     std::forward<AS>(as)...);
        this->_progress_acknowledge_func = p;
    }

  private:
    Access const _access;
};

}

#endif // THREADING_WORKLOAD_HPP