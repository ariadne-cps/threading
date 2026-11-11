/***************************************************************************
 *            test_task_manager.cpp
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

    void test_cooperative_manager_task_acquisition() {
        auto& manager=ThreadManager::instance();
        manager.set_concurrency(0);
        ARIADNE_TESTING_PROVE(!manager.try_run_one_task());
        ARIADNE_TESTING_PROVE(!manager.try_run_task(ThreadPool::Ticket{}));

        if(manager.maximum_concurrency()==0) {
            ctx().skip("requires at least one hardware thread");
            return;
        }

        // Hold the sole worker busy so both pending tickets must be
        // acquired by the calling thread, not raced by the worker.
        std::promise<void> release_worker;
        auto release_signal=release_worker.get_future().share();
        std::atomic<bool> worker_started{false};
        manager.set_concurrency(1);
        struct Cleanup {
            ThreadManager& manager;
            std::promise<void>& release;
            ~Cleanup() {
                try { release.set_value(); } catch (...) { }
                try { manager.set_concurrency(0); } catch (...) { }
            }
        } cleanup{manager,release_worker};
        auto parked=manager.enqueue([&] {
            worker_started.store(true);
            release_signal.wait();
        });
        ARIADNE_TESTING_EXPECT_ATOMIC(worker_started,
            [](bool value) { return value; },5s);

        std::atomic<unsigned> executions{0u};
        auto targeted=manager.enqueue_ticket([&] { ++executions; });
        auto generic=manager.enqueue_ticket([&] { ++executions; });
        ARIADNE_TESTING_PROVE(manager.try_run_task(targeted));
        ARIADNE_TESTING_PROVE(!manager.try_run_task(targeted));
        ARIADNE_TESTING_PROVE(manager.try_run_one_task());
        ARIADNE_TESTING_PROVE(!manager.try_run_task(generic));
        ARIADNE_TESTING_PROVE(!manager.try_run_one_task());
        ARIADNE_TESTING_EXPECT_EQUAL(executions.load(),2u);

        release_worker.set_value();
        parked.get();
        manager.set_concurrency(0);
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

};

int main(int argc, char** argv) {
    ARIADNE_TESTING_SUITE("thread_manager");
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_set_concurrency);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_run_task_with_one_thread);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_run_task_with_multiple_threads);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_run_task_with_no_threads);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_void_task_with_no_threads);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_concurrency_read_during_shrink);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_worker_cannot_shrink_manager);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_cooperative_manager_task_acquisition);
    ARIADNE_TESTING_ADD_TEST(TestThreadManager, test_change_concurrency_and_log_scheduler);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}