#include "ariadne-threading.hpp"
#include "ariadne-testing.hpp"
#include <atomic>
#include <cstddef>
#include <chrono>
#include <thread>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <vector>

using namespace Ariadne;

class TestAdvanced : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_first_result_and_no_result() {
        for(unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            std::atomic<unsigned> calls{0u};
            auto found=parallel_first_result(pool,32u,
                [&](std::size_t index,std::stop_token stop)
                    -> std::optional<unsigned> {
                    ++calls;
                    if(stop.stop_requested())return std::nullopt;
                    if(index==0u)return 17u;
                    return std::nullopt;
                });
            ARIADNE_TESTING_PROVE(bool(found));
            ARIADNE_TESTING_EXPECT_EQUAL(*found,17u);
            ARIADNE_TESTING_PROVE(calls.load()>=1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
            auto none=parallel_first_result(pool,8u,
                [](std::size_t,std::stop_token)->std::optional<unsigned> {
                    return std::nullopt;
                });
            ARIADNE_TESTING_PROVE(!none);
            ARIADNE_TESTING_PROVE(!parallel_first_result(pool,0u,
                [](std::size_t,std::stop_token)->std::optional<unsigned> {
                    return 5u;
                }));
        }
    }

    void test_first_result_external_stop_and_exception() {
        ThreadPool pool(2);
        std::stop_source cancelled;
        cancelled.request_stop();
        std::atomic<unsigned> calls{0u};
        auto absent=parallel_first_result(pool,16u,
            [&](std::size_t,std::stop_token)->std::optional<int> {
                ++calls;
                return 1;
            },0u,cancelled.get_token());
        ARIADNE_TESTING_PROVE(!absent);
        ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),0u);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_first_result(pool,16u,
                [](std::size_t,std::stop_token)->std::optional<int> {
                    throw std::runtime_error("expected");
                }));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_graph_dependencies_and_reuse() {
        for(unsigned workers : {0u,2u}) {
            ThreadPool pool(workers);
            TaskGraph graph;
            std::atomic<unsigned> bits{0u};
            auto a=graph.add_task([&] { bits.fetch_or(1u); });
            auto b=graph.add_task([&] { bits.fetch_or(2u); });
            auto c=graph.add_task([&] {
                ARIADNE_TESTING_PROVE((bits.load()&3u)==3u);
                bits.fetch_or(4u);
            });
            graph.add_dependency(a,c);
            graph.add_dependency(b,c);
            ARIADNE_TESTING_EXPECT_EQUAL(graph.size(),size_t{3});
            graph.run(pool);
            ARIADNE_TESTING_EXPECT_EQUAL(bits.load(),7u);
            bits.store(0u);
            graph.run(pool);
            ARIADNE_TESTING_EXPECT_EQUAL(bits.load(),7u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_graph_ready_without_global_wave_barrier() {
        // A slow unrelated root must not delay an already-ready child.
        ThreadPool pool(2);
        TaskGraph graph;
        std::atomic<bool> child_done{false};
        std::atomic<bool> child_before_slow{false};
        auto slow=graph.add_task([&] {
            for(unsigned retry=0u;retry<2000u && !child_done.load();++retry)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            child_before_slow.store(child_done.load());
        });
        (void)slow;
        auto fast=graph.add_task([] {});
        auto child=graph.add_task([&] { child_done.store(true); });
        graph.add_dependency(fast,child);
        graph.run(pool,ParallelOptions{1u,3u});
        ARIADNE_TESTING_PROVE(child_before_slow.load());
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_graph_cycle_invalid_and_exception() {
        ThreadPool pool(2);
        TaskGraph graph;
        std::atomic<unsigned> count{0};
        auto a=graph.add_task([&] { ++count; });
        auto b=graph.add_task([&] { ++count; });
        graph.add_dependency(a,b);
        ARIADNE_TESTING_EXPECT_EXCEPTION(graph.add_dependency(a,b));
        ARIADNE_TESTING_EXPECT_EXCEPTION(graph.add_dependency(a,a));
        ARIADNE_TESTING_EXPECT_EXCEPTION(graph.add_dependency(a,99u));
        graph.add_dependency(b,a);
        ARIADNE_TESTING_EXPECT_EXCEPTION(graph.run(pool));
        ARIADNE_TESTING_EXPECT_EQUAL(count.load(),0u);
        TaskGraph throws;
        throws.add_task([] { throw std::runtime_error("expected"); });
        throws.add_task([&] { ++count; });
        ARIADNE_TESTING_EXPECT_EXCEPTION(throws.run(pool));
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }
};

int main(int argc,char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("advanced");
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_first_result_and_no_result);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_first_result_external_stop_and_exception);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_dependencies_and_reuse);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_cycle_invalid_and_exception);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_ready_without_global_wave_barrier);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc,argv);
}
