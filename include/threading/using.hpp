/***************************************************************************
 *            using.hpp
 *
 *  Copyright  2023  Luca Geretti
 *
 ****************************************************************************/

/*
 *  This file is part of Ariadne Threading.
 *
 *  Ariadne Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Ariadne Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Ariadne Threading. If not, see <https://www.gnu.org/licenses/>.
 */

/*! \file using.hpp
 *  \brief Header for common using directives.
 */

#ifndef THREADING_USING_HPP
#define THREADING_USING_HPP

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace Ariadne {

using std::condition_variable;
using std::future;
using std::lock_guard;
using std::mutex;
using std::packaged_task;
using std::promise;
using std::shared_ptr;
using std::thread;
using std::unique_lock;

} // namespace Ariadne

#endif // THREADING_USING_HPP
