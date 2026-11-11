#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include "ariadne-testing.hpp"
#include "ariadne-threading.hpp"

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
        auto a = q.receive();
        auto b = q.receive();
        ARIADNE_TESTING_PROVE(bool(a) && bool(b));
        ARIADNE_TESTING_EXPECT_EQUAL(**a, 3);
        ARIADNE_TESTING_EXPECT_EQUAL(**b, 5);
        ARIADNE_TESTING_PROVE(!q.receive());
        ARIADNE_TESTING_EXPECT_EQUAL(q.size(), size_t{0});
    }

    void test_channel_stop() {
        BoundedChannel<int> q(1);
        std::stop_source stop;
        stop.request_stop();
        ARIADNE_TESTING_PROVE(!q.receive(stop.get_token()));
        ARIADNE_TESTING_PROVE(q.send(1));
        ARIADNE_TESTING_PROVE(!q.send(2, stop.get_token()));
        ARIADNE_TESTING_PROVE(!q.receive(stop.get_token()));
        ARIADNE_TESTING_EXPECT_EQUAL(*q.try_receive(), 1);
        ARIADNE_TESTING_PROVE(!q.try_receive());
    }

    void test_closed_channel_rejects_blocking_send() {
        BoundedChannel<int> channel(1);
        ARIADNE_TESTING_PROVE(channel.send(17));
        ARIADNE_TESTING_PROVE(channel.close());

        // A closed, full channel must reject the blocking send API without
        // discarding or overwriting the previously buffered item.
        ARIADNE_TESTING_PROVE(!channel.send(23));
        ARIADNE_TESTING_EXPECT_EQUAL(channel.size(), size_t{1});
        auto buffered = channel.receive();
        ARIADNE_TESTING_PROVE(bool(buffered));
        ARIADNE_TESTING_EXPECT_EQUAL(*buffered, 17);

        // Draining the channel does not reopen it, even with free capacity.
        ARIADNE_TESTING_PROVE(!channel.send(29));
        ARIADNE_TESTING_EXPECT_EQUAL(channel.size(), size_t{0});
        ARIADNE_TESTING_PROVE(!channel.receive());
    }

    void test_channel_worker_exchange() {
        BoundedChannel<unsigned> channel(8);
        ThreadPool pool(2);
        StopTaskGroup group(pool);
        std::atomic<unsigned> received{0u};
        auto producer = group.fork([&](std::stop_token token) {
            for (unsigned i = 1; i <= 100u; ++i)
                if (!channel.send(i, token))
                    return;
            channel.close();
        });
        auto consumer = group.fork([&](std::stop_token token) {
            while (auto item = channel.receive(token))
                received.fetch_add(*item, std::memory_order_relaxed);
        });
        producer.get();
        consumer.get();
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(received.load(), 5050u);
        ARIADNE_TESTING_PROVE(channel.closed());
    }

    void test_channel_data_wakes_waiters() {
        // An empty channel must wake a waiting receiver after a publish.
        {
            BoundedChannel<int> channel(1);
            std::promise<void> started;
            auto entered = started.get_future();
            std::stop_source stop;
            auto receiver = std::async(std::launch::async, [&] {
                started.set_value();
                return channel.receive(stop.get_token());
            });
            entered.wait();
            ARIADNE_TESTING_PROVE(receiver.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            ARIADNE_TESTING_PROVE(channel.try_send(17));
            auto status = receiver.wait_for(std::chrono::seconds(5));
            if (status != std::future_status::ready)
                stop.request_stop();
            ARIADNE_TESTING_PROVE(status == std::future_status::ready);
            auto value = receiver.get();
            ARIADNE_TESTING_PROVE(bool(value));
            ARIADNE_TESTING_EXPECT_EQUAL(*value, 17);
            ARIADNE_TESTING_EXPECT_EQUAL(channel.size(), size_t{0});
        }

        // Removing the oldest item must wake a sender blocked by capacity.
        {
            BoundedChannel<int> channel(1);
            ARIADNE_TESTING_PROVE(channel.send(23));
            std::promise<void> started;
            auto entered = started.get_future();
            std::stop_source stop;
            auto sender = std::async(std::launch::async, [&] {
                started.set_value();
                return channel.send(29, stop.get_token());
            });
            entered.wait();
            ARIADNE_TESTING_PROVE(sender.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            auto first = channel.try_receive();
            ARIADNE_TESTING_PROVE(bool(first));
            ARIADNE_TESTING_EXPECT_EQUAL(*first, 23);
            auto status = sender.wait_for(std::chrono::seconds(5));
            if (status != std::future_status::ready)
                stop.request_stop();
            ARIADNE_TESTING_PROVE(status == std::future_status::ready);
            ARIADNE_TESTING_PROVE(sender.get());
            auto second = channel.receive();
            ARIADNE_TESTING_PROVE(bool(second));
            ARIADNE_TESTING_EXPECT_EQUAL(*second, 29);
        }
    }

    void test_channel_close_wakes_waiters() {
        // Closing an empty channel wakes a pending receiver, without data.
        {
            BoundedChannel<int> channel(1);
            std::promise<void> started;
            auto entered = started.get_future();
            std::stop_source stop;
            auto receiver = std::async(std::launch::async, [&] {
                started.set_value();
                return channel.receive(stop.get_token());
            });
            entered.wait();
            ARIADNE_TESTING_PROVE(receiver.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            ARIADNE_TESTING_PROVE(channel.close());
            auto status = receiver.wait_for(std::chrono::seconds(5));
            if (status != std::future_status::ready)
                stop.request_stop();
            ARIADNE_TESTING_PROVE(status == std::future_status::ready);
            ARIADNE_TESTING_PROVE(!receiver.get());
        }

        // Closing a full channel wakes a blocked sender and preserves data.
        {
            BoundedChannel<int> channel(1);
            ARIADNE_TESTING_PROVE(channel.send(31));
            std::promise<void> started;
            auto entered = started.get_future();
            std::stop_source stop;
            auto sender = std::async(std::launch::async, [&] {
                started.set_value();
                return channel.send(37, stop.get_token());
            });
            entered.wait();
            ARIADNE_TESTING_PROVE(sender.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            ARIADNE_TESTING_PROVE(channel.close());
            auto status = sender.wait_for(std::chrono::seconds(5));
            if (status != std::future_status::ready)
                stop.request_stop();
            ARIADNE_TESTING_PROVE(status == std::future_status::ready);
            ARIADNE_TESTING_PROVE(!sender.get());
            auto retained = channel.receive();
            ARIADNE_TESTING_PROVE(bool(retained));
            ARIADNE_TESTING_EXPECT_EQUAL(*retained, 31);
            ARIADNE_TESTING_PROVE(!channel.receive());
        }
    }

    void test_channel_stop_wakes_blocked_operations() {
        // A stop requested *during* a pending receive must wake the caller
        // without closing the channel or inventing a message.
        {
            BoundedChannel<int> channel(1);
            std::stop_source stop;
            std::promise<void> started;
            auto entered = started.get_future();
            auto receiver = std::async(std::launch::async, [&] {
                started.set_value();
                return channel.receive(stop.get_token());
            });
            entered.wait();
            ARIADNE_TESTING_PROVE(receiver.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            stop.request_stop();
            ARIADNE_TESTING_PROVE(receiver.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            ARIADNE_TESTING_PROVE(!receiver.get());
            ARIADNE_TESTING_PROVE(!channel.closed());
            ARIADNE_TESTING_EXPECT_EQUAL(channel.size(), size_t{0});
        }

        // A stop requested *during* a full-capacity send must not replace
        // or move the message already queued.
        {
            BoundedChannel<int> channel(1);
            ARIADNE_TESTING_PROVE(channel.send(41));
            std::stop_source stop;
            std::promise<void> started;
            auto entered = started.get_future();
            auto sender = std::async(std::launch::async, [&] {
                started.set_value();
                return channel.send(43, stop.get_token());
            });
            entered.wait();
            ARIADNE_TESTING_PROVE(sender.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
            stop.request_stop();
            ARIADNE_TESTING_PROVE(sender.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
            ARIADNE_TESTING_PROVE(!sender.get());
            ARIADNE_TESTING_PROVE(!channel.closed());
            ARIADNE_TESTING_EXPECT_EQUAL(channel.size(), size_t{1});
            auto retained = channel.receive();
            ARIADNE_TESTING_PROVE(bool(retained));
            ARIADNE_TESTING_EXPECT_EQUAL(*retained, 41);
        }
    }

    void test_budget_raii() {
        ConcurrencyBudget budget(2);
        {
            auto a = budget.try_acquire();
            auto b = budget.try_acquire();
            ARIADNE_TESTING_PROVE(bool(a) && bool(b));
            ARIADNE_TESTING_PROVE(!budget.try_acquire());
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{0});
            a->release();
            ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});
            auto c = budget.try_acquire();
            ARIADNE_TESTING_PROVE(bool(c));
            auto moved = std::move(*c);
            ARIADNE_TESTING_PROVE(bool(moved));
        }
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{2});
    }

    void test_budget_permit_move_assignment() {
        // Replacing an owned permit releases its previous slot exactly once.
        ConcurrencyBudget budget(2);
        ARIADNE_TESTING_EXPECT_EQUAL(budget.capacity(), size_t{2});
        auto first = budget.try_acquire();
        auto second = budget.try_acquire();
        ARIADNE_TESTING_PROVE(bool(first) && bool(second));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{0});

        auto held = std::move(*first);
        ARIADNE_TESTING_PROVE(!static_cast<bool>(*first));
        ARIADNE_TESTING_PROVE(static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{0});

        held = std::move(*second);
        ARIADNE_TESTING_PROVE(!static_cast<bool>(*second));
        ARIADNE_TESTING_PROVE(static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});

        // Self-move must preserve ownership and capacity, not release it.
        auto* same = &held;
        held = std::move(*same);
        ARIADNE_TESTING_PROVE(static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});

        // Move-assigning an empty source releases the last held permit.
        held = std::move(*first);
        ARIADNE_TESTING_PROVE(!static_cast<bool>(held));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{2});
        held.release(); // Releasing an empty permit is idempotent.
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{2});
    }

    void test_budget_stop() {
        ConcurrencyBudget budget(1);
        auto first = budget.acquire();
        std::stop_source stop;
        stop.request_stop();
        ARIADNE_TESTING_PROVE(!budget.acquire(stop.get_token()));
        first->release();
        ARIADNE_TESTING_PROVE(bool(budget.acquire()));
        ARIADNE_TESTING_EXPECT_EQUAL(budget.available(), size_t{1});
    }

    void test_invalid_capacity() {
        ARIADNE_TESTING_EXPECT_EXCEPTION(BoundedChannel<int>(0));
        ARIADNE_TESTING_EXPECT_EXCEPTION(ConcurrencyBudget(0));
    }
};

int main(int argc, char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("coordination");
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_fifo_close_and_move);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_channel_stop);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_closed_channel_rejects_blocking_send);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_channel_worker_exchange);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_channel_data_wakes_waiters);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_channel_close_wakes_waiters);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_channel_stop_wakes_blocked_operations);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_budget_raii);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_budget_permit_move_assignment);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_budget_stop);
    ARIADNE_TESTING_ADD_TEST(TestCoordination, test_invalid_capacity);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}
