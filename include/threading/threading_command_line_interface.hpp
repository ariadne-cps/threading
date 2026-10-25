/***************************************************************************
 *            threading/threading_command_line_interface.hpp
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

#ifndef ARIADNE_THREADING_COMMAND_LINE_INTERFACE_HPP
#define ARIADNE_THREADING_COMMAND_LINE_INTERFACE_HPP

#include "logging/logging_command_line_interface.hpp"

namespace Ariadne {

//! \brief Logging CLI extended with threading concurrency options.
class ThreadingCommandLineInterface : public LoggingCommandLineInterface {
  public:
    ThreadingCommandLineInterface();
};

} // namespace Ariadne

#endif // ARIADNE_THREADING_COMMAND_LINE_INTERFACE_HPP
