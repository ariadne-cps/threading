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
#include "macro/testing.hpp"

using namespace Ariadne;

class TestCommandLine {
  public:
    Void test() {
        ARIADNE_TEST_CALL(test_inherited_logging())
        ARIADNE_TEST_CALL(test_concurrency())
    }

  private:
    ThreadingCommandLineInterface make_cli() const {
        return ThreadingCommandLineInterface();
    }

    Void test_inherited_logging() {
        ThreadingCommandLineInterface cli;
        ARIADNE_TEST_ASSERT(cli.acquire({""}))
        ARIADNE_TEST_ASSERT(cli.acquire({"","--scheduler","immediate","--verbosity","2","--concurrency","0","--theme","none"}))
        ARIADNE_TEST_EQUALS(Logger::instance().configuration().verbosity(),2u)
        ARIADNE_TEST_ASSERT(not Logger::instance().configuration().theme().has_style())
        ARIADNE_TEST_EQUALS(ThreadManager::instance().concurrency(),0u)
        ARIADNE_TEST_ASSERT(not cli.acquire({"","--concurrency","invalid"}))
        Bool custom=false;
        cli.add(CommandLineContribution("custom", {
            make_unvalued_argument_parser("x","custom","Test inherited extension", [&custom] {custom=true;})
        }));
        ARIADNE_TEST_ASSERT(cli.acquire({"","--custom"}))
        ARIADNE_TEST_ASSERT(custom)
        Logger::instance().use_nonblocking_scheduler();
    }

    Void test_concurrency() {
        ThreadingCommandLineInterface cli=make_cli();

        ARIADNE_TEST_ASSERT(cli.acquire({"","-c","0"}))
        ARIADNE_TEST_EQUALS(ThreadManager::instance().concurrency(),0u)

        ARIADNE_TEST_ASSERT(cli.acquire({"","--concurrency","0"}))
        ARIADNE_TEST_EQUALS(ThreadManager::instance().concurrency(),0u)

        ARIADNE_TEST_ASSERT(not cli.acquire({"","-c","-2"}))
        ARIADNE_TEST_ASSERT(not cli.acquire({"","-c","q"}))
        ARIADNE_TEST_ASSERT(not cli.acquire({"","-c"}))

        ARIADNE_TEST_ASSERT(cli.acquire({"","-c","max"}))
        ARIADNE_TEST_EQUALS(
            ThreadManager::instance().concurrency(),
            ThreadManager::instance().maximum_concurrency())

        ThreadManager::instance().set_concurrency(0);
    }
};

Int main() {
    TestCommandLine().test();
    return ARIADNE_TEST_FAILURES;
}
