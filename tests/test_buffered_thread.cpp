/***************************************************************************
 *            test_buffered_thread.cpp
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

#include "ariadne-testing.hpp"
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
        BufferedThread thread;
        thread.enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
    }

    void test_exception() {
        BufferedThread thread;
        auto future = thread.enqueue([] { throw std::exception(); });
        ARIADNE_TESTING_EXPECT_EXCEPTION(future.get());
    }

    void test_has_queued_tasks() {
        BufferedThread thread;
        thread.set_queue_capacity(2);
        thread.enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
        thread.enqueue([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
        ARIADNE_TESTING_PROVE(thread.queue_size()>0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ARIADNE_TESTING_EXPECT_EQUAL(thread.queue_size(),0);
    }

    void test_set_queue_capacity_down_failure() {
        BufferedThread thread;
        thread.set_queue_capacity(3);
        auto fn([]{ std::this_thread::sleep_for(std::chrono::milliseconds(100)); });
        thread.enqueue(fn);
        thread.enqueue(fn);
        thread.enqueue(fn);
        ARIADNE_TESTING_EXPECT_EXCEPTION(thread.set_queue_capacity(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ARIADNE_TESTING_EXPECT_NO_THROW(thread.set_queue_capacity(1));
    }

    void test_task_return() {
        BufferedThread thread;
        auto result = thread.enqueue([] { return 42; });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ARIADNE_TESTING_EXPECT_EQUAL(a.load(),n_threads);
        threads.clear();
    }

    static void add_cases(Ariadne::Testing::Suite& suite) {
        suite.add("test_create", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_create();
        });
        suite.add("test_set_queue_capacity", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_set_queue_capacity();
        });
        suite.add("test_destroy_before_completion", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_destroy_before_completion();
        });
        suite.add("test_exception", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_exception();
        });
        suite.add("test_has_queued_tasks", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_has_queued_tasks();
        });
        suite.add("test_set_queue_capacity_down_failure", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_set_queue_capacity_down_failure();
        });
        suite.add("test_task_return", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_task_return();
        });
        suite.add("test_task_capture", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_task_capture();
        });
        suite.add("test_task_arguments", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_task_arguments();
        });
        suite.add("test_multiple_tasks", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_multiple_tasks();
        });
        suite.add("test_atomic_multiple_threads", [](Ariadne::Testing::Context& context) {
            TestBufferedThread fixture(context);
            fixture.test_atomic_multiple_threads();
        });
    }

};

int main(int argc, char** argv) {
    ThreadRegistry registry;
    Ariadne::Logger::instance().attach_thread_registry(&registry);
    Ariadne::Testing::Suite suite("buffered_thread");
    TestBufferedThread::add_cases(suite);
    return Ariadne::Testing::run_suite(suite, argc, argv);
}