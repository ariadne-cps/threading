#include "ariadne-threading.hpp"
#include "ariadne-testing.hpp"
#include <atomic>
#include <barrier>
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
            std::stop_source cancel_during_search;
            bool request_external_stop=false;
            bool force_no_result=false;
            bool simultaneous_success=false;
            bool throw_from_candidate=false;
            std::barrier both_candidates_ready(2);
            auto search=[&](std::size_t index,std::stop_token stop)
                    -> std::optional<unsigned> {
                ++calls;
                if(throw_from_candidate)
                    throw std::runtime_error("candidate failed");
                if(simultaneous_success) {
                    // Both callbacks finish their computation before either
                    // can publish. Only one result may be selected.
                    both_candidates_ready.arrive_and_wait();
                    return static_cast<unsigned>(17u+index);
                }
                // Trigger cancellation *after* the operation starts. The
                // candidate sees the propagated stop token immediately.
                if(request_external_stop && index==0u) {
                    // A non-cooperative callback can finish with a value
                    // after an external stop. The publisher must reject it.
                    cancel_during_search.request_stop();
                    return 17u;
                }
                if(stop.stop_requested())return std::nullopt;
                if(index==0u && !force_no_result)return 17u;
                return std::nullopt;
            };
            auto found=parallel_first_result(pool,32u,search);
            ARIADNE_TESTING_PROVE(bool(found));
            ARIADNE_TESTING_EXPECT_EQUAL(*found,17u);
            ARIADNE_TESTING_PROVE(calls.load()>=1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});

            // An explicit single-lane limit must still find the same
            // candidate; the search callback is reused for this and the
            // cancellation case rather than instantiating new behaviors.
            auto limited=parallel_first_result(pool,32u,search,1u);
            ARIADNE_TESTING_PROVE(bool(limited));
            ARIADNE_TESTING_EXPECT_EQUAL(*limited,17u);

            // Exhaust the entire candidate range through the *same* F&
            // instantiation. A negative result must visit each candidate
            // once, then terminate on the index >= candidates boundary.
            force_no_result=true;
            const auto before_exhaustion=calls.load();
            auto exhausted=parallel_first_result(pool,3u,search,1u);
            ARIADNE_TESTING_PROVE(!exhausted);
            ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),before_exhaustion+3u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
            force_no_result=false;

            // Both preflight exits must avoid invoking any candidate.
            // Reuse the same callback (F&) so these branches belong to
            // the same function instantiation as the normal search.
            const auto before_preflight=calls.load();
            auto empty_search=parallel_first_result(pool,0u,search);
            ARIADNE_TESTING_PROVE(!empty_search);
            ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),before_preflight);

            std::stop_source already_stopped;
            already_stopped.request_stop();
            auto pre_cancelled=parallel_first_result(
                pool,3u,search,0u,already_stopped.get_token());
            ARIADNE_TESTING_PROVE(!pre_cancelled);
            ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),before_preflight);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});

            if(workers!=0u) {
                // Check the already-tested publication race in the same
                // callback instantiation that covers the normal search.
                const auto before_race=calls.load();
                simultaneous_success=true;
                auto winner=parallel_first_result(pool,2u,search,2u);
                ARIADNE_TESTING_PROVE(bool(winner));
                ARIADNE_TESTING_PROVE(*winner==17u || *winner==18u);
                ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),before_race+2u);
                ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
                simultaneous_success=false;
            }

            // A candidate exception must propagate through the same
            // instantiation, without leaving pending work behind.
            const auto before_failure=calls.load();
            throw_from_candidate=true;
            ARIADNE_TESTING_EXPECT_EXCEPTION(
                parallel_first_result(pool,1u,search,1u));
            ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),before_failure+1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
            throw_from_candidate=false;

            const auto before_cancel=calls.load();
            request_external_stop=true;
            auto interrupted=parallel_first_result(
                pool,32u,search,0u,cancel_during_search.get_token());
            ARIADNE_TESTING_PROVE(!interrupted);
            ARIADNE_TESTING_PROVE(cancel_during_search.stop_requested());
            ARIADNE_TESTING_PROVE(calls.load()>before_cancel);
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

    void test_graph_stop_before_run_and_resume() {
        for(unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            TaskGraph graph;
            std::atomic<unsigned> count{0u};
            auto a=graph.add_task([&] { ++count; });
            auto b=graph.add_task([&] { ++count; });
            graph.add_dependency(a,b);
            std::stop_source stop;
            stop.request_stop();
            graph.run(pool,ParallelOptions{1u,3u},stop.get_token());
            ARIADNE_TESTING_EXPECT_EQUAL(count.load(),0u);
            graph.run(pool);
            ARIADNE_TESTING_EXPECT_EQUAL(count.load(),2u);
        }
    }

    void test_graph_stop_from_node() {
        // In single-lane mode the first node can request an external
        // stop and prevent other ready nodes from being dispatched.
        for(unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            TaskGraph graph;
            std::stop_source stop;
            std::atomic<unsigned> count{0u};
            graph.add_task([&] {
                ++count;
                stop.request_stop();
            });
            graph.add_task([&] { ++count; });
            graph.run(pool,ParallelOptions{1u,1u},stop.get_token());
            ARIADNE_TESTING_EXPECT_EQUAL(count.load(),1u);
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_graph_with_shared_spawn_budget() {
        for(unsigned workers : {0u,1u,4u}) {
            ThreadPool pool(workers);
            ConcurrencyBudget budget(1u);
            TaskGraph graph;
            std::atomic<unsigned> count{0u};
            auto root=graph.add_task([&] {
                parallel_for(pool,0u,8u,[&](std::size_t) { ++count; },
                             ParallelOptions{1u,5u,std::nullopt,&budget});
            });
            for(unsigned i=0u;i<8u;++i) {
                auto child=graph.add_task([&] { ++count; });
                graph.add_dependency(root,child);
            }
            graph.run(pool,ParallelOptions{1u,5u,std::nullopt,&budget});
            ARIADNE_TESTING_EXPECT_EQUAL(count.load(),16u);
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{1});
            ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
        }
    }

    void test_graph_empty_invalid_options_and_dependency() {
        ThreadPool pool(2);
        TaskGraph empty;
        empty.run(pool);
        ARIADNE_TESTING_EXPECT_EQUAL(empty.size(),size_t{0});
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});

        TaskGraph one;
        std::atomic<unsigned> calls{0u};
        auto task=one.add_task([&] { ++calls; });
        // Both endpoints are checked: an invalid predecessor must be
        // rejected even when the successor is valid.
        ARIADNE_TESTING_EXPECT_EXCEPTION(one.add_dependency(99u,task));
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            one.run(pool,ParallelOptions{0u,2u}));
        ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),0u);
        one.run(pool);
        ARIADNE_TESTING_EXPECT_EQUAL(calls.load(),1u);
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(),size_t{0});
    }

    void test_graph_concurrent_exceptions_join_every_action() {
        ThreadPool pool(2);
        TaskGraph graph;
        std::barrier rendezvous(2);
        std::atomic<unsigned> started{0u};
        // Both roots must start before either throws, guaranteeing that
        // the second error reaches the already-failed graph state.
        for(unsigned i=0u;i<2u;++i)
            graph.add_task([&] {
                ++started;
                rendezvous.arrive_and_wait();
                throw std::runtime_error("independent graph task failure");
            });
        ARIADNE_TESTING_EXPECT_EXCEPTION(
            graph.run(pool,ParallelOptions{1u,2u}));
        ARIADNE_TESTING_EXPECT_EQUAL(started.load(),2u);
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
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_empty_invalid_options_and_dependency);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_concurrent_exceptions_join_every_action);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_ready_without_global_wave_barrier);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_stop_before_run_and_resume);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_stop_from_node);
    ARIADNE_TESTING_ADD_TEST(TestAdvanced,test_graph_with_shared_spawn_budget);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc,argv);
}
