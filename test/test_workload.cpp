/***************************************************************************
 *            test_workload.cpp
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

#include <functional>
#include "utility/test.hpp"
#include "utility/container.hpp"
#include "threading/workload.hpp"

using namespace Ariadne;

template<class T> class SynchronisedList : public List<T> {
  public:
    void append(T const& v) { lock_guard<mutex> guard(_mux); return List<T>::push_back(v); }
  private:
    mutex _mux;
};

using StaticWorkloadType = StaticWorkload<int,std::shared_ptr<std::atomic<int>>>;
using DynamicWorkloadType = DynamicWorkload<int,std::shared_ptr<SynchronisedList<int>>>;

void sum_all(int const& val, std::shared_ptr<std::atomic<int>> result) {
    result->operator+=(val);
}

void print(int const& val) {
    LOGGING_PRINTLN_VAR(val)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void square_and_store(DynamicWorkloadType::Access& wla, int const& val, std::shared_ptr<SynchronisedList<int>> results) {
    int next_val = val*val;
    if (next_val < 46340) {
        wla.append(next_val);
    }
    results->append(next_val);
}

void progress_acknowledge(int const& val, std::shared_ptr<ProgressIndicator> indicator) {
    indicator->update_current(val);
    indicator->update_final(std::numeric_limits<int>::max());
}

void throw_exception_immediately(DynamicWorkloadType::Access&, int const&, std::shared_ptr<SynchronisedList<int>>) {
    throw new std::exception();
}

void throw_exception_later(DynamicWorkloadType::Access& wla, int const& val, std::shared_ptr<SynchronisedList<int>>) {
    int next_val = val+1;
    if (next_val > 4) throw new std::exception();
    else wla.append(next_val);
}




struct ProcessConcurrencyState {
    std::atomic<bool> started = false;
    std::atomic<bool> release = false;
};

void block_until_released(int const&, std::shared_ptr<ProcessConcurrencyState> state) {
    state->started = true;
    while (not state->release.load()) std::this_thread::yield();
}

struct ProgressConcurrencyState {
    std::atomic<size_t> active = 0;
    std::atomic<size_t> maximum = 0;
};


struct ConcurrentDoubleExceptionState {
    std::atomic<size_t> ready = 0;
};

void throw_together(int const&, std::shared_ptr<ConcurrentDoubleExceptionState> state) {
    ++state->ready;
    while (state->ready.load() < 2) std::this_thread::yield();
    throw std::runtime_error("expected");
}

struct ConcurrentExceptionState {
    std::atomic<bool> slow_started = false;
    std::atomic<bool> slow_finished = false;
};

void throw_while_other_task_runs(int const& val, std::shared_ptr<ConcurrentExceptionState> state) {
    if (val == 1) {
        state->slow_started = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        state->slow_finished = true;
        return;
    }
    while (not state->slow_started) std::this_thread::yield();
    throw std::runtime_error("expected");
}


void throw_once_then_accumulate(int const& value, std::shared_ptr<std::atomic<bool>> first, std::shared_ptr<std::atomic<int>> total) {
    if (first->exchange(false)) throw std::runtime_error("expected");
    total->operator+=(value);
}

class TestWorkload {
  public:

    void test_construct_static() {
        ThreadManager::instance().set_concurrency(0);
        auto result = std::make_shared<std::atomic<int>>();
        StaticWorkloadType wl(&sum_all, result);
    }

    void test_construct_dynamic() {
        ThreadManager::instance().set_concurrency(0);
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        ARIADNE_TEST_EQUALS(wl.size(),0)
    }

    void test_append() {
        ThreadManager::instance().set_concurrency(0);
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        wl.append(2);
        ARIADNE_TEST_EQUALS(wl.size(),1)
        wl.append({10,20});
        ARIADNE_TEST_EQUALS(wl.size(),3)
    }

    void test_process_nothing() {
        ThreadManager::instance().set_maximum_concurrency();
        auto result = std::make_shared<std::atomic<int>>();
        StaticWorkloadType wl(&sum_all, result);
        ARIADNE_TEST_EXECUTE(wl.process())
    }

    void test_serial_processing_static() {
        ThreadManager::instance().set_concurrency(0);
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        result->append(2);
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        wl.append(2);
        wl.process();
        ARIADNE_TEST_PRINT(*result)
        ARIADNE_TEST_EQUALS(result->size(),5)
    }

    void test_serial_processing_dynamic() {
        ThreadManager::instance().set_concurrency(0);
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        result->append(2);
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        wl.append(2);
        wl.process();
        ARIADNE_TEST_PRINT(*result)
        ARIADNE_TEST_EQUALS(result->size(),5)
    }

    void test_concurrent_processing_static() {
        ThreadManager::instance().set_maximum_concurrency();
        auto result = std::make_shared<std::atomic<int>>();
        *result = 0;
        StaticWorkloadType wl(&sum_all, result);
        wl.append({2,7,-3,5,8,10,5,8});
        wl.process();
        ARIADNE_TEST_EQUALS(*result,42)
    }

    void test_concurrent_processing_dynamic() {
        ThreadManager::instance().set_maximum_concurrency();
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        result->append(2);
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        wl.append(2);
        wl.process();
        ARIADNE_TEST_PRINT(*result)
        ARIADNE_TEST_EQUALS(result->size(),5)
    }

    void test_print_hold() {
        ThreadManager::instance().set_concurrency(0);
        Logger::instance().configuration().set_verbosity(2);
        StaticWorkload<int> wl(&print);
        wl.append({1,2,3,4,5});
        wl.process();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Logger::instance().configuration().set_verbosity(0);
    }

    void test_throw_serial_exception_immediately() {
        ThreadManager::instance().set_concurrency(0);
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &throw_exception_immediately, result);
        wl.append(2);
        ARIADNE_TEST_FAIL(wl.process())
    }

    void test_throw_serial_exception_later() {
        ThreadManager::instance().set_concurrency(0);
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &throw_exception_later, result);
        wl.append(2);
        ARIADNE_TEST_FAIL(wl.process())
    }

    void test_throw_concurrent_exception_immediately() {
        ThreadManager::instance().set_maximum_concurrency();
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &throw_exception_immediately, result);
        wl.append(2);
        ARIADNE_TEST_FAIL(wl.process())
    }

    void test_throw_concurrent_exception_later() {
        ThreadManager::instance().set_maximum_concurrency();
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &throw_exception_later, result);
        wl.append(2);
        ARIADNE_TEST_FAIL(wl.process())
    }





    void test_concurrent_logger_level_increase() {
        if (ThreadManager::instance().maximum_concurrency() == 0) return;

        ThreadManager::instance().set_concurrency(0);
        Logger::instance().configuration().set_verbosity(0);
        ThreadManager::instance().set_concurrency(1);
        Logger::instance().increase_level(1);

        auto result = std::make_shared<std::atomic<int>>(0);
        StaticWorkloadType workload(&sum_all,result);
        workload.append(1);
        workload.process();

        Logger::instance().decrease_level(1);
        ARIADNE_TEST_EQUALS(result->load(),1)
        ThreadManager::instance().set_concurrency(0);
    }


    void test_concurrent_logger_level_decrease() {
        if (ThreadManager::instance().maximum_concurrency() == 0) return;

        ThreadManager::instance().set_concurrency(0);
        Logger::instance().configuration().set_verbosity(0);
        Logger::instance().increase_level(1);
        ThreadManager::instance().set_concurrency(1);
        Logger::instance().decrease_level(1);

        auto result = std::make_shared<std::atomic<int>>(0);
        StaticWorkloadType workload(&sum_all,result);
        workload.append(1);
        workload.process();

        ARIADNE_TEST_EQUALS(result->load(),1)
        ThreadManager::instance().set_concurrency(0);
    }

    void test_concurrent_process_is_rejected() {
        ThreadManager::instance().set_concurrency(0);
        auto state = std::make_shared<ProcessConcurrencyState>();
        StaticWorkload<int,std::shared_ptr<ProcessConcurrencyState>> wl(&block_until_released,state);
        wl.append(1);
        Thread processor([&wl] { wl.process(); },"processor");
        while (not state->started.load()) std::this_thread::yield();
        ARIADNE_TEST_FAIL(wl.process())
        state->release = true;
    }

    void test_progress_acknowledgement_is_serialised() {
        if (ThreadManager::instance().maximum_concurrency() < 2) return;
        ThreadManager::instance().set_concurrency(2);
        Logger::instance().configuration().set_verbosity(2);
        auto state = std::make_shared<ProgressConcurrencyState>();
        auto result = std::make_shared<std::atomic<int>>();
        using WorkloadType = DynamicWorkload<int,std::shared_ptr<std::atomic<int>>>;
        auto progress = [state](int const&, std::shared_ptr<ProgressIndicator> indicator) {
            auto active = ++state->active;
            auto maximum = state->maximum.load();
            while (active > maximum and not state->maximum.compare_exchange_weak(maximum,active)) { }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            indicator->update_current(static_cast<double>(active));
            indicator->update_final(10.0);
            --state->active;
        };
        auto task = [](WorkloadType::Access&, int const& value, std::shared_ptr<std::atomic<int>> total) {
            total->operator+=(value);
        };
        WorkloadType wl(progress,task,result);
        wl.append({1,2,3,4});
        wl.process();
        ARIADNE_TEST_EQUALS(state->maximum.load(),1)
        Logger::instance().configuration().set_verbosity(0);
    }


    void test_multiple_concurrent_exceptions_preserve_first() {
        if (ThreadManager::instance().maximum_concurrency() < 2) return;
        ThreadManager::instance().set_concurrency(2);

        auto state = std::make_shared<ConcurrentDoubleExceptionState>();
        StaticWorkload<int,std::shared_ptr<ConcurrentDoubleExceptionState>> workload(&throw_together,state);
        workload.append({1,2});

        ARIADNE_TEST_FAIL(workload.process())
        ARIADNE_TEST_EQUALS(state->ready.load(),2)
    }

    void test_concurrent_exception_waits_for_running_tasks() {
        if (ThreadManager::instance().maximum_concurrency() < 2) return;
        ThreadManager::instance().set_concurrency(2);
        auto state = std::make_shared<ConcurrentExceptionState>();
        StaticWorkload<int,std::shared_ptr<ConcurrentExceptionState>> wl(&throw_while_other_task_runs, state);
        wl.append({1,0});
        ARIADNE_TEST_FAIL(wl.process())
        ARIADNE_TEST_ASSERT(state->slow_finished.load())
    }


    void test_reuse_after_serial_exception() {
        ThreadManager::instance().set_concurrency(0);
        auto first = std::make_shared<std::atomic<bool>>(true);
        auto total = std::make_shared<std::atomic<int>>(0);
        StaticWorkload<int,std::shared_ptr<std::atomic<bool>>,std::shared_ptr<std::atomic<int>>> wl(&throw_once_then_accumulate, first, total);
        wl.append(1);
        ARIADNE_TEST_FAIL(wl.process())
        wl.append(7);
        ARIADNE_TEST_EXECUTE(wl.process())
        ARIADNE_TEST_EQUALS(total->load(),7)
    }

    void test_reuse_after_concurrent_exception() {
        if (ThreadManager::instance().maximum_concurrency() == 0) return;
        ThreadManager::instance().set_concurrency(1);
        auto first = std::make_shared<std::atomic<bool>>(true);
        auto total = std::make_shared<std::atomic<int>>(0);
        StaticWorkload<int,std::shared_ptr<std::atomic<bool>>,std::shared_ptr<std::atomic<int>>> wl(&throw_once_then_accumulate, first, total);
        wl.append(1);
        ARIADNE_TEST_FAIL(wl.process())
        wl.append(9);
        ARIADNE_TEST_EXECUTE(wl.process())
        ARIADNE_TEST_EQUALS(total->load(),9)
    }

    void test_multiple_append() {
        ThreadManager::instance().set_maximum_concurrency();
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        result->append(2);
        result->append(3);
        wl.append({2,3});
        wl.process();
        ARIADNE_TEST_PRINT(*result)
        ARIADNE_TEST_EQUALS(result->size(),10)
    }

    void test_multiple_process() {
        ThreadManager::instance().set_maximum_concurrency();
        std::shared_ptr<SynchronisedList<int>> result = std::make_shared<SynchronisedList<int>>();
        result->append(2);
        DynamicWorkloadType wl(&progress_acknowledge, &square_and_store, result);
        wl.append(2);
        wl.process();
        result->clear();
        result->append(3);
        wl.append(3);
        wl.process();
        ARIADNE_TEST_PRINT(*result)
        ARIADNE_TEST_EQUALS(result->size(),5)
    }

    void test() {
        ARIADNE_TEST_CALL(test_construct_static())
        ARIADNE_TEST_CALL(test_construct_dynamic())
        ARIADNE_TEST_CALL(test_append())
        ARIADNE_TEST_CALL(test_process_nothing())
        ARIADNE_TEST_CALL(test_serial_processing_static())
        ARIADNE_TEST_CALL(test_serial_processing_dynamic())
        ARIADNE_TEST_CALL(test_concurrent_processing_static())
        ARIADNE_TEST_CALL(test_concurrent_processing_dynamic())
        ARIADNE_TEST_CALL(test_print_hold())
        ARIADNE_TEST_CALL(test_throw_serial_exception_immediately())
        ARIADNE_TEST_CALL(test_throw_serial_exception_later())
        ARIADNE_TEST_CALL(test_throw_concurrent_exception_immediately())
        ARIADNE_TEST_CALL(test_throw_concurrent_exception_later())
        ARIADNE_TEST_CALL(test_concurrent_logger_level_increase())
        ARIADNE_TEST_CALL(test_concurrent_logger_level_decrease())
        ARIADNE_TEST_CALL(test_concurrent_process_is_rejected())
        ARIADNE_TEST_CALL(test_progress_acknowledgement_is_serialised())
        ARIADNE_TEST_CALL(test_multiple_concurrent_exceptions_preserve_first())
        ARIADNE_TEST_CALL(test_concurrent_exception_waits_for_running_tasks())
        ARIADNE_TEST_CALL(test_reuse_after_serial_exception())
        ARIADNE_TEST_CALL(test_reuse_after_concurrent_exception())
        ARIADNE_TEST_CALL(test_multiple_append())
        ARIADNE_TEST_CALL(test_multiple_process())
    }

};

int main() {
    TestWorkload().test();
    return ARIADNE_TEST_FAILURES;
}
