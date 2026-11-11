/***************************************************************************
 *            test_umbrella.cpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
/*
 *  This file is part of Threading, licensed under GPL-3.0-or-later.
 */

// The umbrella must be a self-contained first include.
#include <type_traits>

#include "ariadne-testing.hpp"
#include "ariadne-threading.hpp"

using namespace Ariadne;

class TestUmbrella : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;

    void test_public_api() {
        static_assert(std::is_class_v<Thread>);
        static_assert(std::is_class_v<BufferedThread>);
        static_assert(std::is_class_v<ThreadManager>);
        static_assert(std::is_class_v<ThreadingCommandLineInterface>);
        static_assert(std::is_class_v<WorkloadAdvancement>);

        Buffer<int> buffer(2);
        buffer.push(7);
        ARIADNE_TESTING_EXPECT_EQUAL(buffer.pull(), 7);

        ThreadPool pool(0);
        TaskGroup group(pool);
        auto handle = group.fork([] { return 42; });
        ARIADNE_TESTING_EXPECT_EQUAL(handle.get(), 42);
        group.join();
        ARIADNE_TESTING_EXPECT_EQUAL(pool.queue_size(), size_t{0});
    }
};

int main(int argc, char** argv) {
    (void)ThreadManager::instance();
    ARIADNE_TESTING_SUITE("umbrella");
    ARIADNE_TESTING_ADD_TEST(TestUmbrella, test_public_api);
    return ARIADNE_TESTING_RUN_TESTS_WITH_ARGS(argc, argv);
}
