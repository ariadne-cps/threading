/***************************************************************************
 *            workload_observation.hpp
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

/** @file threading/workload_observation.hpp
 *  @brief Declarations and utilities for workload observation.
 */

#ifndef THREADING_WORKLOAD_OBSERVATION_HPP
#define THREADING_WORKLOAD_OBSERVATION_HPP

#include <cstddef>

namespace Ariadne {

//! One coherent view of a workload's progress counters.
//! A task that throws is still counted as completed once its execution ends.
//! Dynamic workloads may increase total() after the snapshot was captured.
//! Completed does not imply that the task succeeded.
struct WorkloadSnapshot {
    std::size_t waiting = 0u;
    std::size_t processing = 0u;
    std::size_t completed = 0u;

    /** @brief Return the total number of waiting, processing and completed items. */
    std::size_t total() const noexcept {
        return waiting + processing + completed;
    }

    double completion_rate() const noexcept {
        const auto count = total();
        return count == 0u ? 1.0 : static_cast<double>(completed) / static_cast<double>(count);
    }

    /** @brief Report whether any work remains outstanding. */
    bool has_finished() const noexcept {
        return waiting == 0u && processing == 0u;
    }
};

//! Read-only, application-independent observation contract.
//! Implementations synchronize their own state. snapshot() never invokes
//! Logging or user callbacks. Concurrent snapshots can show different
//! progress states but each returned snapshot must be internally consistent.
//! An observer must not outlive the source object being inspected.
class WorkloadObservable {
  public:
    /** @brief Destroy a workload observer interface. */
    virtual ~WorkloadObservable() = default;
    /** @brief Capture an internally consistent progress snapshot. */
    virtual WorkloadSnapshot snapshot() const = 0;
};

} // namespace Ariadne

#endif // THREADING_WORKLOAD_OBSERVATION_HPP
