/***************************************************************************
 *            test_workload_advancement.cpp
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
#include "ariadne-testing.hpp"
#include "threading/workload_advancement.hpp"

using namespace Ariadne;

class TestWorkloadAdvancement : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:

    void test_empty_completion_rate() {
        WorkloadAdvancement wp;
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completion_rate(),1.0);
        ARIADNE_TESTING_PROVE(wp.has_finished());
    }

    void test_creation() {
        WorkloadAdvancement wp(5);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completion_rate(),0.0);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.waiting(),5);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.processing(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completed(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.total(),5);
        ARIADNE_TESTING_PROVE(not wp.has_finished());
    }

    void test_advance() {
        WorkloadAdvancement wp(3);
        wp.add_to_waiting();
        ARIADNE_TESTING_EXPECT_EQUAL(wp.waiting(),4);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.total(),4);
        wp.add_to_processing();
        ARIADNE_TESTING_EXPECT_EQUAL(wp.waiting(),3);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.processing(),1);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.total(),4);
        wp.add_to_completed();
        ARIADNE_TESTING_EXPECT_EQUAL(wp.processing(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completed(),1);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.total(),4);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completion_rate(),0.25);
    }

    void test_finished() {
        WorkloadAdvancement wp;
        ARIADNE_TESTING_PROVE(wp.has_finished());
        wp.add_to_waiting(2);
        ARIADNE_TESTING_PROVE(not wp.has_finished());
        wp.add_to_processing(2);
        wp.add_to_completed(2);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completion_rate(),1.0);
        ARIADNE_TESTING_PROVE(wp.has_finished());
    }


    void test_concurrent_transitions() {
        constexpr size_t count = 128;
        WorkloadAdvancement wp(count);
        std::vector<std::thread> threads;
        threads.reserve(count);
        for (size_t i=0; i<count; ++i) {
            threads.emplace_back([&wp] {
                wp.add_to_processing();
                wp.add_to_completed();
            });
        }
        for (auto& thread : threads) thread.join();
        ARIADNE_TESTING_EXPECT_EQUAL(wp.waiting(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.processing(),0);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completed(),count);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completion_rate(),1.0);
        ARIADNE_TESTING_PROVE(wp.has_finished());
    }

    void test_invalid_transitions() {
        WorkloadAdvancement wp(4);
        ARIADNE_TESTING_EXPECT_EXCEPTION(wp.add_to_waiting(0));
        ARIADNE_TESTING_EXPECT_EXCEPTION(wp.add_to_processing(5));
        ARIADNE_TESTING_EXPECT_EXCEPTION(wp.add_to_completed());
        wp.add_to_processing(2);
        ARIADNE_TESTING_EXPECT_EXCEPTION(wp.add_to_completed(3));
        wp.add_to_completed(1);
        ARIADNE_TESTING_EXPECT_EQUAL(wp.completion_rate(),0.25);
    }


};

int main(int argc, char** argv) {
    ARIADNE_TESTING_SUITE("workload_advancement");
    ARIADNE_TESTING_ADD_TEST(TestWorkloadAdvancement, test_empty_completion_rate);
    ARIADNE_TESTING_ADD_TEST(TestWorkloadAdvancement, test_creation);
    ARIADNE_TESTING_ADD_TEST(TestWorkloadAdvancement, test_advance);
    ARIADNE_TESTING_ADD_TEST(TestWorkloadAdvancement, test_finished);
    ARIADNE_TESTING_ADD_TEST(TestWorkloadAdvancement, test_concurrent_transitions);
    ARIADNE_TESTING_ADD_TEST(TestWorkloadAdvancement, test_invalid_transitions);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}