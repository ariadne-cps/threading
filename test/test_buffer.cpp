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
#include "utility/test.hpp"
#include "threading/buffer.hpp"

using namespace Ariadne;

class TestBuffer {
  public:

    void test_construct() {
        Buffer<size_t> buffer(2);
        ARIADNE_TEST_EQUALS(buffer.size(),0);
        ARIADNE_TEST_EQUALS(buffer.capacity(),2);
    }

    void test_construct_invalid() {
        ARIADNE_TEST_FAIL(Buffer<size_t>(0));
    }

    void test_set_capacity_when_empty() {
        Buffer<size_t> buffer(2);
        buffer.set_capacity(5);
        ARIADNE_TEST_EQUALS(buffer.capacity(),5);
        buffer.set_capacity(3);
        ARIADNE_TEST_EQUALS(buffer.capacity(),3);
        ARIADNE_TEST_FAIL(buffer.set_capacity(0));
    }

    void test_set_capacity_when_filled() {
        Buffer<size_t> buffer(2);
        buffer.push(4);
        buffer.push(2);
        ARIADNE_TEST_EXECUTE(buffer.set_capacity(5));
        ARIADNE_TEST_FAIL(buffer.set_capacity(1));
        buffer.pull();
        ARIADNE_TEST_EXECUTE(buffer.set_capacity(1));
    }

    void test_single_buffer() {
        Buffer<size_t> buffer(2);
        buffer.push(4);
        buffer.push(2);
        ARIADNE_TEST_EQUALS(buffer.size(),2);
        auto o1 = buffer.pull();
        auto o2 = buffer.pull();
        ARIADNE_TEST_EQUALS(buffer.size(),0);
        ARIADNE_TEST_EQUALS(o1,4);
        ARIADNE_TEST_EQUALS(o2,2);
    }

    void test_io_buffer() {
        Buffer<size_t> ib(2);
        Buffer<size_t> ob(2);

        std::thread thread([&ib,&ob]() {
            while (true) {
                try {
                    auto i = ib.pull();
                    ob.push(i);
                } catch (BufferInterruptPullingException&) {
                    break;
                }
            }
        });
        ib.push(4);
        ib.push(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ARIADNE_TEST_EQUALS(ib.size(),0);
        ARIADNE_TEST_EQUALS(ob.size(),2);
        auto o1 = ob.pull();
        ARIADNE_TEST_EQUALS(ob.size(),1);
        ARIADNE_TEST_EQUALS(o1,4);
        auto o2 = ob.pull();
        ARIADNE_TEST_EQUALS(ob.size(),0);
        ARIADNE_TEST_EQUALS(o2,2);
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

        ARIADNE_TEST_EQUALS(consumed.load(),producers*per_producer)
        ARIADNE_TEST_EQUALS(buffer.size(),0)
    }

    void test() {
        ARIADNE_TEST_CALL(test_construct());
        ARIADNE_TEST_CALL(test_construct_invalid());
        ARIADNE_TEST_CALL(test_set_capacity_when_empty());
        ARIADNE_TEST_CALL(test_set_capacity_when_filled());
        ARIADNE_TEST_CALL(test_single_buffer());
        ARIADNE_TEST_CALL(test_io_buffer());
        ARIADNE_TEST_CALL(test_multiple_producers_consumers());
    }
};

int main() {
    TestBuffer().test();
    return ARIADNE_TEST_FAILURES;
}
