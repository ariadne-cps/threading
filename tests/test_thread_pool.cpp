/***************************************************************************
 *            test_thread_pool.cpp
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
#include "ariadne-testing.hpp"
#include "logging/logging.hpp"
#include "logging/thread_registry_interface.hpp"
#include "threading/thread_pool.hpp"

using namespace Ariadne;

using namespace std::chrono_literals;

class ThreadRegistry : public Ariadne::ThreadRegistryInterface {
public:
    ThreadRegistry() : _threads_registered(0) { }
    bool has_threads_registered() const override { return _threads_registered > 0; }
    void set_threads_registered(unsigned int threads_registered) { _threads_registered = threads_registered; }
private:
    unsigned int _threads_registered;
};

class TestSmartThreadPool : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:

    void test_construct_thread_name() {
        ARIADNE_TESTING_EXPECT_EQUAL(construct_thread_name("name",9,9),"name9");
        ARIADNE_TESTING_EXPECT_EQUAL(construct_thread_name("name",9,10),"name09");
        ARIADNE_TESTING_EXPECT_EQUAL(construct_thread_name("name",10,11),"name10");
    }

    void test_construct() {
        auto max_concurrency = std::thread::hardware_concurrency();
        ThreadPool pool(max_concurrency);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),max_concurrency);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
    }

    void test_construct_empty() {
        ThreadPool pool(0);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),0);
        VoidFunction fn([]{ std::this_thread::sleep_for(100ms); });
        pool.enqueue(fn);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),1);
    }

    void test_construct_with_name() {
        ThreadPool pool(1);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.name(),THREAD_POOL_DEFAULT_NAME);
        ThreadPool pool2(1,"name");
        ARIADNE_TESTING_EXPECT_EQUAL(pool2.name(),"name");
    }

    void test_execute_single() {
        ThreadPool pool(1);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),1);
        auto future = pool.enqueue([]{});
        future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
    }

    void test_exception() {
        ThreadPool pool(1);
        auto future = pool.enqueue([]{ throw std::exception(); });
        ARIADNE_TESTING_EXPECT_EXCEPTION(future.get());
    }

    void test_destroy_before_completion() {
        std::atomic<bool> completed{false};
        {
            ThreadPool pool(1);
            pool.enqueue([&completed] {
                std::this_thread::sleep_for(100ms);
                completed.store(true);
            });
        } // Pool destruction drains the task and joins its worker.
        ARIADNE_TESTING_PROVE(completed.load());
    }

    void test_execute_multiple_sequentially() {
        ThreadPool pool(1);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),1);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
        std::vector<future<void>> futures;
        for (size_t i=0; i<2; ++i)
            futures.push_back(pool.enqueue([]{}));
        for (auto& future : futures) future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
    }

    void test_execute_multiple_concurrently() {
        size_t num_threads = 2;
        ThreadPool pool(num_threads);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),2);
        std::atomic<size_t> active = 0;
        std::atomic<size_t> maximum = 0;
        std::atomic<bool> release = false;
        std::vector<future<void>> futures;
        for (size_t i=0; i<num_threads; ++i) {
            futures.push_back(pool.enqueue([&] {
                auto current = ++active;
                auto observed = maximum.load();
                while (current > observed and not maximum.compare_exchange_weak(observed,current)) { }
                while (not release.load()) std::this_thread::yield();
                --active;
            }));
        }
        ARIADNE_TESTING_EXPECT_ATOMIC(maximum,
            [num_threads](size_t observed) { return observed >= num_threads; },5s);
        release = true;
        for (auto& future : futures) future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(maximum.load(),num_threads);
    }

    void test_execute_multiple_concurrently_sequentially() {
        size_t num_threads = 2;
        ThreadPool pool(num_threads);
        std::vector<future<void>> futures;
        for (size_t i=0; i<2*num_threads; ++i)
            futures.push_back(pool.enqueue([]{}));
        for (auto& future : futures) future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
    }

    void test_process_on_atomic_type() {
        auto max_concurrency = std::thread::hardware_concurrency();
        ThreadPool pool(max_concurrency);
        std::vector<future<size_t>> results;
        std::atomic<size_t> x = 0;

        for (size_t i = 0; i < 2 * max_concurrency; ++i) {
            results.emplace_back(pool.enqueue([&x] {
                size_t r = ++x;
                return r * r;
            }));
        }

        size_t actual_sum = 0, expected_sum = 0;
        for (size_t i = 0; i < 2 * max_concurrency; ++i) {
            actual_sum += results[i].get();
            expected_sum += (i+1)*(i+1);
        }
        ARIADNE_TESTING_EXPECT_EQUAL(x.load(),2*max_concurrency);
        ARIADNE_TESTING_EXPECT_EQUAL(actual_sum,expected_sum);
    }

    void test_set_num_threads_up_statically() {
        ThreadPool pool(0);
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(1));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),1);
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(3));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),3);
    }

    void test_set_num_threads_same_statically() {
        ThreadPool pool(3);
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(3));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),3);
    }

    void test_set_num_threads_down_statically() {
        ThreadPool pool(3);
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(1));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),1);
    }

    void test_set_num_threads_up_dynamically() {
        ThreadPool pool(0);
        auto first = pool.enqueue([]{});
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),1);
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(1));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),1);
        first.get();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);

        auto second = pool.enqueue([]{});
        auto third = pool.enqueue([]{});
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(3));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),3);
        second.get();
        third.get();
    }

    void test_set_num_threads_down_dynamically() {
        ThreadPool pool(3);
        VoidFunction fn([] { std::this_thread::sleep_for(100ms); });
        std::vector<future<void>> futures;
        for (size_t i=0; i<5; ++i)
            futures.push_back(pool.enqueue(fn));
        ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(2));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),2);
        for (auto& future : futures) future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
    }





    void test_shrink_from_worker_is_rejected() {
        ThreadPool pool(2);
        auto future = pool.enqueue([&pool] { pool.set_num_threads(0); });
        ARIADNE_TESTING_EXPECT_EXCEPTION(future.get());
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),2);
    }

    void test_resize_repeatedly() {
        ThreadPool pool(4);
        std::atomic<size_t> completed = 0;
        for (size_t round=0; round<20; ++round) {
            for (size_t i=0; i<16; ++i)
                pool.enqueue([&completed] { ++completed; });
            pool.set_num_threads(2);
            pool.set_num_threads(4);
        }
        pool.set_num_threads(0);
        ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),320);
    }

    void test_set_num_threads_to_zero_dynamically() {
        ThreadPool pool(3);
        std::atomic<size_t> running_started = 0;
        std::atomic<bool> release_running = false;
        std::vector<future<void>> running;

        for (size_t i=0; i<3; ++i) {
            running.push_back(pool.enqueue([&] {
                ++running_started;
                while (not release_running.load()) std::this_thread::yield();
            }));
        }
        ARIADNE_TESTING_EXPECT_ATOMIC(running_started,
            [](size_t observed) { return observed == 3; },5s);

        std::atomic<size_t> queued_started = 0;
        auto queued1 = pool.enqueue([&] { ++queued_started; });
        auto queued2 = pool.enqueue([&] { ++queued_started; });

        auto shrink = std::async(std::launch::async,[&] { pool.set_num_threads(0); });
        release_running = true;
        for (auto& future : running) future.get();
        shrink.get();

        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),0);
        // All two tasks must have either completed or remained queued.
        auto started_after_shrink = queued_started.load();
        ARIADNE_TESTING_EXPECT_EQUAL(started_after_shrink + pool.queue_size(),std::size_t{2});

        pool.set_num_threads(1);
        queued1.get();
        queued2.get();

        ARIADNE_TESTING_EXPECT_EQUAL(queued_started.load(),2);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0);
    }

    void test_enqueue_to_stopping_pool_throws() {
        // Once destruction has begun, an external thread must observe
        // StoppedThreadPoolException instead of queueing more work, while
        // the running task is drained and workers exit cleanly.
        auto* pool=new ThreadPool(2u);
        std::atomic<bool> task_started{false};
        std::atomic<bool> release{false};
        std::atomic<bool> got_exception{false};
        auto future=pool->enqueue([&] {
            task_started.store(true);
            while(!release.load())std::this_thread::yield();
        });
        while(!task_started.load())std::this_thread::yield();
        // The pool outlives the spammer: destruction joins the worker that
        // runs the task, and the task is released only after spammer.join().
        std::thread destroyer([&] { delete pool; });
        std::thread spammer([&] {
            for(unsigned i=0u;!got_exception.load() && i<1000000u;++i) {
                try {
                    pool->enqueue_ticket([] {});
                } catch(StoppedThreadPoolException const&) {
                    got_exception.store(true);
                }
            }
        });
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while(!got_exception.load() && std::chrono::steady_clock::now()<deadline)
            std::this_thread::yield();
        spammer.join();
        release.store(true);
        future.get();
        destroyer.join();
        ARIADNE_TESTING_PROVE(got_exception.load());
    }

    void test_shrink_drains_queued_work() {
        ThreadPool pool(2u);
        for(unsigned round=0u;round<200u;++round) {
            std::vector<future<void>> futures;
            for(unsigned i=0u;i<8u;++i)
                futures.push_back(pool.enqueue([] { std::this_thread::yield(); }));
            ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(1u));
            ARIADNE_TESTING_EXPECT_NO_THROW(pool.set_num_threads(2u));
            for(auto& future:futures)future.get();
        }
        ARIADNE_TESTING_EXPECT_EQUAL(pool.num_threads(),2u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),0u);
    }

};

int main(int argc, char** argv) {
    ThreadRegistry registry;
    Ariadne::Logger::instance().attach_thread_registry(&registry);
    ARIADNE_TESTING_SUITE("thread_pool");
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_construct_thread_name);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_construct);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_construct_empty);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_construct_with_name);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_execute_single);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_exception);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_destroy_before_completion);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_execute_multiple_sequentially);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_execute_multiple_concurrently);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_execute_multiple_concurrently_sequentially);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_process_on_atomic_type);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_set_num_threads_up_statically);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_set_num_threads_same_statically);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_set_num_threads_down_statically);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_set_num_threads_up_dynamically);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_set_num_threads_down_dynamically);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_shrink_from_worker_is_rejected);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_resize_repeatedly);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_set_num_threads_to_zero_dynamically);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_enqueue_to_stopping_pool_throws);
    ARIADNE_TESTING_ADD_TEST(TestSmartThreadPool, test_shrink_drains_queued_work);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}