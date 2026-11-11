/***************************************************************************
 *            workload.cpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
#include "threading/workload.hpp"

#include <iomanip>
#include <sstream>

#include "utility/macros.hpp"
#include "utility/tuple.hpp"

namespace Ariadne {

WorkloadCore::WorkloadCore() : _advancement(0), _progress_indicator(new ProgressIndicator(0)), _logger_level(0) {}

void WorkloadCore::_process() {
    unique_lock<mutex> process_lock(_process_mutex, std::try_to_lock);
    ARIADNE_PRECONDITION(process_lock.owns_lock());
    _log_scope_manager.reset(new LogScopeManager(ARIADNE_PRETTY_FUNCTION, 0));
    _logger_level = Logger::instance().current_level();
    for (;;) {
        unique_lock<mutex> lock(_element_availability_mutex);
        _element_availability_condition.wait(lock, [this] {
            if (_exception != nullptr)
                return _advancement.processing() == 0;
            return _advancement.has_finished() or not _sequential_queue.empty();
        });
        if (_exception != nullptr) {
            auto exception = _exception;
            _exception = nullptr;
            _log_scope_manager.reset();
            rethrow_exception(exception);
        }
        if (_advancement.has_finished()) {
            _log_scope_manager.reset();
            return;
        }

        CompletelyBoundFunctionType task, progress_acknowledge;
        make_lpair(task, progress_acknowledge) = _sequential_queue.front();
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

size_t WorkloadCore::_size() const {
    lock_guard<mutex> lock(_element_availability_mutex);
    return _sequential_queue.size();
}

void WorkloadCore::_append_bound(CompletelyBoundFunctionType task, CompletelyBoundFunctionType progress_acknowledge) {
    {
        lock_guard<mutex> lock(_element_availability_mutex);
        _advancement.add_to_waiting();
        _sequential_queue.emplace(std::move(task), std::move(progress_acknowledge));
    }
    _element_availability_condition.notify_one();
}

void WorkloadCore::_enqueue_bound(CompletelyBoundFunctionType task, CompletelyBoundFunctionType progress_acknowledge) {
    if (_using_concurrency()) {
        _advancement.add_to_waiting();
        ThreadManager::instance().enqueue(VoidFunction([this, task = std::move(task), progress_acknowledge = std::move(progress_acknowledge)] {
            _concurrent_task_wrapper(task, progress_acknowledge);
        }));
    } else {
        _append_bound(std::move(task), std::move(progress_acknowledge));
    }
}

bool WorkloadCore::_using_concurrency() const { return ThreadManager::instance().concurrency() > 0; }

void WorkloadCore::_concurrent_task_wrapper(const CompletelyBoundFunctionType& task, const CompletelyBoundFunctionType& progress_acknowledge) {
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
            if (_exception == nullptr)
                _exception = std::current_exception();
        }
        _element_availability_condition.notify_one();
    }

    {
        lock_guard<mutex> lock(_element_availability_mutex);
        _advancement.add_to_completed();
        _element_availability_condition.notify_one();
    }
}

void WorkloadCore::_print_hold() {
    std::ostringstream logger_stream;
    logger_stream << "[" << _progress_indicator->symbol() << "] " << _progress_indicator->percentage() << "% ";
    logger_stream << " (w=" << std::setw(2) << std::left << _advancement.waiting()
                  << " p=" << std::setw(2) << std::left << _advancement.processing()
                  << " c=" << std::setw(3) << std::left << _advancement.completed()
                  << ")";
    Logger::instance().hold(_log_scope_manager->scope(), logger_stream.str());
}

} // namespace Ariadne
