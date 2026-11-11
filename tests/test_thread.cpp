/***************************************************************************
 *            test_thread.cpp
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
#include <future>
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

    void test_create() {
        Thread thread1([]{}, "thr");
        ARIADNE_TESTING_EXPECT_NO_THROW(thread1.id());
        ARIADNE_TESTING_EXPECT_EQUAL(thread1.name(),"thr");
        Thread thread2([]{});
        ARIADNE_TESTING_EXPECT_EQUAL(to_string(thread2.id()),thread2.name());
    }

    void test_destroy_before_completion() {
        std::atomic<bool> completed{false};
        {
            Thread thread([&completed] {
                std::this_thread::sleep_for(100ms);
                completed.store(true);
            },"");
        } // Destructor must join the task.
        ARIADNE_TESTING_PROVE(completed.load());
    }

    void test_task() {
        std::atomic<int> a = 0;
        Thread thread([&a] { a++; });
        ARIADNE_TESTING_EXPECT_ATOMIC(a,[](int observed) { return observed == 1; },10s);
        ARIADNE_TESTING_EXPECT_EQUAL(a.load(),1);
        ARIADNE_TESTING_PROVE(thread.exception() == nullptr);
    }

    void test_exception() {
        std::promise<void> finished;
        auto ready = finished.get_future();
        Thread thread([&finished] {
            // The signal becomes ready when the worker actually exits.
            finished.set_value_at_thread_exit();
            throw std::exception();
        });
        ARIADNE_TESTING_EXPECT_READY(ready,10s);
        ARIADNE_TESTING_PROVE(thread.exception() != nullptr);
    }



    void test_destroy_inactive() {
        std::atomic<size_t> executions = 0;
        {
            Thread thread([&executions] { ++executions; },"inactive",false);
        }
        ARIADNE_TESTING_EXPECT_EQUAL(executions.load(),0);
    }

    void test_concurrent_activate() {
        std::atomic<size_t> executions = 0;
        Thread thread([&executions] { ++executions; },"inactive",false);
        {
            Thread first([&thread] { thread.activate(); },"activator1");
            Thread second([&thread] { thread.activate(); },"activator2");
            ARIADNE_TESTING_EXPECT_ATOMIC(executions,
                [](size_t observed) { return observed == 1; },10s);
        } // Join both activators before the exact-count assertion.
        ARIADNE_TESTING_EXPECT_EQUAL(executions.load(),1);
    }

    void test_atomic_multiple_threads() {
        size_t n_threads = 10*std::thread::hardware_concurrency();
        ARIADNE_TESTING_INSPECT("n_threads",n_threads);
        List<shared_ptr<Thread>> threads;

        std::atomic<size_t> a = 0;
        for (size_t i=0; i<n_threads; ++i) {
            threads.push_back(std::make_shared<Thread>([&a] { a++; }));
        }

        ARIADNE_TESTING_EXPECT_ATOMIC(a,
            [n_threads](size_t observed) { return observed == n_threads; },10s);
        ARIADNE_TESTING_EXPECT_EQUAL(a.load(),n_threads);
        threads.clear();
    }


};

int main(int argc, char** argv) {
    ThreadRegistry registry;
    Ariadne::Logger::instance().attach_thread_registry(&registry);
    ARIADNE_TESTING_SUITE("thread");
    ARIADNE_TESTING_ADD_TEST(TestThread, test_create);
    ARIADNE_TESTING_ADD_TEST(TestThread, test_destroy_before_completion);
    ARIADNE_TESTING_ADD_TEST(TestThread, test_task);
    ARIADNE_TESTING_ADD_TEST(TestThread, test_exception);
    ARIADNE_TESTING_ADD_TEST(TestThread, test_destroy_inactive);
    ARIADNE_TESTING_ADD_TEST(TestThread, test_concurrent_activate);
    ARIADNE_TESTING_ADD_TEST(TestThread, test_atomic_multiple_threads);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}