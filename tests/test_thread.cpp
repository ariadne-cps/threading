/***************************************************************************
 *            test_thread.cpp
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
#include "ariadne-testing.hpp"
#include "utility/container.hpp"
#include "logging/logging.hpp"
#include "logging/thread_registry_interface.hpp"
#include "threading/thread.hpp"
#include "threading/using.hpp"

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

class TestThread : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:

    void test_create() const {
        Thread thread1([]{}, "thr");
        ARIADNE_TESTING_EXPECT_NO_THROW(thread1.id());
        ARIADNE_TESTING_EXPECT_EQUAL(thread1.name(),"thr");
        Thread thread2([]{});
        ARIADNE_TESTING_EXPECT_EQUAL(to_string(thread2.id()),thread2.name());
    }

    void test_destroy_before_completion() const {
        Thread thread([] { std::this_thread::sleep_for(100ms); },"");
    }

    void test_task() const {
        std::atomic<int> a = 0;
        Thread thread([&a] { a++; });
        std::this_thread::sleep_for(10ms);
        ARIADNE_TESTING_EXPECT_EQUAL(a.load(),1);
        ARIADNE_TESTING_PROVE(thread.exception() == nullptr);
    }

    void test_exception() const {
        Thread thread([] { throw std::exception(); });
        std::this_thread::sleep_for(10ms);
        ARIADNE_TESTING_PROVE(thread.exception() != nullptr);
    }



    void test_destroy_inactive() const {
        std::atomic<size_t> executions = 0;
        {
            Thread thread([&executions] { ++executions; },"inactive",false);
        }
        ARIADNE_TESTING_EXPECT_EQUAL(executions.load(),0);
    }

    void test_concurrent_activate() const {
        std::atomic<size_t> executions = 0;
        Thread thread([&executions] { ++executions; },"inactive",false);
        Thread first([&thread] { thread.activate(); },"activator1");
        Thread second([&thread] { thread.activate(); },"activator2");
        std::this_thread::sleep_for(10ms);
        ARIADNE_TESTING_EXPECT_EQUAL(executions,1);
    }

    void test_atomic_multiple_threads() const {
        size_t n_threads = 10*std::thread::hardware_concurrency();
        ARIADNE_TESTING_INSPECT("n_threads",n_threads);
        List<shared_ptr<Thread>> threads;

        std::atomic<size_t> a = 0;
        for (size_t i=0; i<n_threads; ++i) {
            threads.push_back(std::make_shared<Thread>([&a] { a++; }));
        }

        std::this_thread::sleep_for(100ms);
        ARIADNE_TESTING_EXPECT_EQUAL(a,n_threads);
        threads.clear();
    }

    static void add_cases(Ariadne::Testing::Suite& suite) {
        suite.add("test_create", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_create();
        });
        suite.add("test_destroy_before_completion", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_destroy_before_completion();
        });
        suite.add("test_task", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_task();
        });
        suite.add("test_exception", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_exception();
        });
        suite.add("test_destroy_inactive", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_destroy_inactive();
        });
        suite.add("test_concurrent_activate", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_concurrent_activate();
        });
        suite.add("test_atomic_multiple_threads", [](Ariadne::Testing::Context& context) {
            TestThread fixture(context);
            fixture.test_atomic_multiple_threads();
        });
    }

};

int main(int argc, char** argv) {
    ThreadRegistry registry;
    Ariadne::Logger::instance().attach_thread_registry(&registry);
    Ariadne::Testing::Suite suite("thread");
    TestThread::add_cases(suite);
    return Ariadne::Testing::run_suite(suite, argc, argv);
}