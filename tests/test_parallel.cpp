#include "ariadne-threading.hpp"
#include "ariadne-testing.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Ariadne;

class TestParallel : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_parallel_for() {
        for(unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            std::vector<std::atomic<unsigned>> hits(1025u);
            for(auto& hit:hits)hit.store(0u);
            parallel_for(pool,10u,1035u,[&](std::size_t i) {
                hits[i-10u].fetch_add(1u,std::memory_order_relaxed);
            },ParallelOptions{17u,3u});
            for(auto const& hit:hits)
                ARIADNE_TESTING_EXPECT_EQUAL(hit.load(),1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_zero_and_invalid_ranges() {
        ThreadPool pool(2);
        std::atomic<unsigned> calls{0};
        parallel_for(pool,42u,42u,[&](std::size_t) { ++calls; });
        ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),0u);
        ARIADNE_TESTING_EXPECT_EQUAL(
            parallel_reduce(pool,10u,10u,13,
                [](std::size_t i) { return static_cast<int>(i); },
                [](int x,int y) { return x+y; }),13);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool,5u,4u,[](std::size_t) {}));
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool,0u,3u,[](std::size_t) {},
                         ParallelOptions{0u,1u}));
    }

    void test_reduce_deterministic() {
        for(unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            auto sum=parallel_reduce(pool,0u,1000u,std::size_t{0},
                [](std::size_t i) { return i+1u; },
                [](std::size_t a,std::size_t b) { return a+b; },
                ParallelOptions{9u,4u});
            ARIADNE_TESTING_EXPECT_EQUAL(sum,size_t{500500});
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_exception_drains_lanes() {
        ThreadPool pool(2);
        std::atomic<unsigned> entered{0u};
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool,0u,256u,[&](std::size_t i) {
                entered.fetch_add(1u,std::memory_order_relaxed);
                if(i==0u)throw std::runtime_error("expected");
            },ParallelOptions{32u,3u}));
        ARIADNE_TESTING_PROVE(entered.load()>0u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_portfolio_stop() {
        for(unsigned workers : {0u,2u}) {
            ThreadPool pool(workers);
            std::atomic<unsigned> completed{0u};
            StopTaskGroup group(pool);
            auto first=group.fork([&](std::stop_token token) {
                if(!token.stop_requested()) {
                    ++completed;
                    group.request_stop();
                }
                return 7;
            });
            auto second=group.fork([&](std::stop_token token) {
                while(!token.stop_requested())std::this_thread::yield();
                ++completed;
                return 9;
            });
            ARIADNE_TESTING_EXPECT_EQUAL(first.get(),7);
            ARIADNE_TESTING_EXPECT_EQUAL(second.get(),9);
            group.join();
            ARIADNE_TESTING_PROVE(group.stop_requested());
            ARIADNE_TESTING_EXPECT_EQUAL(completed.load(),2u);
        }
    }

    void test_portfolio_exception_and_scope_join() {
        ThreadPool pool(0);
        std::atomic<unsigned> visited{0u};
        {
            StopTaskGroup group(pool);
            group.fork([&](std::stop_token) {
                ++visited;
                throw std::runtime_error("expected");
            });
            ARIADNE_TESTING_EXPECT_EXCEPTION(group.join());
        }
        ARIADNE_TESTING_EXPECT_EQUAL(visited.load(),1u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }
};

int main(int argc,char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("parallel");
    ARIADNE_TESTING_ADD_TEST(TestParallel,test_parallel_for);
    ARIADNE_TESTING_ADD_TEST(TestParallel,test_zero_and_invalid_ranges);
    ARIADNE_TESTING_ADD_TEST(TestParallel,test_reduce_deterministic);
    ARIADNE_TESTING_ADD_TEST(TestParallel,test_exception_drains_lanes);
    ARIADNE_TESTING_ADD_TEST(TestParallel,test_portfolio_stop);
    ARIADNE_TESTING_ADD_TEST(TestParallel,test_portfolio_exception_and_scope_join);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc,argv);
}
