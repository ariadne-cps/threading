/***************************************************************************
 *            using.hpp
 *
 *  Copyright  2023  Luca Geretti
 *
 ****************************************************************************/

/*
 *  This file is part of Threading.
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

/*! \file using.hpp
 *  \brief Header for common using directives.
 */

#ifndef THREADING_USING_HPP
#define THREADING_USING_HPP

#include <utility>
#include <mutex>
#include <condition_variable>
#include <future>
#include <thread>

namespace Ariadne {

using std::future;
using std::promise;
using std::unique_lock;
using std::lock_guard;
using std::condition_variable;
using std::packaged_task;
using std::mutex;
using std::shared_ptr;
using std::thread;

} // namespace Ariadne

#endif // THREADING_USING_HPP
