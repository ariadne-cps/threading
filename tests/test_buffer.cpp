/***************************************************************************
 *            test_buffer.cpp
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

#include <thread>
#include <vector>
#include <atomic>
#include <limits>
#include "ariadne-testing.hpp"
#include "threading/buffer.hpp"

using namespace Ariadne;

class TestBuffer : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:

    void test_construct() {
        Buffer<size_t> buffer(2);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.size(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.capacity(),2);
    }

    void test_construct_invalid() {
        ARIADNE_TESTING_EXPECT_EXCEPTION(Buffer<size_t>(0));
    }

    void test_set_capacity_when_empty() {
        Buffer<size_t> buffer(2);
        buffer.set_capacity(5);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.capacity(),5);
        buffer.set_capacity(3);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.capacity(),3);
        ARIADNE_TESTING_EXPECT_EXCEPTION(buffer.set_capacity(0));
    }

    void test_set_capacity_when_filled() {
        Buffer<size_t> buffer(2);
        buffer.push(4);
        buffer.push(2);
        ARIADNE_TESTING_EXPECT_NO_THROW(buffer.set_capacity(5));
        ARIADNE_TESTING_EXPECT_EXCEPTION(buffer.set_capacity(1));
        buffer.pull();
        ARIADNE_TESTING_EXPECT_NO_THROW(buffer.set_capacity(1));
    }

    void test_single_buffer() {
        Buffer<size_t> buffer(2);
        buffer.push(4);
        buffer.push(2);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.size(),2);
        auto o1 = buffer.pull();
        auto o2 = buffer.pull();
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.size(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(o1,4);
        ARIADNE_TESTING_EXPECT_EQUAL(o2,2);
    }

    void test_io_buffer() {
        Buffer<size_t> ib(2);
        Buffer<size_t> ob(2);

        std::atomic<size_t> forwarded{0};
        std::thread thread([&ib,&ob,&forwarded]() {
            while (true) {
                try {
                    auto i = ib.pull();
                    ob.push(i);
                    ++forwarded;
                } catch (BufferInterruptPullingException&) {
                    break;
                }
            }
        });
        ib.push(4);
        ib.push(2);
        ARIADNE_TESTING_EXPECT_ATOMIC(forwarded,
            [](size_t observed) { return observed == 2; }, std::chrono::seconds(10));
        ARIADNE_TESTING_EXPECT_EQUAL(ib.size(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(ob.size(),2);
        auto o1 = ob.pull();
        ARIADNE_TESTING_EXPECT_EQUAL(ob.size(),1);
        ARIADNE_TESTING_EXPECT_EQUAL(o1,4);
        auto o2 = ob.pull();
        ARIADNE_TESTING_EXPECT_EQUAL(ob.size(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(o2,2);
        ib.interrupt_consuming();
        thread.join();
    }


    void test_multiple_producers_consumers() {
        Buffer<size_t> buffer(4);
        constexpr size_t producers = 4;
        constexpr size_t consumers = 4;
        constexpr size_t per_producer = 100;
        constexpr size_t stop = std::numeric_limits<size_t>::max();

        std::atomic<size_t> consumed = 0;
        std::vector<std::thread> producer_threads;
        std::vector<std::thread> consumer_threads;

        for (size_t i=0; i<consumers; ++i) {
            consumer_threads.emplace_back([&] {
                while (true) {
                    auto value = buffer.pull();
                    if (value == stop) return;
                    ++consumed;
                }
            });
        }

        for (size_t i=0; i<producers; ++i) {
            producer_threads.emplace_back([&,i] {
                for (size_t j=0; j<per_producer; ++j)
                    buffer.push(i*per_producer+j);
            });
        }

        for (auto& thread : producer_threads) thread.join();
        for (size_t i=0; i<consumers; ++i) buffer.push(stop);
        for (auto& thread : consumer_threads) thread.join();

        ARIADNE_TESTING_EXPECT_EQUAL(consumed.load(),producers*per_producer);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.size(),0);
    }

    static void add_cases(Ariadne::Testing::Suite& suite) {
        suite.add("test_construct", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_construct();
        });
        suite.add("test_construct_invalid", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_construct_invalid();
        });
        suite.add("test_set_capacity_when_empty", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_set_capacity_when_empty();
        });
        suite.add("test_set_capacity_when_filled", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_set_capacity_when_filled();
        });
        suite.add("test_single_buffer", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_single_buffer();
        });
        suite.add("test_io_buffer", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_io_buffer();
        });
        suite.add("test_multiple_producers_consumers", [](Ariadne::Testing::Context& context) {
            TestBuffer fixture(context);
            fixture.test_multiple_producers_consumers();
        });
    }
};

int main(int argc, char** argv) {
    Ariadne::Testing::Suite suite("buffer");
    TestBuffer::add_cases(suite);
    return Ariadne::Testing::run_suite(suite, argc, argv);
}