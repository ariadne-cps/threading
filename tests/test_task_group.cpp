/***************************************************************************
 *            test_task_group.cpp
 *
 *  Copyright  2026  Luca Geretti
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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#include "ariadne-testing.hpp"
#include "threading/task_group.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

using namespace Ariadne;

class TestTaskGroup : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_zero_worker_helping() {
        ThreadPool pool(0);
        TaskGroup group(pool);
        auto result = group.fork([] { return 21; });
        ARIADNE_TESTING_EXPECT_EQUAL(result.get(),21);
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_nested_single_worker_fork_join() {
        ThreadPool pool(1);
        std::function<int(unsigned)> compute;
        compute = [&](unsigned depth) -> int {
            if (depth == 0) return 1;
            TaskGroup group(pool);
            auto first = group.fork([&compute,depth] { return compute(depth-1); });
            auto other = compute(depth-1);
            auto total = first.get()+other;
            group.join();
            return total;
        };
        // A blocking future.get() in the only worker would deadlock here.
        ARIADNE_TESTING_EXPECT_EQUAL(compute(5),32);
    }

    void test_parallel_forks() {
        ThreadPool pool(2);
        std::atomic<int> active{0};
        TaskGroup group(pool);
        auto work = [&] {
            active.fetch_add(1);
            while (active.load() < 2) std::this_thread::yield();
            return 1;
        };
        auto first = group.fork(work);
        auto second = group.fork(work);
        ARIADNE_TESTING_EXPECT_EQUAL(first.get()+second.get(),2);
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(active.load(),2);
    }

    void test_join_drains_after_exception() {
        ThreadPool pool(1);
        std::atomic<int> finished{0};
        TaskGroup group(pool);
        group.fork([&] { ++finished; throw std::runtime_error("expected"); });
        group.fork([&] { ++finished; });
        ARIADNE_TESTING_EXPECT_EXCEPTION(group.join());
        ARIADNE_TESTING_EXPECT_EQUAL(finished.load(),2);
    }

    void test_destructor_joins() {
        ThreadPool pool(1);
        std::atomic<bool> completed{false};
        {
            TaskGroup group(pool);
            group.fork([&] { completed = true; });
        }
        ARIADNE_TESTING_PROVE(completed.load());
    }

    void test_void_handle() {
        ThreadPool pool(0);
        TaskGroup group(pool);
        std::atomic<int> count{0};
        auto task = group.fork([&] { ++count; });
        task.get();
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(count.load(),1);
    }

    void test_thread_manager_sequential_mode() {
        auto& manager = ThreadManager::instance();
        manager.set_concurrency(0);
        auto calling_thread = std::this_thread::get_id();
        TaskGroup group(manager);
        auto result = group.fork([] { return std::this_thread::get_id(); });
        ARIADNE_TESTING_PROVE(result.get()==calling_thread);
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(manager.concurrency(),size_t{0});
    }

    void test_thread_manager_worker_mode() {
        auto& manager = ThreadManager::instance();
        if (manager.maximum_concurrency() == 0) {
            ctx().skip("requires at least one hardware thread");
            return;
        }
        manager.set_concurrency(1);
        auto calling_thread = std::this_thread::get_id();
        std::atomic<bool> started{false};
        {
            TaskGroup group(manager);
            auto result = group.fork([&started] {
                auto worker_id = std::this_thread::get_id();
                started.store(true);
                return worker_id;
            });
            // Observe asynchronous start before helping: this guarantees
            // that the task was actually dispatched to a worker.
            ARIADNE_TESTING_EXPECT_ATOMIC(started,
                [](bool value) { return value; },std::chrono::seconds(10));
            ARIADNE_TESTING_PROVE(result.get()!=calling_thread);
            group.join();
        }
        manager.set_concurrency(0);
    }

    static void add_cases(Ariadne::Testing::Suite& suite) {
        suite.add("zero_worker_helping", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_zero_worker_helping();
        });
        suite.add("nested_single_worker_fork_join", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_nested_single_worker_fork_join();
        });
        suite.add("parallel_forks", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_parallel_forks();
        });
        suite.add("join_drains_after_exception", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_join_drains_after_exception();
        });
        suite.add("destructor_joins", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_destructor_joins();
        });
        suite.add("void_handle", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_void_handle();
        });
        suite.add("thread_manager_sequential_mode", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_thread_manager_sequential_mode();
        });
        suite.add("thread_manager_worker_mode", [](Ariadne::Testing::Context& ctx) {
            TestTaskGroup fixture(ctx); fixture.test_thread_manager_worker_mode();
        });
    }
};

int main(int argc, char** argv) {
    Ariadne::Testing::Suite suite("task_group");
    TestTaskGroup::add_cases(suite);
    return Ariadne::Testing::run_suite(suite,argc,argv);
}
