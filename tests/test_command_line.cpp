/***************************************************************************
 *            test_command_line.cpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/

/*
 *  This file is part of Ariadne Threading.
 *
 *  Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "threading/threading_command_line_interface.hpp"
#include "threading/thread_manager.hpp"
#include "logging/logging.hpp"
#include "ariadne-testing.hpp"

using namespace Ariadne;

class TestCommandLine : public Ariadne::Testing::ContextFixture {
  public:
    using ContextFixture::ContextFixture;
  public:
    static void add_cases(Ariadne::Testing::Suite& suite) {
        suite.add("test_inherited_logging", [](Ariadne::Testing::Context& context) {
            TestCommandLine fixture(context);
            fixture.test_inherited_logging();
        });
        suite.add("test_concurrency", [](Ariadne::Testing::Context& context) {
            TestCommandLine fixture(context);
            fixture.test_concurrency();
        });
    }

  private:
    ThreadingCommandLineInterface make_cli() const {
        return ThreadingCommandLineInterface();
    }

    Void test_inherited_logging() {
        ThreadingCommandLineInterface cli;
        ARIADNE_TESTING_PROVE(cli.acquire({""}));
        ARIADNE_TESTING_PROVE(cli.acquire({"","--scheduler","immediate","--verbosity","2","--concurrency","0","--theme","none"}));
        ARIADNE_TESTING_EXPECT_EQUAL(Logger::instance().configuration().verbosity(),2u);
        ARIADNE_TESTING_PROVE(not Logger::instance().configuration().theme().has_style());
        ARIADNE_TESTING_EXPECT_EQUAL(ThreadManager::instance().concurrency(),0u);
        ARIADNE_TESTING_PROVE(not cli.acquire({"","--concurrency","invalid"}));
        Bool custom=false;
        cli.add(CommandLineContribution("custom", {
            make_unvalued_argument_parser("x","custom","Test inherited extension", [&custom] {custom=true;})
        }));
        ARIADNE_TESTING_PROVE(cli.acquire({"","--custom"}));
        ARIADNE_TESTING_PROVE(custom);
        Logger::instance().use_nonblocking_scheduler();
    }

    Void test_concurrency() {
        ThreadingCommandLineInterface cli=make_cli();

        ARIADNE_TESTING_PROVE(cli.acquire({"","-c","0"}));
        ARIADNE_TESTING_EXPECT_EQUAL(ThreadManager::instance().concurrency(),0u);

        ARIADNE_TESTING_PROVE(cli.acquire({"","--concurrency","0"}));
        ARIADNE_TESTING_EXPECT_EQUAL(ThreadManager::instance().concurrency(),0u);

        ARIADNE_TESTING_PROVE(not cli.acquire({"","-c","-2"}));
        ARIADNE_TESTING_PROVE(not cli.acquire({"","-c","q"}));
        ARIADNE_TESTING_PROVE(not cli.acquire({"","-c"}));

        ARIADNE_TESTING_PROVE(cli.acquire({"","-c","max"}));
        ARIADNE_TESTING_EXPECT_EQUAL(
            ThreadManager::instance().concurrency(),
            ThreadManager::instance().maximum_concurrency());

        ThreadManager::instance().set_concurrency(0);
    }
};

int main(int argc, char** argv) {
    Ariadne::Testing::Suite suite("command_line");
    TestCommandLine::add_cases(suite);
    return Ariadne::Testing::run_suite(suite, argc, argv);
}