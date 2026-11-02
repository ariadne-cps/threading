#include "ariadne-threading.hpp"
#include "ariadne-testing.hpp"
#include <atomic>
#include <thread>
#include <vector>

using namespace Ariadne;

class TestObservation : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_snapshot_transitions() {
        WorkloadAdvancement progress(3);
        WorkloadObservable const& observable=progress;
        auto initial=observable.snapshot();
        ARIADNE_TESTING_EXPECT_EQUAL(initial.waiting,3u);
        ARIADNE_TESTING_EXPECT_EQUAL(initial.processing,0u);
        ARIADNE_TESTING_EXPECT_EQUAL(initial.completed,0u);
        ARIADNE_TESTING_EXPECT_EQUAL(initial.total(),3u);
        ARIADNE_TESTING_EXPECT_EQUAL(initial.completion_rate(),0.0);
        progress.add_to_processing();
        progress.add_to_completed();
        auto current=observable.snapshot();
        ARIADNE_TESTING_EXPECT_EQUAL(current.waiting,2u);
        ARIADNE_TESTING_EXPECT_EQUAL(current.processing,0u);
        ARIADNE_TESTING_EXPECT_EQUAL(current.completed,1u);
        ARIADNE_TESTING_EXPECT_EQUAL(current.total(),3u);
        ARIADNE_TESTING_PROVE(!current.has_finished());
        progress.add_to_processing(2);
        progress.add_to_completed(2);
        ARIADNE_TESTING_PROVE(observable.snapshot().has_finished());
        ARIADNE_TESTING_EXPECT_EQUAL(observable.snapshot().completion_rate(),1.0);
    }

    void test_concurrent_snapshot_consistency() {
        constexpr unsigned tasks=1000u;
        WorkloadAdvancement progress(tasks);
        WorkloadObservable const& observable=progress;
        std::atomic<bool> completed{false};
        std::thread consumer([&] {
            while(!completed.load()) {
                auto snapshot=observable.snapshot();
                if(snapshot.total()!=tasks || snapshot.completed>tasks)
                    std::terminate();
            }
        });
        for(unsigned i=0;i<tasks;++i) {
            progress.add_to_processing();
            progress.add_to_completed();
        }
        completed.store(true);
        consumer.join();
        auto final=observable.snapshot();
        ARIADNE_TESTING_EXPECT_EQUAL(final.completed,tasks);
        ARIADNE_TESTING_PROVE(final.has_finished());
    }

    void test_static_and_dynamic_observable() {
        ThreadManager::instance().set_concurrency(0u);
        StaticWorkload<int> fixed([](int const&) {});
        WorkloadObservable const& static_source=fixed;
        fixed.append({1,2,3});
        auto pending=static_source.snapshot();
        ARIADNE_TESTING_EXPECT_EQUAL(pending.waiting,3u);
        fixed.process();
        ARIADNE_TESTING_EXPECT_EQUAL(static_source.snapshot().completed,3u);

        using Dynamic=DynamicWorkload<int>;
        Dynamic dynamic(
            [](int const&,std::shared_ptr<ProgressIndicator>) {},
            [](Dynamic::Access& access,int const& value) {
                if(value>0)access.append(value-1);
            });
        WorkloadObservable const& dynamic_source=dynamic;
        dynamic.append(3);
        dynamic.process();
        auto result=dynamic_source.snapshot();
        ARIADNE_TESTING_EXPECT_EQUAL(result.completed,4u);
        ARIADNE_TESTING_PROVE(result.has_finished());
    }
};

int main(int argc,char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("observation");
    ARIADNE_TESTING_ADD_TEST(TestObservation,test_snapshot_transitions);
    ARIADNE_TESTING_ADD_TEST(TestObservation,test_concurrent_snapshot_consistency);
    ARIADNE_TESTING_ADD_TEST(TestObservation,test_static_and_dynamic_observable);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc,argv);
}
