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

#include "threading/command_line.hpp"
#include "threading/thread_manager.hpp"
#include "macro/testing.hpp"

using namespace Ariadne;

class TestCommandLine {
  public:
    Void test() {
        ARIADNE_TEST_CALL(test_contribution())
        ARIADNE_TEST_CALL(test_concurrency())
    }

  private:
    CommandLineInterface make_cli() const {
        CommandLineInterface cli;
        cli.add(threading_command_line());
        return cli;
    }

    Void test_contribution() {
        CommandLineContribution const contribution=threading_command_line();
        ARIADNE_TEST_EQUALS(contribution.name(),"threading")
        ARIADNE_TEST_EQUALS(contribution.parsers().size(),1u)
        ARIADNE_TEST_EQUALS(contribution.parsers()[0].short_id(),"c")
        ARIADNE_TEST_EQUALS(contribution.parsers()[0].long_id(),"concurrency")
    }

    Void test_concurrency() {
        CommandLineInterface cli=make_cli();

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
