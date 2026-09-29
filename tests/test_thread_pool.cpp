/***************************************************************************
 *            test_thread_pool.cpp
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
#include "utility/test.hpp"
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

class TestSmartThreadPool {
  public:

    void test_construct_thread_name() const {
        ARIADNE_TEST_EQUALS(construct_thread_name("name",9,9),"name9");
        ARIADNE_TEST_EQUALS(construct_thread_name("name",9,10),"name09");
        ARIADNE_TEST_EQUALS(construct_thread_name("name",10,11),"name10");
    }

    void test_construct() {
        auto max_concurrency = std::thread::hardware_concurrency();
        ThreadPool pool(max_concurrency);
        ARIADNE_TEST_EQUALS(pool.num_threads(),max_concurrency);
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);
    }

    void test_construct_empty() {
        ThreadPool pool(0);
        ARIADNE_TEST_EQUALS(pool.num_threads(),0);
        VoidFunction fn([]{ std::this_thread::sleep_for(100ms); });
        pool.enqueue(fn);
        ARIADNE_TEST_EQUALS(pool.queue_size(),1);
    }

    void test_construct_with_name() {
        ThreadPool pool(1);
        ARIADNE_TEST_EQUALS(pool.name(),THREAD_POOL_DEFAULT_NAME);
        ThreadPool pool2(1,"name");
        ARIADNE_TEST_EQUALS(pool2.name(),"name");
    }

    void test_execute_single() {
        ThreadPool pool(1);
        ARIADNE_TEST_EQUALS(pool.num_threads(),1);
        auto future = pool.enqueue([]{});
        future.get();
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);
    }

    void test_exception() {
        ThreadPool pool(1);
        auto future = pool.enqueue([]{ throw new std::exception(); });
        ARIADNE_TEST_FAIL(future.get());
    }

    void test_destroy_before_completion() {
        ThreadPool pool(1);
        pool.enqueue([]{ std::this_thread::sleep_for(100ms); });
    }

    void test_execute_multiple_sequentially() {
        ThreadPool pool(1);
        ARIADNE_TEST_EQUALS(pool.num_threads(),1);
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);
        std::vector<future<void>> futures;
        for (size_t i=0; i<2; ++i)
            futures.push_back(pool.enqueue([]{}));
        for (auto& future : futures) future.get();
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);
    }

    void test_execute_multiple_concurrently() {
        size_t num_threads = 2;
        ThreadPool pool(num_threads);
        ARIADNE_TEST_EQUALS(pool.num_threads(),2);
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
        while (maximum.load() < num_threads) std::this_thread::yield();
        release = true;
        for (auto& future : futures) future.get();
        ARIADNE_TEST_EQUALS(maximum.load(),num_threads);
    }

    void test_execute_multiple_concurrently_sequentially() {
        size_t num_threads = 2;
        ThreadPool pool(num_threads);
        std::vector<future<void>> futures;
        for (size_t i=0; i<2*num_threads; ++i)
            futures.push_back(pool.enqueue([]{}));
        for (auto& future : futures) future.get();
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);
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
        ARIADNE_TEST_EQUALS(x.load(),2*max_concurrency);
        ARIADNE_TEST_EQUAL(actual_sum,expected_sum);
    }

    void test_set_num_threads_up_statically() const {
        ThreadPool pool(0);
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(1));
        ARIADNE_TEST_EQUALS(pool.num_threads(),1);
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(3));
        ARIADNE_TEST_EQUALS(pool.num_threads(),3);
    }

    void test_set_num_threads_same_statically() const {
        ThreadPool pool(3);
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(3));
        ARIADNE_TEST_EQUALS(pool.num_threads(),3);
    }

    void test_set_num_threads_down_statically() const {
        ThreadPool pool(3);
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(1));
        ARIADNE_TEST_EQUAL(pool.num_threads(),1);
    }

    void test_set_num_threads_up_dynamically() const {
        ThreadPool pool(0);
        auto first = pool.enqueue([]{});
        ARIADNE_TEST_EQUALS(pool.queue_size(),1);
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(1));
        ARIADNE_TEST_EQUALS(pool.num_threads(),1);
        first.get();
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);

        auto second = pool.enqueue([]{});
        auto third = pool.enqueue([]{});
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(3));
        ARIADNE_TEST_EQUALS(pool.num_threads(),3);
        second.get();
        third.get();
    }

    void test_set_num_threads_down_dynamically() const {
        ThreadPool pool(3);
        VoidFunction fn([] { std::this_thread::sleep_for(100ms); });
        std::vector<future<void>> futures;
        for (size_t i=0; i<5; ++i)
            futures.push_back(pool.enqueue(fn));
        ARIADNE_TEST_EXECUTE(pool.set_num_threads(2));
        ARIADNE_TEST_EQUAL(pool.num_threads(),2);
        for (auto& future : futures) future.get();
        ARIADNE_TEST_EQUALS(pool.queue_size(),0);
    }





    void test_shrink_from_worker_is_rejected() {
        ThreadPool pool(2);
        auto future = pool.enqueue([&pool] { pool.set_num_threads(0); });
        ARIADNE_TEST_FAIL(future.get())
        ARIADNE_TEST_EQUALS(pool.num_threads(),2)
    }

    void test_resize_repeatedly() const {
        ThreadPool pool(4);
        std::atomic<size_t> completed = 0;
        for (size_t round=0; round<20; ++round) {
            for (size_t i=0; i<16; ++i)
                pool.enqueue([&completed] { ++completed; });
            pool.set_num_threads(2);
            pool.set_num_threads(4);
        }
        pool.set_num_threads(0);
        ARIADNE_TEST_EQUALS(completed,320)
    }

    void test_set_num_threads_to_zero_dynamically() const {
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
        while (running_started.load() < 3) std::this_thread::yield();

        std::atomic<size_t> queued_started = 0;
        auto queued1 = pool.enqueue([&] { ++queued_started; });
        auto queued2 = pool.enqueue([&] { ++queued_started; });

        auto shrink = std::async(std::launch::async,[&] { pool.set_num_threads(0); });
        release_running = true;
        for (auto& future : running) future.get();
        shrink.get();

        ARIADNE_TEST_EQUAL(pool.num_threads(),0);
        auto started_after_shrink = queued_started.load();
        ARIADNE_TEST_ASSERT(started_after_shrink <= 2)

        pool.set_num_threads(1);
        queued1.get();
        queued2.get();

        ARIADNE_TEST_EQUALS(queued_started.load(),2)
        ARIADNE_TEST_EQUALS(pool.queue_size(),0)
    }

    void test() {
        ARIADNE_TEST_CALL(test_construct_thread_name());
        ARIADNE_TEST_CALL(test_construct());
        ARIADNE_TEST_CALL(test_construct_empty());
        ARIADNE_TEST_CALL(test_construct_with_name());
        ARIADNE_TEST_CALL(test_execute_single());
        ARIADNE_TEST_CALL(test_exception());
        ARIADNE_TEST_CALL(test_destroy_before_completion());
        ARIADNE_TEST_CALL(test_execute_multiple_sequentially());
        ARIADNE_TEST_CALL(test_execute_multiple_concurrently());
        ARIADNE_TEST_CALL(test_execute_multiple_concurrently_sequentially());
        ARIADNE_TEST_CALL(test_process_on_atomic_type());
        ARIADNE_TEST_CALL(test_set_num_threads_up_statically());
        ARIADNE_TEST_CALL(test_set_num_threads_same_statically());
        ARIADNE_TEST_CALL(test_set_num_threads_down_statically());
        ARIADNE_TEST_CALL(test_set_num_threads_up_dynamically());
        ARIADNE_TEST_CALL(test_set_num_threads_down_dynamically());
        ARIADNE_TEST_CALL(test_shrink_from_worker_is_rejected());
        ARIADNE_TEST_CALL(test_resize_repeatedly());
        ARIADNE_TEST_CALL(test_set_num_threads_to_zero_dynamically());
    }
};

int main() {
    ThreadRegistry registry;
    Ariadne::Logger::instance().attach_thread_registry(&registry);
    TestSmartThreadPool().test();
    return ARIADNE_TEST_FAILURES;
}
