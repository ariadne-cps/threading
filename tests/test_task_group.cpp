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
#include <memory>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

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
        // Targeted acquisition removes the actual queue entry.
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

    void test_deep_recursive_fork_join() {
        // The targeted fork/join wait must not build a stack of unrelated
        // subtrees. Previously some schedulers crashed around depth 14.
        for (unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            std::function<int(unsigned)> compute;
            compute = [&](unsigned depth) -> int {
                if (depth == 0u) return 1;
                TaskGroup group(pool);
                auto first = group.fork([&compute,depth] { return compute(depth-1u); });
                int other = compute(depth-1u);
                const int result = first.get()+other;
                group.join();
                return result;
            };
            ARIADNE_TESTING_EXPECT_EQUAL(compute(14u),16384);
        }
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

    void test_one_child_repeated_join_and_refork() {
        ThreadPool pool(0);
        TaskGroup group(pool);
        for(int i=0;i<16;++i) {
            auto handle=group.fork([i] { return i*i; });
            ARIADNE_TESTING_EXPECT_EQUAL(handle.get(),i*i);
            group.join();
            // Re-joining an empty group must not repeat the task.
            group.join();
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_multiple_forks_first_exception_order() {
        ThreadPool pool(0);
        std::atomic<int> completed{0};
        TaskGroup group(pool);
        group.fork([&completed] {
            ++completed;
            throw std::runtime_error("first");
        });
        group.fork([&completed] {
            ++completed;
            throw std::runtime_error("second");
        });
        auto last=group.fork([&completed] {
            ++completed;
            return 99;
        });
        bool first_observed=false;
        try {
            group.join();
        } catch (std::runtime_error const& error) {
            first_observed=std::string(error.what())=="first";
        }
        ARIADNE_TESTING_PROVE(first_observed);
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),3);
        ARIADNE_TESTING_EXPECT_EQUAL(last.get(),99);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        // Joining after an exception must be a no-op.
        group.join();
    }

    void test_multiple_forks_destruction_drains() {
        ThreadPool pool(0);
        std::atomic<int> completed{0};
        {
            TaskGroup group(pool);
            for(int i=0;i<4;++i)
                group.fork([&completed] { ++completed; });
        }
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),4);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_ticket_queue_churn() {
        ThreadPool pool(0);
        std::atomic<int> executed{0};
        std::vector<ThreadPool::Ticket> tickets;
        tickets.reserve(1024);
        for (size_t i=0;i<1024;++i) {
            tickets.push_back(pool.enqueue_ticket([&executed] { ++executed; }));
        }
        // Remove alternating tickets out of FIFO order, forcing iterator
        // maintenance while other tickets remain in the queue.
        for (size_t i=1;i<tickets.size();i+=2)
            ARIADNE_TESTING_PROVE(pool.try_run_task(tickets[i]));
        for (size_t i=0;i<tickets.size();i+=2)
            ARIADNE_TESTING_PROVE(pool.try_run_task(tickets[i]));
        ARIADNE_TESTING_EXPECT_EQUAL(executed.load(),1024);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        for (ThreadPool::Ticket const& ticket : tickets)
            ARIADNE_TESTING_PROVE(!pool.try_run_task(ticket));
    }

    void test_ticket_acquisition_race() {
        ThreadPool pool(2);
        std::atomic<int> executions{0};
        for (unsigned i=0; i<256; ++i) {
            TaskGroup group(pool);
            auto child=group.fork([&executions] {
                return executions.fetch_add(1)+1;
            });
            ARIADNE_TESTING_EXPECT_EQUAL(child.get(),static_cast<int>(i+1));
            group.join();
        }
        ARIADNE_TESTING_EXPECT_EQUAL(executions.load(),256);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
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

    void test_explicit_direct_ticket_handle() {
        // Direct handles remain available for experimentation even though
        // TaskGroup uses the callback representation by default.
        for(unsigned workers : {0u,1u,2u}) {
            ThreadPool pool(workers);
            std::atomic<unsigned> count{0u};
            auto task=std::make_shared<std::packaged_task<int()>>([&count] {
                count.fetch_add(1u,std::memory_order_relaxed);
                return 77;
            });
            auto future=task->get_future().share();
            auto ticket=pool.enqueue_ticket([task] { (*task)(); });
            TaskHandle<ThreadPool,int> handle(
                pool,std::move(future),std::move(ticket));
            ARIADNE_TESTING_EXPECT_EQUAL(handle.get(),77);
            ARIADNE_TESTING_EXPECT_EQUAL(count.load(),1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_generic_future_helping_is_preserved() {
        // The public generic constructor still executes pending scheduler
        // work while waiting, without requiring a targeted ticket.
        ThreadPool pool(0);
        std::promise<int> promise;
        auto result=promise.get_future().share();
        auto worker=pool.enqueue([&promise] { promise.set_value(37); });
        TaskHandle<ThreadPool,int> handle(pool,std::move(result));
        ARIADNE_TESTING_EXPECT_EQUAL(handle.get(),37);
        worker.get();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
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

};

int main(int argc, char** argv) {
    // ThreadPool workers register with Logger. ThreadManager installs
    // the process-wide registry before the first worker is constructed.
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("task_group");
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_zero_worker_helping);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_nested_single_worker_fork_join);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_deep_recursive_fork_join);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_parallel_forks);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_join_drains_after_exception);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_one_child_repeated_join_and_refork);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_multiple_forks_first_exception_order);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_multiple_forks_destruction_drains);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_ticket_acquisition_race);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_ticket_queue_churn);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_destructor_joins);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_void_handle);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_explicit_direct_ticket_handle);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_generic_future_helping_is_preserved);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_thread_manager_sequential_mode);
    ARIADNE_TESTING_ADD_TEST(TestTaskGroup, test_thread_manager_worker_mode);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}
