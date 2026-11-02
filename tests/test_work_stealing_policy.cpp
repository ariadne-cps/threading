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

    void test_concurrent_submission_and_exact_once() {
        // Concurrent producers exercise admission reservations, queue
        // publication and worker wakeups for the shared scheduler.
        {
            ThreadPool pool(4);
            constexpr int producers=4;
            constexpr int tasks_per_producer=256;
            std::atomic<int> executed{0};
            std::vector<std::thread> submitters;
            submitters.reserve(producers);
            for(int i=0;i<producers;++i) {
                submitters.emplace_back([&pool,&executed] {
                    std::vector<std::future<void>> futures;
                    futures.reserve(tasks_per_producer);
                    for(int j=0;j<tasks_per_producer;++j)
                        futures.push_back(pool.enqueue([&executed] {
                            executed.fetch_add(1,std::memory_order_relaxed);
                        }));
                    for(auto& future:futures)future.get();
                });
            }
            for(auto& producer:submitters)producer.join();
            ARIADNE_TESTING_EXPECT_EQUAL(executed.load(),producers*tasks_per_producer);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_idle_worker_wake_bursts() {
        // Let workers return to idle between short bursts. Each submitted
        // task must complete even when FIFO wakes only one worker.
        {
            ThreadPool pool(4);
            std::atomic<int> executed{0};
            for(int round=0;round<24;++round) {
                std::this_thread::sleep_for(1ms);
                std::vector<std::future<void>> futures;
                futures.reserve(16);
                for(int i=0;i<16;++i) {
                    futures.push_back(pool.enqueue([&executed] {
                        executed.fetch_add(1,std::memory_order_relaxed);
                    }));
                }
                for(auto& future:futures) {
                    ARIADNE_TESTING_EXPECT_READY(future,5s);
                    future.get();
                }
                ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
            }
            ARIADNE_TESTING_EXPECT_EQUAL(executed.load(),24*16);
        }
    }

    void test_external_burst_wakes_all_available_workers() {
        // A burst of independent external tasks must wake enough workers
        // to make progress in parallel, not just one FIFO consumer.
        std::atomic<unsigned> started{0u};
        std::atomic<bool> release{false};
        ThreadPool pool(4);
        std::this_thread::sleep_for(5ms);
        std::vector<std::future<void>> futures;
        futures.reserve(4);
        for(unsigned i=0u;i<4u;++i) {
            futures.push_back(pool.enqueue([&] {
                started.fetch_add(1u,std::memory_order_release);
                while(!release.load(std::memory_order_acquire))
                    std::this_thread::yield();
            }));
        }
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(started.load(std::memory_order_acquire)!=4u &&
              std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        const unsigned concurrent=started.load(std::memory_order_acquire);
        // Always open the gate before checking: failing the assertion must
        // not strand worker tasks or hang destruction of the pool.
        release.store(true,std::memory_order_release);
        for(auto& future:futures) {
            ARIADNE_TESTING_EXPECT_READY(future,10s);
            future.get();
        }
        ARIADNE_TESTING_EXPECT_EQUAL(concurrent,4u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_resize_drains_external_burst() {
        // Shrink waits for zero outstanding activity on an independent epoch.
        // Its wait must not steal the notification intended for a worker.
        std::atomic<bool> release{false};
        std::atomic<unsigned> active{0u};
        std::atomic<unsigned> finished{0u};
        ThreadPool pool(2);
        std::vector<std::future<void>> futures;
        for(unsigned i=0u;i<2u;++i) {
            futures.push_back(pool.enqueue([&] {
                active.fetch_add(1u,std::memory_order_release);
                while(!release.load(std::memory_order_acquire))
                    std::this_thread::yield();
                finished.fetch_add(1u,std::memory_order_relaxed);
            }));
        }
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(active.load(std::memory_order_acquire)!=2u &&
              std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        const unsigned workers_started=active.load(std::memory_order_acquire);
        for(unsigned i=0u;i<48u;++i)
            futures.push_back(pool.enqueue([&] {
                finished.fetch_add(1u,std::memory_order_relaxed);
            }));
        auto shrink=std::async(std::launch::async,[&pool] {
            pool.set_num_threads(1);
        });
        release.store(true,std::memory_order_release);
        ARIADNE_TESTING_EXPECT_READY(shrink,10s);
        shrink.get();
        for(auto& future:futures) {
            ARIADNE_TESTING_EXPECT_READY(future,10s);
            future.get();
        }
        ARIADNE_TESTING_EXPECT_EQUAL(workers_started,2u);
        ARIADNE_TESTING_EXPECT_EQUAL(finished.load(),50u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),size_t{1});
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_global_submission_not_starved_by_local_descendants() {
        // Only one worker can run: it fills its own LIFO with descendants.
        // An external FIFO submission must be chosen before all descendants
        // run, despite the continuously nonempty local queue.
        // Keep captured state alive until the pool has drained all children.
        std::atomic<int> local_executed{0};
        std::atomic<bool> release_parent{false};
        std::promise<void> descendants_ready;
        auto ready=descendants_ready.get_future();
        {
            ThreadPool pool(1);
            auto parent=pool.enqueue([&] {
                for(int i=0;i<48;++i) {
                    pool.enqueue([&] {
                        local_executed.fetch_add(1,std::memory_order_relaxed);
                    });
                }
                descendants_ready.set_value();
                while(!release_parent.load(std::memory_order_acquire))
                    std::this_thread::yield();
            });
            ARIADNE_TESTING_EXPECT_READY(ready,10s);
            auto external=pool.enqueue([&] {
                return local_executed.load(std::memory_order_relaxed);
            });
            release_parent.store(true,std::memory_order_release);
            ARIADNE_TESTING_EXPECT_READY(external,10s);
            const int local_before_external=external.get();
            parent.get();
            // The local preference limit is eight consecutive local picks.
            ARIADNE_TESTING_PROVE(local_before_external<=8);
            // The destructor must drain the remaining descendants, whose
            // futures are intentionally discarded above.
        }
        ARIADNE_TESTING_EXPECT_EQUAL(local_executed.load(),48);
    }

    void test_zero_workers_help() {
        ThreadPool pool(0);
        TaskGroup group(pool);
        auto value = group.fork([] { return 42; });
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{1});
        ARIADNE_TESTING_EXPECT_EQUAL(value.get(),42);
        group.join();
        // Targeted acquisition removes the actual queue entry.
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_worker_executes_external_submission() {
        ThreadPool pool(2);
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
        ThreadPool pool(1);
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
        ThreadPool pool(3);
        TaskGroup group(pool);
        std::vector<TaskHandle<ThreadPool,int>> handles;
        for(int i=0;i<32;++i) handles.push_back(group.fork([i] { return i*i; }));
        int total=0;
        for(auto const& handle:handles) total+=handle.get();
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(total,10416);
    }

    void test_steal_from_busy_worker() {
        ThreadPool pool(2);
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
        ThreadPool pool(2);
        TaskGroup group(pool);
        std::atomic<int> count{0};
        group.fork([&] { ++count; throw std::runtime_error("expected"); });
        group.fork([&] { ++count; });
        ARIADNE_TESTING_EXPECT_EXCEPTION(group.join());
        ARIADNE_TESTING_EXPECT_EQUAL(count.load(),2);
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

    void test_destructor_drains() {
        std::atomic<int> completed{0};
        {
            ThreadPool pool(2);
            for(int i=0;i<12;++i) pool.enqueue([&completed] { ++completed; });
        }
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),12);
    }

    void test_zero_worker_destructor_drains_descendants() {
        // The parent executes only once shutdown has begun. Its child
        // must still be accepted and drained by the scheduler.
        {
            std::atomic<int> completed{0};
            {
                ThreadPool pool(0);
                pool.enqueue([&] {
                    ++completed;
                    pool.enqueue([&completed] { ++completed; });
                });
            }
            ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),2);
        }
    }

    void test_local_helping_while_joining() {
        ThreadPool pool(1);
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
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_global_submission_not_starved_by_local_descendants);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_concurrent_submission_and_exact_once);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_idle_worker_wake_bursts);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_external_burst_wakes_all_available_workers);
    ARIADNE_TESTING_ADD_TEST(TestWorkStealingPolicy,test_resize_drains_external_burst);
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
