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
#include <iomanip>
#include "utility/container.hpp"
#include "utility/tuple.hpp"
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

//! \brief Non-template core shared by all workload instantiations
class WorkloadCore {
  protected:
    using CompletelyBoundFunctionType = std::function<void(void)>;

    WorkloadCore() : _advancement(0), _progress_indicator(new ProgressIndicator(0)), _logger_level(0) { }

    void _process() {
        unique_lock<mutex> process_lock(_process_mutex,std::try_to_lock);
        ARIADNE_PRECONDITION(process_lock.owns_lock());
        _log_scope_manager.reset(new LogScopeManager(ARIADNE_PRETTY_FUNCTION,0));
        _logger_level = Logger::instance().current_level();
        for (;;) {
            unique_lock<mutex> lock(_element_availability_mutex);
            _element_availability_condition.wait(lock, [this] {
                if (_exception != nullptr) return _advancement.processing() == 0;
                return _advancement.has_finished() or not _sequential_queue.empty();
            });
            if (_exception != nullptr) {
                auto exception = _exception;
                _exception = nullptr;
                _log_scope_manager.reset();
                rethrow_exception(exception);
            }
            if (_advancement.has_finished()) { _log_scope_manager.reset(); return; }

            CompletelyBoundFunctionType task, progress_acknowledge;
            make_lpair(task,progress_acknowledge) = _sequential_queue.front();
            _sequential_queue.pop();
            lock.unlock();
            if (_using_concurrency()) {
                ThreadManager::instance().enqueue(VoidFunction([this, task, progress_acknowledge] { _concurrent_task_wrapper(task, progress_acknowledge); }));
            } else {
                _advancement.add_to_processing();
                if (not Logger::instance().is_muted_at(0)) {
                    lock_guard<mutex> progress_lock(_progress_mutex);
                    progress_acknowledge();
                    _print_hold();
                }
                try {
                    task();
                } catch (...) {
                    _advancement.add_to_completed();
                    _log_scope_manager.reset();
                    throw;
                }
                _advancement.add_to_completed();
            }
        }
    }

    size_t _size() const {
        lock_guard<mutex> lock(_element_availability_mutex);
        return _sequential_queue.size();
    }

    void _append_bound(CompletelyBoundFunctionType task, CompletelyBoundFunctionType progress_acknowledge) {
        {
            lock_guard<mutex> lock(_element_availability_mutex);
            _advancement.add_to_waiting();
            _sequential_queue.emplace(std::move(task),std::move(progress_acknowledge));
        }
        _element_availability_condition.notify_one();
    }

    void _enqueue_bound(CompletelyBoundFunctionType task, CompletelyBoundFunctionType progress_acknowledge) {
        if (_using_concurrency()) {
            _advancement.add_to_waiting();
            ThreadManager::instance().enqueue(VoidFunction([this,task=std::move(task),progress_acknowledge=std::move(progress_acknowledge)] {
                _concurrent_task_wrapper(task, progress_acknowledge);
            }));
        } else {
            _append_bound(std::move(task),std::move(progress_acknowledge));
        }
    }

    WorkloadAdvancement _advancement;
    shared_ptr<ProgressIndicator> _progress_indicator;

  private:
    bool _using_concurrency() const { return ThreadManager::instance().concurrency() > 0; }

    void _concurrent_task_wrapper(CompletelyBoundFunctionType const& task, CompletelyBoundFunctionType const& progress_acknowledge) {
        _advancement.add_to_processing();
        auto current_logger_level = Logger::instance().current_level();
        Logger::instance().decrease_level(current_logger_level);
        Logger::instance().increase_level(_logger_level);

        if (not Logger::instance().is_muted_at(0)) {
            lock_guard<mutex> progress_lock(_progress_mutex);
            progress_acknowledge();
            _print_hold();
        }
        try {
            task();
        } catch (...) {
            {
                lock_guard<mutex> lock(_element_availability_mutex);
                if (_exception == nullptr) _exception = std::current_exception();
            }
            _element_availability_condition.notify_one();
        }

        {
            lock_guard<mutex> lock(_element_availability_mutex);
            _advancement.add_to_completed();
        }
        _element_availability_condition.notify_one();
    }

    void _print_hold() {
        std::ostringstream logger_stream;
        logger_stream << "[" << _progress_indicator->symbol() << "] " << _progress_indicator->percentage() << "% ";
        logger_stream << " (w="<<std::setw(2)<<std::left<<_advancement.waiting()
                      << " p="<<std::setw(2)<<std::left<<_advancement.processing()
                      << " c="<<std::setw(3)<<std::left<<_advancement.completed()
                      << ")";
        Logger::instance().hold(_log_scope_manager->scope(),logger_stream.str());
    }

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

//! \brief A basic static workload where all elements are appended and then processed
template<class E, class... AS>
class StaticWorkload : public WorkloadBase<E,AS...> {
public:
    using TaskFunctionType = std::function<void(E const&, AS...)>;

    StaticWorkload(TaskFunctionType f, AS... as) : WorkloadBase<E, AS...>() {
        this->_task_func = std::bind(std::forward<TaskFunctionType const>(f), std::placeholders::_1, std::forward<AS>(as)...);
    }
};

//! \brief A dynamic workload in which it is possible to append new elements from the called function
template<class E, class... AS>
class DynamicWorkload : public WorkloadBase<E,AS...> {
  public:
    //! \brief Reduced interface to be used by the processing function (and any function called by it)
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