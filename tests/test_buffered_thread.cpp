/***************************************************************************
 *            test_buffered_thread.cpp
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

#include "ariadne-testing.hpp"
#include <future>
#include "utility/container.hpp"
#include "logging/logging.hpp"
#include "logging/thread_registry_interface.hpp"
#include "threading/buffered_thread.hpp"

using namespace Ariadne;

class ThreadRegistry : public Ariadne::ThreadRegistryInterface {
public:
    ThreadRegistry() : _threads_registered(0) { }
    bool has_threads_registered() const override { return _threads_registered > 0; }
    void set_threads_registered(unsigned int threads_registered) { _threads_registered = threads_registered; }
private:
    unsigned int _threads_registered;
};

class TestBufferedThread : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:

    void test_create() {
        BufferedThread thread1("thr");
        ARIADNE_TESTING_EXPECT_NO_THROW(thread1.id());
        ARIADNE_TESTING_EXPECT_EQUAL(thread1.name(),"thr");
        ARIADNE_TESTING_EXPECT_EQUAL(thread1.queue_size(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(thread1.queue_capacity(),1);
        BufferedThread thread2;
        ARIADNE_TESTING_EXPECT_EQUAL(to_string(thread2.id()),thread2.name());
    }

    void test_set_queue_capacity() {
        BufferedThread thread;
        ARIADNE_TESTING_EXPECT_EXCEPTION(thread.set_queue_capacity(0));
        ARIADNE_TESTING_EXPECT_NO_THROW(thread.set_queue_capacity(2));
        ARIADNE_TESTING_EXPECT_NO_THROW(thread.set_queue_capacity(1));
    }

    void test_destroy_before_completion() {
        std::atomic<bool> completed{false};
        {
            BufferedThread thread;
            thread.enqueue([&completed] {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                completed.store(true);
            });
        } // Queued task must complete before destruction returns.
        ARIADNE_TESTING_PROVE(completed.load());
    }

    void test_exception() {
        BufferedThread thread;
        auto future = thread.enqueue([] { throw std::exception(); });
        ARIADNE_TESTING_EXPECT_EXCEPTION(future.get());
    }

    void test_has_queued_tasks() {
        BufferedThread thread;
        thread.set_queue_capacity(2);
        // The gate ensures the second task is queued before the check.
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto first = thread.enqueue([gate] { gate.wait(); });
        auto second = thread.enqueue([gate] { gate.wait(); });
        ARIADNE_TESTING_PROVE(thread.queue_size()>0);
        release.set_value();
        ARIADNE_TESTING_EXPECT_READY(second,std::chrono::seconds(10));
        first.get();
        second.get();
        ARIADNE_TESTING_EXPECT_EQUAL(thread.queue_size(),0);
    }

    void test_set_queue_capacity_down_failure() {
        BufferedThread thread;
        thread.set_queue_capacity(3);
        std::promise<void> release;
        auto gate = release.get_future().share();
        auto fn = [gate] { gate.wait(); };
        auto first = thread.enqueue(fn);
        auto second = thread.enqueue(fn);
        auto third = thread.enqueue(fn);
        ARIADNE_TESTING_EXPECT_EXCEPTION(thread.set_queue_capacity(1));
        release.set_value();
        ARIADNE_TESTING_EXPECT_READY(third,std::chrono::seconds(10));
        first.get();
        second.get();
        third.get();
        ARIADNE_TESTING_EXPECT_NO_THROW(thread.set_queue_capacity(1));
    }

    void test_task_return() {
        BufferedThread thread;
        auto result = thread.enqueue([] { return 42; });
        ARIADNE_TESTING_EXPECT_READY(result,std::chrono::seconds(10));
        ARIADNE_TESTING_EXPECT_EQUAL(result.get(),42);
    }

    void test_task_capture() {
        int a = 0;
        BufferedThread thread;
        auto future = thread.enqueue([&a] { a++; });
        future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(a,1);
    }

    void test_task_arguments() {
        int x = 3;
        int y = 5;
        BufferedThread thread;
        auto future = thread.enqueue([](int a, int b) { return a * b; }, x, y);
        ARIADNE_TESTING_EXPECT_READY(future,std::chrono::seconds(10));
        auto r = future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(r,15);
    }

    void test_multiple_tasks() {
        BufferedThread thread;
        int a = 4;
        thread.enqueue([&a] {
            a += 2;
            return a;
        });
        auto future = thread.enqueue([&a] {
            a *= 7;
            return a;
        });
        int r = future.get();
        ARIADNE_TESTING_EXPECT_EQUAL(r,42);
    }

    void test_atomic_multiple_threads() {
        size_t n_threads = 10*std::thread::hardware_concurrency();
        ARIADNE_TESTING_INSPECT("n_threads",n_threads);
        List<shared_ptr<BufferedThread>> threads;

        std::atomic<size_t> a = 0;
        for (size_t i=0; i<n_threads; ++i) {
            threads.push_back(make_shared<BufferedThread>(("add" + to_string(i))));
            threads.at(i)->enqueue([&a] { a++; });
        }

        ARIADNE_TESTING_EXPECT_ATOMIC(a,
            [n_threads](size_t observed) { return observed == n_threads; },
            std::chrono::seconds(10));
        ARIADNE_TESTING_EXPECT_EQUAL(a.load(),n_threads);
        threads.clear();
    }


};

int main(int argc, char** argv) {
    ThreadRegistry registry;
    Ariadne::Logger::instance().attach_thread_registry(&registry);
    ARIADNE_TESTING_SUITE("buffered_thread");
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_create);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_set_queue_capacity);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_destroy_before_completion);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_exception);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_has_queued_tasks);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_set_queue_capacity_down_failure);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_task_return);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_task_capture);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_task_arguments);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_multiple_tasks);
    ARIADNE_TESTING_ADD_TEST(TestBufferedThread, test_atomic_multiple_threads);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}