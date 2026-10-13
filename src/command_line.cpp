/***************************************************************************
 *            command_line.cpp
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
 *
 *  Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Threading.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "threading/command_line.hpp"
#include "threading/thread_manager.hpp"

namespace Ariadne {

CommandLineContribution threading_command_line() {
    return CommandLineContribution(
        "threading",
        {
            make_valued_argument_parser(
                "c",
                "concurrency",
                "Choose the concurrency as a non-negative integer <value> or use 'max' to choose the hardware concurrency of this machine (default: 0)",
                [](String const& value) -> VoidFunction {
                    if (value == "max") {
                        return [] {
                            ThreadManager::instance().set_maximum_concurrency();
                        };
                    }

                    Int const concurrency=std::stoi(value);
                    if (concurrency < 0) {
                        throw std::runtime_error("negative concurrency");
                    }
                    return [concurrency] {
                        ThreadManager::instance().set_concurrency(
                            static_cast<SizeType>(concurrency));
                    };
                })
        });
}

} // namespace Ariadne
