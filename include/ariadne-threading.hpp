/***************************************************************************
 *            ariadne-threading.hpp
 *
 *  Copyright  2026  Luca Geretti
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

/** @file ariadne-threading.hpp
 *  @brief Public umbrella header for Ariadne Threading.
 */

#ifndef ARIADNE_THREADING_UMBRELLA_HPP
#define ARIADNE_THREADING_UMBRELLA_HPP

#include "ariadne-logging.hpp"

#include "threading/bounded_channel.hpp"
#include "threading/concurrency_budget.hpp"
#include "threading/buffer.hpp"
#include "threading/floating_point_environment.hpp"
#include "threading/parallel.hpp"
#include "threading/parallel_first.hpp"
#include "threading/task_graph.hpp"
#include "threading/stop_task_group.hpp"
#include "threading/buffered_thread.hpp"
#include "threading/task_group.hpp"
#include "threading/templates.hpp"
#include "threading/thread.hpp"
#include "threading/thread_manager.hpp"
#include "threading/thread_pool.hpp"
#include "threading/threading_command_line_interface.hpp"
#include "threading/using.hpp"
#include "threading/workload.hpp"
#include "threading/workload_advancement.hpp"
#include "threading/workload_observation.hpp"
#include "threading/workload_interface.hpp"

#endif // ARIADNE_THREADING_UMBRELLA_HPP
