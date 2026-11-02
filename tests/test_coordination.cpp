#include "ariadne-threading.hpp"
#include "ariadne-testing.hpp"
#include <atomic>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Ariadne;

class TestCoordination : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_fifo_close_and_move() {
        BoundedChannel<std::unique_ptr<int>> q(2);
        ARIADNE_TESTING_PROVE(q.try_send(std::make_unique<int>(3)));
        ARIADNE_TESTING_PROVE(q.try_send(std::make_unique<int>(5)));
        ARIADNE_TESTING_PROVE(!q.try_send(std::make_unique<int>(7)));
        ARIADNE_TESTING_PROVE(q.close());
        ARIADNE_TESTING_PROVE(!q.close());
        ARIADNE_TESTING_PROVE(!q.try_send(std::make_unique<int>(9)));
        auto a=q.receive();
        auto b=q.receive();
        ARIADNE_TESTING_PROVE(bool(a) && bool(b));
        ARIADNE_TESTING_EXPECT_EQUAL(**a,3);
        ARIADNE_TESTING_EXPECT_EQUAL(**b,5);
        ARIADNE_TESTING_PROVE(!q.receive());
        ARIADNE_TESTING_EXPECT_EQUAL(q.size(),size_t{0});
    }

    void test_channel_stop() {
        BoundedChannel<int> q(1);
        std::stop_source stop;
        stop.request_stop();
        ARIADNE_TESTING_PROVE(!q.receive(stop.get_token()));
        ARIADNE_TESTING_PROVE(q.send(1));
        ARIADNE_TESTING_PROVE(!q.send(2,stop.get_token()));
        ARIADNE_TESTING_PROVE(!q.receive(stop.get_token()));
        ARIADNE_TESTING_EXPECT_EQUAL(*q.try_receive(),1);
        ARIADNE_TESTING_PROVE(!q.try_receive());
    }

    void test_channel_worker_exchange() {
        BoundedChannel<unsigned> channel(8);
        ThreadPool pool(2);
        StopTaskGroup group(pool);
        std::atomic<unsigned> received{0u};
        auto producer=group.fork([&](std::stop_token token) {
            for(unsigned i=1;i<=100u;++i)
                if(!channel.send(i,token))return;
            channel.close();
        });
        auto consumer=group.fork([&](std::stop_token token) {
            while(auto item=channel.receive(token))
                received.fetch_add(*item,std::memory_order_relaxed);
        });
        producer.get();
        consumer.get();
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(received.load(),5050u);
        ARIADNE_TESTING_PROVE(channel.closed());
    }

    void test_budget_raii() {
        ConcurrencyBudget budget(2);
        {
            auto a=budget.try_acquire();
            auto b=budget.try_acquire();
            ARIADNE_TESTING_PROVE(bool(a) && bool(b));
            ARIADNE_TESTING_PROVE(!budget.try_acquire());
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{0});
            a->release();
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{1});
            auto c=budget.try_acquire();
            ARIADNE_TESTING_PROVE(bool(c));
            auto moved=std::move(*c);
            ARIADNE_TESTING_PROVE(bool(moved));
        }
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{2});
    }

    void test_budget_permit_move_assignment() {
        // Replacing an owned permit releases its previous slot exactly once.
        ConcurrencyBudget budget(2);
        ARIADNE_TESTING_EXPECT_EQUAL(budget.capacity(),size_t{2});
        auto first=budget.try_acquire();
        auto second=budget.try_acquire();
        ARIADNE_TESTING_PROVE(bool(first) && bool(second));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{0});

        auto held=std::move(*first);
        ARIADNE_TESTING_PROVE(!static_cast<bool>(*first));
        ARIADNE_TESTING_PROVE(static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{0});

        held=std::move(*second);
        ARIADNE_TESTING_PROVE(!static_cast<bool>(*second));
        ARIADNE_TESTING_PROVE(static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{1});

        // Self-move must preserve ownership and capacity, not release it.
        auto* same=&held;
        held=std::move(*same);
        ARIADNE_TESTING_PROVE(static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{1});

        // Move-assigning an empty source releases the last held permit.
        held=std::move(*first);
        ARIADNE_TESTING_PROVE(!static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{2});
        held.release(); // Releasing an empty permit is idempotent.
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{2});
    }

    void test_budget_stop() {
        ConcurrencyBudget budget(1);
        auto first=budget.acquire();
        std::stop_source stop;
        stop.request_stop();
        ARIADNE_TESTING_PROVE(!budget.acquire(stop.get_token()));
        first->release();
        ARIADNE_TESTING_PROVE(bool(budget.acquire()));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(),size_t{1});
    }

    void test_invalid_capacity() {
        ARIADNE_TESTING_EXPECT_EXCEPTION(BoundedChannel<int>(0));
        ARIADNE_TESTING_EXPECT_EXCEPTION(ConcurrencyBudget(0));
    }
};

int main(int argc,char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("coordination");
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_fifo_close_and_move);
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_channel_stop);
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_channel_worker_exchange);
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_budget_raii);
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_budget_permit_move_assignment);
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_budget_stop);
    ARIADNE_TESTING_ADD_TEST(TestCoordination,test_invalid_capacity);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc,argv);
}
