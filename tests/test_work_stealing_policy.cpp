/***************************************************************************
 *            test_work_stealing_pool.cpp
 *
 *  Copyright 2026 Luca Geretti
 *
 ****************************************************************************/
/*
 * This file is part of Threading, licensed under GPL-3.0-or-later.
 */
#include "ariadne-testing.hpp"
#include "threading/thread_pool.hpp"
#include "threading/task_group.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Ariadne;
using namespace std::chrono_literals;

class TestWorkStealingPolicy : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_zero_workers_help() {
        ThreadPool pool(0,SchedulingPolicy::WorkStealing);
        TaskGroup group(pool);
        auto value = group.fork([] { return 42; });
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{1});
        ARIADNE_TESTING_EXPECT_EQUAL(value.get(),42);
        group.join();
        // Targeted acquisition removes the actual queue entry.
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_worker_executes_external_submission() {
        ThreadPool pool(2,SchedulingPolicy::WorkStealing);
        std::promise<void> signal;
        auto done = signal.get_future();
        const auto caller = std::this_thread::get_id();
        auto result = pool.enqueue([&signal] {
            auto id = std::this_thread::get_id();
            signal.set_value();
            return id;
        });
        ARIADNE_TESTING_EXPECT_READY(done,10s);
        ARIADNE_TESTING_PROVE(result.get()!=caller);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),size_t{2});
    }

    void test_recursive_fork_join_single_worker() {
        ThreadPool pool(1,SchedulingPolicy::WorkStealing);
        std::function<int(unsigned)> tree;
        tree = [&](unsigned depth) -> int {
            if (depth==0) return 1;
            TaskGroup group(pool);
            auto left = group.fork([&tree,depth] { return tree(depth-1); });
            int right = tree(depth-1);
            int result = left.get()+right;
            group.join();
            return result;
        };
        ARIADNE_TESTING_EXPECT_EQUAL(tree(6),64);
    }

    void test_multiple_forks_preserve_results() {
        ThreadPool pool(3,SchedulingPolicy::WorkStealing);
        TaskGroup group(pool);
        std::vector<TaskHandle<ThreadPool,int>> handles;
        for(int i=0;i<32;++i) handles.push_back(group.fork([i] { return i*i; }));
        int total=0;
        for(auto const& handle:handles) total+=handle.get();
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(total,10416);
    }

    void test_steal_from_busy_worker() {
        ThreadPool pool(2,SchedulingPolicy::WorkStealing);
        std::promise<void> enqueued;
        auto ready = enqueued.get_future();
        std::atomic<int> completed{0};
        auto parent=pool.enqueue([&] {
            TaskGroup children(pool);
            for(int i=0;i<16;++i) children.fork([&completed] { ++completed; });
            enqueued.set_value();
            while(completed.load()==0)std::this_thread::yield();
            children.join();
        });
        ARIADNE_TESTING_EXPECT_READY(ready,10s);
        ARIADNE_TESTING_EXPECT_READY(parent,10s);
        parent.get();
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),16);
    }

    void test_join_waits_for_all_after_exception() {
        ThreadPool pool(2,SchedulingPolicy::WorkStealing);
        TaskGroup group(pool);
        std::atomic<int> count{0};
        group.fork([&] { ++count; throw std::runtime_error("expected"); });
        group.fork([&] { ++count; });
        ARIADNE_TESTING_EXPECT_EXCEPTION(group.join());
        ARIADNE_TESTING_EXPECT_EQUAL(count.load(),2);
    }

    void test_ticket_queue_churn() {
        ThreadPool pool(0,SchedulingPolicy::WorkStealing);
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
        ThreadPool pool(2,SchedulingPolicy::WorkStealing);
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

    void test_destructor_drains() {
        std::atomic<int> completed{0};
        {
            ThreadPool pool(2,SchedulingPolicy::WorkStealing);
            for(int i=0;i<12;++i) pool.enqueue([&completed] { ++completed; });
        }
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),12);
    }

    void test_zero_worker_destructor_drains_descendants() {
        std::atomic<int> completed{0};
        {
            ThreadPool pool(0,SchedulingPolicy::WorkStealing);
            pool.enqueue([&] {
                ++completed;
                pool.enqueue([&completed] { ++completed; });
            });
        }
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),2);
    }

    void test_local_helping_while_joining() {
        ThreadPool pool(1,SchedulingPolicy::WorkStealing);
        auto root=pool.enqueue([&] {
            TaskGroup group(pool);
            auto child=group.fork([] { return 17; });
            int result=child.get();
            group.join();
            return result;
        });
        ARIADNE_TESTING_EXPECT_READY(root,10s);
        ARIADNE_TESTING_EXPECT_EQUAL(root.get(),17);
    }
};

int main(int argc,char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("work_stealing_policy");
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_zero_workers_help);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_worker_executes_external_submission);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_recursive_fork_join_single_worker);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_multiple_forks_preserve_results);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_steal_from_busy_worker);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_join_waits_for_all_after_exception);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_ticket_acquisition_race);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_ticket_queue_churn);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_destructor_drains);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_zero_worker_destructor_drains_descendants);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_local_helping_while_joining);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc,argv);
}
