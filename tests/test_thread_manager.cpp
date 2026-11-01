/***************************************************************************
 *            test_task_manager.cpp
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

#include <chrono>
#include <thread>
#include "ariadne-testing.hpp"
#include "threading/thread_manager.hpp"

using namespace Ariadne;
using namespace std::chrono_literals;

class TestThreadManager : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:

    void test_set_concurrency() {
        auto max_concurrency = ThreadManager::instance().maximum_concurrency();
        ThreadManager::instance().set_concurrency(max_concurrency);
        ARIADNE_TESTING_EXPECT_EQUAL(ThreadManager::instance().concurrency(), max_concurrency);
        ThreadManager::instance().set_maximum_concurrency();
        ARIADNE_TESTING_EXPECT_EQUAL(ThreadManager::instance().concurrency(), max_concurrency);
        ARIADNE_TESTING_EXPECT_EXCEPTION(ThreadManager::instance().set_concurrency(1 + max_concurrency));
    }

    void test_run_task_with_one_thread() {
        ThreadManager::instance().set_concurrency(1);
        int a = 10;
        auto result = ThreadManager::instance().enqueue(std::function<int()>([&a]{ return a * a; })).get();
        ARIADNE_TESTING_EXPECT_EQUAL(result,100);
    }

    void test_run_task_with_multiple_threads() {
        ThreadManager::instance().set_concurrency(ThreadManager::instance().maximum_concurrency());
        int a = 10;
        auto result = ThreadManager::instance().enqueue(std::function<int()>([&a]{ return a * a; })).get();
        ARIADNE_TESTING_EXPECT_EQUAL(result,100);
    }

    void test_run_task_with_no_threads() {
        ThreadManager::instance().set_concurrency(0);
        int a = 10;
        auto result = ThreadManager::instance().enqueue(std::function<int()>([&a]{ return a * a; })).get();
        ARIADNE_TESTING_EXPECT_EQUAL(result,100);
    }


    void test_void_task_with_no_threads() {
        ThreadManager::instance().set_concurrency(0);
        std::atomic<bool> executed = false;
        ThreadManager::instance().enqueue(VoidFunction([&] { executed = true; })).get();
        ARIADNE_TESTING_PROVE(executed.load());
    }

    void test_concurrency_read_during_shrink() {
        if (ThreadManager::instance().maximum_concurrency() < 2) { ctx().skip("requires at least two hardware threads"); return; }
        ThreadManager::instance().set_concurrency(2);
        std::atomic<bool> task_started = false;
        std::atomic<bool> allow_read = false;
        // Return the worker's observation to the owning case thread.
        auto future = ThreadManager::instance().enqueue([&]() -> size_t {
            task_started = true;
            while (not allow_read.load()) std::this_thread::yield();
            return ThreadManager::instance().concurrency();
        });

        ARIADNE_TESTING_EXPECT_ATOMIC(task_started,
            [](bool started) { return started; },5s);

        auto shrink = std::async(std::launch::async,[] {
            ThreadManager::instance().set_concurrency(1);
        });

        while (ThreadManager::instance().concurrency() != 1) std::this_thread::yield();
        allow_read = true;
        ARIADNE_TESTING_EXPECT_READY(future,5s);
        const auto observed_concurrency = future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(observed_concurrency,size_t{1});
        shrink.get();
        ThreadManager::instance().set_concurrency(0);
    }


    void test_thread_registry_during_shrink_to_zero() {
        if (ThreadManager::instance().maximum_concurrency() == 0) { ctx().skip("requires at least one hardware thread"); return; }
        ThreadManager::instance().set_concurrency(1);
        std::atomic<bool> task_started = false;
        std::atomic<bool> allow_finish = false;

        auto future = ThreadManager::instance().enqueue(VoidFunction([&] {
            task_started = true;
            while (not allow_finish.load()) std::this_thread::yield();
        }));
        ARIADNE_TESTING_EXPECT_ATOMIC(task_started,
            [](bool started) { return started; },5s);

        auto shrink = std::async(std::launch::async,[] {
            ThreadManager::instance().set_concurrency(0);
        });
        while (ThreadManager::instance().concurrency() != 0) std::this_thread::yield();

        ARIADNE_TESTING_PROVE(ThreadManager::instance().has_threads_registered());
        allow_finish = true;
        future.get();
        shrink.get();
        ARIADNE_TESTING_PROVE(not ThreadManager::instance().has_threads_registered());
    }


    void test_worker_cannot_shrink_manager() {
        if (ThreadManager::instance().maximum_concurrency() == 0) { ctx().skip("requires at least one hardware thread"); return; }
        ThreadManager::instance().set_concurrency(1);
        auto future = ThreadManager::instance().enqueue(VoidFunction([] {
            ThreadManager::instance().set_concurrency(0);
        }));
        ARIADNE_TESTING_EXPECT_EXCEPTION(future.get());
        ARIADNE_TESTING_EXPECT_EQUAL(ThreadManager::instance().concurrency(),1);
        ThreadManager::instance().set_concurrency(0);
    }

    void test_change_concurrency_and_log_scheduler() {
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_concurrency(1));
        ARIADNE_TESTING_EXPECT_EXCEPTION(ThreadManager::instance().set_logging_immediate_scheduler());
        ARIADNE_TESTING_EXPECT_EXCEPTION(ThreadManager::instance().set_logging_blocking_scheduler());
        ARIADNE_TESTING_EXPECT_EXCEPTION(ThreadManager::instance().set_logging_nonblocking_scheduler());
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_concurrency(0));
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_logging_immediate_scheduler());
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_logging_blocking_scheduler());
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_logging_nonblocking_scheduler());
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_concurrency(1));
        ARIADNE_TESTING_EXPECT_NO_THROW(ThreadManager::instance().set_concurrency(0));
    }

    static void add_cases(Ariadne::Testing::Suite& suite) {
        suite.add("test_set_concurrency", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_set_concurrency();
        });
        suite.add("test_run_task_with_one_thread", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_run_task_with_one_thread();
        });
        suite.add("test_run_task_with_multiple_threads", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_run_task_with_multiple_threads();
        });
        suite.add("test_run_task_with_no_threads", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_run_task_with_no_threads();
        });
        suite.add("test_void_task_with_no_threads", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_void_task_with_no_threads();
        });
        suite.add("test_concurrency_read_during_shrink", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_concurrency_read_during_shrink();
        });
        suite.add("test_worker_cannot_shrink_manager", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_worker_cannot_shrink_manager();
        });
        suite.add("test_change_concurrency_and_log_scheduler", [](Ariadne::Testing::Context& context) {
            TestThreadManager fixture(context);
            fixture.test_change_concurrency_and_log_scheduler();
        });
    }
};

int main(int argc, char** argv) {
    Ariadne::Testing::Suite suite("thread_manager");
    TestThreadManager::add_cases(suite);
    return Ariadne::Testing::run_suite(suite, argc, argv);
}