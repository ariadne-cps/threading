#include <algorithm>
#include <atomic>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ariadne-testing.hpp"
#include "ariadne-threading.hpp"

using namespace Ariadne;

class TestParallel : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_parallel_for() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            std::vector<std::atomic<unsigned>> hits(1025u);
            for (auto& hit: hits)
                hit.store(0u);
            parallel_for(pool, 10u, 1035u, [&](std::size_t i) { hits[i - 10u].fetch_add(1u, std::memory_order_relaxed); }, ParallelOptions{17u, 3u});
            for (const auto& hit: hits)
                ARIADNE_TESTING_EXPECT_EQUAL(hit.load(), 1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_zero_and_invalid_ranges() {
        ThreadPool pool(2);
        std::atomic<unsigned> calls{0};
        parallel_for(pool, 42u, 42u, [&](std::size_t) { ++calls; });
        ARIADNE_TESTING_EXPECT_EQUAL(calls.load(), 0u);
        ARIADNE_TESTING_EXPECT_EQUAL(
            parallel_reduce(pool, 10u, 10u, 13, [](std::size_t i) { return static_cast<int>(i); }, [](int x, int y) { return x + y; }), 13);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 5u, 4u, [](std::size_t) {}));
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 3u, [](std::size_t) {}, ParallelOptions{0u, 1u}));
    }

    void test_reduce_deterministic() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            auto sum = parallel_reduce(pool, 0u, 1000u, std::size_t{0}, [](std::size_t i) { return i + 1u; }, [](std::size_t a, std::size_t b) { return a + b; }, ParallelOptions{9u, 4u});
            ARIADNE_TESTING_EXPECT_EQUAL(sum, size_t{500500});
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_exception_drains_lanes() {
        ThreadPool pool(2);
        std::atomic<unsigned> entered{0u};
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 256u, [&](std::size_t i) {
                entered.fetch_add(1u,std::memory_order_relaxed);
                if(i==0u)throw std::runtime_error("expected"); }, ParallelOptions{32u, 3u}));
        ARIADNE_TESTING_PROVE(entered.load() > 0u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }

    void test_rounding_mode_is_scoped_per_lane() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            const int caller_rounding = std::fegetround();
            ARIADNE_TESTING_PROVE(caller_rounding != -1);
            std::atomic<unsigned> observed{0u};
            parallel_for(pool, 0u, 4096u, [&](std::size_t) {
                if(std::fegetround()==FE_DOWNWARD)
                    observed.fetch_add(1u,std::memory_order_relaxed); }, ParallelOptions{1u, 5u, FE_DOWNWARD});
            ARIADNE_TESTING_EXPECT_EQUAL(observed.load(), 4096u);
            ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), caller_rounding);

            std::atomic<unsigned> map_checks{0u};
            std::atomic<unsigned> reduce_checks{0u};
            auto result = parallel_reduce(pool, 0u, 128u, 0u, [&](std::size_t) -> unsigned {
                    if(std::fegetround()==FE_UPWARD)++map_checks;
                    return 1u; }, [&](unsigned a, unsigned b) {
                    if(std::fegetround()==FE_UPWARD)++reduce_checks;
                    return a+b; }, ParallelOptions{1u, 5u, FE_UPWARD});
            ARIADNE_TESTING_EXPECT_EQUAL(result, 128u);
            // 128 map operations and 128 in-lane reductions must all
            // use FE_UPWARD; joining N lanes adds N-1 reductions.
            const auto lanes = std::min(std::size_t{128},
                                        std::min(std::size_t{5}, std::size_t{workers} + 1u));
            ARIADNE_TESTING_EXPECT_EQUAL(map_checks.load(), 128u);
            ARIADNE_TESTING_EXPECT_EQUAL(reduce_checks.load(),
                                         static_cast<unsigned>(128u + lanes - 1u));
            ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), caller_rounding);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_invalid_rounding_mode_rejected_without_state_change() {
        // The requested mode is not one of the supported FE_* constants.
        // A failed construction must leave the caller's FP environment
        // unchanged, and the exception must remain observable.
        const int original = std::fegetround();
        ARIADNE_TESTING_PROVE(original != -1);
        ARIADNE_TESTING_EXPECT_EXCEPTION(ScopedRoundingMode(-1));
        ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), original);

        // A valid guard must still work after a rejected request.
        {
            ScopedRoundingMode guard(FE_TONEAREST);
            ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), FE_TONEAREST);
        }
        ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), original);
    }

    void test_rounding_mode_restored_after_exception() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            const int initial = std::fegetround();
            ARIADNE_TESTING_PROVE(initial != -1);
            ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_reduce(
                pool, 0u, 32u, 0u,
                [](std::size_t i) -> unsigned {
                    if (i == 0u)
                        throw std::runtime_error("expected");
                    return 1u;
                },
                [](unsigned a, unsigned b) { return a + b; },
                ParallelOptions{1u, 5u, FE_DOWNWARD}));
            ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), initial);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_fixed_block_reduce_independent_of_workers() {
        // Ordinary IEEE summation can depend on grouping. Fixed blocks
        // constrain grouping without claiming a correctly rounded sum.
        const double samples[] = {1.0e16, -1.0e16, 1.0, 1.0e16, -1.0e16, 1.0};
        std::uint64_t reference = 0u;
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            for (unsigned max_lanes: {0u, 1u, 3u}) {
                const auto sum = parallel_reduce_fixed_blocks(
                    pool, 0u, 6u, 0.0,
                    [&](std::size_t i) { return samples[i]; },
                    [](double a, double b) { return a + b; },
                    ParallelOptions{3u, max_lanes, FE_TONEAREST});
                ARIADNE_TESTING_EXPECT_EQUAL(sum, 2.0);
                const auto bits = std::bit_cast<std::uint64_t>(sum);
                if (workers == 0u && max_lanes == 0u)
                    reference = bits;
                ARIADNE_TESTING_EXPECT_EQUAL(bits, reference);
            }
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_fixed_block_reduce_exceptions_and_rounding() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            const auto initial = std::fegetround();
            ARIADNE_TESTING_PROVE(initial != -1);
            std::atomic<unsigned> maps{0u};
            std::atomic<unsigned> reductions{0u};
            auto result = parallel_reduce_fixed_blocks(pool, 0u, 19u, 0u, [&](std::size_t) -> unsigned {
                    if(std::fegetround()==FE_UPWARD)++maps;
                    return 1u; }, [&](unsigned a, unsigned b) {
                    if(std::fegetround()==FE_UPWARD)++reductions;
                    return a+b; }, ParallelOptions{4u, 3u, FE_UPWARD});
            ARIADNE_TESTING_EXPECT_EQUAL(result, 19u);
            ARIADNE_TESTING_EXPECT_EQUAL(maps.load(), 19u);
            ARIADNE_TESTING_EXPECT_EQUAL(reductions.load(), 23u);
            ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), initial);
            ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_reduce_fixed_blocks(
                pool, 0u, 16u, 0u,
                [](std::size_t i) -> unsigned {
                    if (i == 0u)
                        throw std::runtime_error("expected");
                    return 1u;
                },
                [](unsigned a, unsigned b) { return a + b; },
                ParallelOptions{2u, 3u, FE_DOWNWARD}));
            ARIADNE_TESTING_EXPECT_EQUAL(std::fegetround(), initial);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_cooperative_parallel_cancellation() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            std::stop_source stop;
            stop.request_stop();
            std::atomic<unsigned> calls{0u};
            ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_for(
                pool, 0u, 64u, [&](std::size_t) { ++calls; },
                ParallelOptions{1u, 3u}, stop.get_token()));
            ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_reduce(
                pool, 0u, 64u, 0u,
                [&](std::size_t) -> unsigned { ++calls; return 1u; },
                [](unsigned a, unsigned b) { return a + b; },
                ParallelOptions{1u, 3u}, stop.get_token()));
            ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_reduce_fixed_blocks(
                pool, 0u, 64u, 0u,
                [&](std::size_t) -> unsigned { ++calls; return 1u; },
                [](unsigned a, unsigned b) { return a + b; },
                ParallelOptions{8u, 3u}, stop.get_token()));
            ARIADNE_TESTING_EXPECT_EQUAL(calls.load(), 0u);
            stop = std::stop_source{};
            std::atomic<unsigned> started{0u};
            ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_for(
                pool, 0u, 1024u, [&](std::size_t) {
                    if (started.fetch_add(1u) == 0u)
                        stop.request_stop();
                },
                ParallelOptions{1u, 3u}, stop.get_token()));
            ARIADNE_TESTING_PROVE(started.load() >= 1u);
            ARIADNE_TESTING_PROVE(started.load() < 1024u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_cooperative_reduce_stop_during_work() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            for (bool fixed: {false, true}) {
                std::stop_source stop;
                std::atomic<unsigned> visited{0u};
                auto map = [&](std::size_t) -> unsigned {
                    if (visited.fetch_add(1u) == 0u)
                        stop.request_stop();
                    return 1u;
                };
                auto reduce = [](unsigned a, unsigned b) { return a + b; };
                ARIADNE_TESTING_EXPECT_EXCEPTION(
                    fixed
                        ? parallel_reduce_fixed_blocks(pool, 0u, 1024u, 0u, map, reduce,
                                                       ParallelOptions{8u, 3u}, stop.get_token())
                        : parallel_reduce(pool, 0u, 1024u, 0u, map, reduce,
                                          ParallelOptions{1u, 3u}, stop.get_token()));
                ARIADNE_TESTING_PROVE(visited.load() >= 1u);
                ARIADNE_TESTING_PROVE(visited.load() < 1024u);
                ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
            }
        }
    }

    void test_shared_spawn_budget_nested() {
        for (unsigned workers: {0u, 1u, 4u}) {
            ThreadPool pool(workers);
            ConcurrencyBudget budget(1u);
            std::atomic<unsigned> visited{0u};
            parallel_for(pool, 0u, 8u, [&](std::size_t) { parallel_for(pool, 0u, 16u, [&](std::size_t) { ++visited; }, ParallelOptions{1u, 5u, std::nullopt, &budget}); }, ParallelOptions{1u, 5u, std::nullopt, &budget});
            ARIADNE_TESTING_EXPECT_EQUAL(visited.load(), 128u);
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});
            auto result = parallel_reduce(pool, 0u, 64u, 0u, [](std::size_t) -> unsigned { return 1u; }, [](unsigned a, unsigned b) { return a + b; }, ParallelOptions{1u, 5u, std::nullopt, &budget});
            ARIADNE_TESTING_EXPECT_EQUAL(result, 64u);
            auto fixed = parallel_reduce_fixed_blocks(pool, 0u, 64u, 0u, [](std::size_t) -> unsigned { return 1u; }, [](unsigned a, unsigned b) { return a + b; }, ParallelOptions{4u, 5u, std::nullopt, &budget});
            ARIADNE_TESTING_EXPECT_EQUAL(fixed, 64u);
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
        }
    }

    void test_budget_exhaustion_and_exception_release() {
        ThreadPool pool(2);
        ConcurrencyBudget budget(1u);
        auto held = budget.try_acquire();
        ARIADNE_TESTING_PROVE(bool(held));
        std::atomic<unsigned> hit{0u};
        parallel_for(pool, 0u, 23u, [&](std::size_t) { ++hit; }, ParallelOptions{1u, 3u, std::nullopt, &budget});
        ARIADNE_TESTING_EXPECT_EQUAL(hit.load(), 23u);
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{0});
        held->release();
        ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_for(pool, 0u, 32u, [](std::size_t i) {
                if(i==0u)throw std::runtime_error("expected"); }, ParallelOptions{1u, 3u, std::nullopt, &budget}));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }

    void test_portfolio_stop() {
        for (unsigned workers: {0u, 2u}) {
            ThreadPool pool(workers);
            std::atomic<unsigned> completed{0u};
            StopTaskGroup group(pool);
            auto first = group.fork([&](std::stop_token token) {
                if (!token.stop_requested()) {
                    ++completed;
                    group.request_stop();
                }
                return 7;
            });
            auto second = group.fork([&](std::stop_token token) {
                while (!token.stop_requested())
                    std::this_thread::yield();
                ++completed;
                return 9;
            });
            ARIADNE_TESTING_EXPECT_EQUAL(first.get(), 7);
            ARIADNE_TESTING_EXPECT_EQUAL(second.get(), 9);
            group.join();
            ARIADNE_TESTING_PROVE(group.stop_requested());
            ARIADNE_TESTING_EXPECT_EQUAL(completed.load(), 2u);
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
        ARIADNE_TESTING_EXPECT_EQUAL(visited.load(), 1u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }

    void test_parallel_for_path_matrix() {
        // One callback type exercises every branch of parallel_for and
        // execute_lanes in a single instantiation: invalid range, empty
        // range, pre-requested stop, single lane, multiple lanes, stop
        // during work, caller-lane failure and forked-lane failure.
        ThreadPool pool(2u);
        std::atomic<int> mode{0};
        std::stop_source stop;
        std::atomic<bool> forked_threw{false};
        auto fn = [&](std::size_t i) {
            const int m = mode.load(std::memory_order_relaxed);
            if (m == 1 && i == 0u)
                throw std::runtime_error("caller lane");
            if (m == 2 && i >= 4u)
                throw std::runtime_error("forked lane");
            if (m == 3 && i == 0u)
                stop.request_stop();
            if (m == 4) {
                if (i >= 4u) {
                    forked_threw.store(true, std::memory_order_release);
                    throw std::runtime_error("forked lane");
                }
                if (i == 0u) {
                    while (!forked_threw.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    throw std::runtime_error("caller lane");
                }
            }
        };
        ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_for(pool, 5u, 4u, fn));
        parallel_for(pool, 7u, 7u, fn, ParallelOptions{1u, 0u});
        stop.request_stop();
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 0u}, stop.get_token()));
        stop = std::stop_source{};
        parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 1u});
        parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 0u});
        mode.store(3);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 0u}, stop.get_token()));
        stop = std::stop_source{};
        mode.store(1);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 0u}));
        mode.store(2);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 0u}));
        mode.store(4);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_for(pool, 0u, 8u, fn, ParallelOptions{1u, 0u}));
        mode.store(0);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }

    void test_parallel_reduce_path_matrix() {
        // One map/reduce pair exercises every branch of parallel_reduce in
        // a single instantiation, including a stop requested while the
        // partial lane results are being combined.
        ThreadPool pool(2u);
        std::atomic<int> mode{0};
        std::stop_source stop;
        std::atomic<unsigned> reduce_calls{0u};
        auto map = [&](std::size_t i) -> unsigned {
            const int m = mode.load(std::memory_order_relaxed);
            if (m == 1 && i == 0u)
                stop.request_stop();
            if (m == 2 && i == 0u)
                throw std::runtime_error("map");
            return 1u;
        };
        auto reduce = [&](unsigned a, unsigned b) {
            const unsigned call =
                reduce_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
            // With count==6 every lane reduces once per element, so the
            // first combine of partial results is reduce call number 7.
            if ((mode.load(std::memory_order_relaxed) == 3 ||
                 mode.load(std::memory_order_relaxed) == 4) &&
                call == 7u)
                stop.request_stop();
            return a + b;
        };
        ARIADNE_TESTING_EXPECT_EXCEPTION(parallel_reduce(pool, 5u, 4u, 0u, map, reduce));
        ARIADNE_TESTING_EXPECT_EQUAL(parallel_reduce(pool, 7u, 7u, 42u, map, reduce), 42u);
        stop.request_stop();
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 0u},
                            stop.get_token()));
        stop = std::stop_source{};
        ARIADNE_TESTING_EXPECT_EQUAL(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 0u}), 6u);
        ARIADNE_TESTING_EXPECT_EQUAL(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 1u}), 6u);
        mode.store(1);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 0u},
                            stop.get_token()));
        stop = std::stop_source{};
        mode.store(2);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 0u}));
        mode.store(3);
        reduce_calls.store(0u);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 0u},
                            stop.get_token()));
        stop = std::stop_source{};
        mode.store(4);
        reduce_calls.store(0u);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce(pool, 0u, 6u, 0u, map, reduce, ParallelOptions{1u, 2u},
                            stop.get_token()));
        mode.store(0);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }

    void test_fixed_blocks_path_matrix() {
        // One map/reduce pair exercises every branch of
        // parallel_reduce_fixed_blocks in a single instantiation, including
        // a stop requested while the block subtotals are being combined.
        ThreadPool pool(2u);
        std::atomic<int> mode{0};
        std::stop_source stop;
        std::atomic<unsigned> reduce_calls{0u};
        auto map = [&](std::size_t i) -> unsigned {
            if (mode.load(std::memory_order_relaxed) == 1 && i == 0u)
                stop.request_stop();
            return 1u;
        };
        auto reduce = [&](unsigned a, unsigned b) {
            const unsigned call =
                reduce_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
            // With grain_size one and count N there are N in-block reduce
            // calls, so the first combine of block subtotals is call N+1.
            const int m = mode.load(std::memory_order_relaxed);
            if (m == 2 && call == 4u)
                stop.request_stop();
            if (m == 3 && call == 3u)
                stop.request_stop();
            return a + b;
        };
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce_fixed_blocks(pool, 5u, 4u, 0u, map, reduce));
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce_fixed_blocks(pool, 0u, 4u, 0u, map, reduce,
                                         ParallelOptions{0u, 0u}));
        ARIADNE_TESTING_EXPECT_EQUAL(
            parallel_reduce_fixed_blocks(pool, 7u, 7u, 42u, map, reduce,
                                         ParallelOptions{4u, 0u}),
            42u);
        stop.request_stop();
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce_fixed_blocks(pool, 0u, 6u, 0u, map, reduce,
                                         ParallelOptions{2u, 0u}, stop.get_token()));
        stop = std::stop_source{};
        ARIADNE_TESTING_EXPECT_EQUAL(
            parallel_reduce_fixed_blocks(pool, 0u, 6u, 0u, map, reduce,
                                         ParallelOptions{2u, 0u}),
            6u);
        mode.store(1);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce_fixed_blocks(pool, 0u, 6u, 0u, map, reduce,
                                         ParallelOptions{2u, 0u}, stop.get_token()));
        stop = std::stop_source{};
        mode.store(2);
        reduce_calls.store(0u);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce_fixed_blocks(pool, 0u, 3u, 0u, map, reduce,
                                         ParallelOptions{1u, 0u}, stop.get_token()));
        stop = std::stop_source{};
        mode.store(3);
        reduce_calls.store(0u);
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            parallel_reduce_fixed_blocks(pool, 0u, 2u, 0u, map, reduce,
                                         ParallelOptions{1u, 0u}, stop.get_token()));
        mode.store(0);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }
};

int main(int argc, char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("parallel");
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_parallel_for);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_zero_and_invalid_ranges);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_reduce_deterministic);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_exception_drains_lanes);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_rounding_mode_is_scoped_per_lane);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_rounding_mode_restored_after_exception);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_invalid_rounding_mode_rejected_without_state_change);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_fixed_block_reduce_independent_of_workers);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_fixed_block_reduce_exceptions_and_rounding);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_cooperative_parallel_cancellation);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_cooperative_reduce_stop_during_work);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_shared_spawn_budget_nested);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_budget_exhaustion_and_exception_release);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_portfolio_stop);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_portfolio_exception_and_scope_join);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_parallel_for_path_matrix);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_parallel_reduce_path_matrix);
    ARIADNE_TESTING_ADD_TEST(TestParallel, test_fixed_blocks_path_matrix);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}
