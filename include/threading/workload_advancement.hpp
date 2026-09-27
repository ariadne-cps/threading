/***************************************************************************
 *            workload_advancement.hpp
 *
 *  Copyright  2022  Luca Geretti
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

/*! \file workload_advancement.hpp
 *  \brief Synchronised class to manage the status of multiple elements to process
 */

#ifndef THREADING_WORKLOAD_ADVANCEMENT_HPP
#define THREADING_WORKLOAD_ADVANCEMENT_HPP

#include <algorithm>
#include <mutex>

namespace Ariadne {

using std::mutex;

//! \brief Synchronised class to manage the status of multiple elements to process
class WorkloadAdvancement {
  public:
    WorkloadAdvancement(size_t initial = 0);

    //! \brief The elements waiting to be processed
    size_t waiting() const;
    //! \brief The elements under processing
    size_t processing() const;
    //! \brief The completed elements
    size_t completed() const;

    //! \brief All the elements (sum of waiting, processing and completed)
    size_t total() const;

    //! \brief Add n elements to waiting
    void add_to_waiting(size_t n = 1);
    //! \brief Move n waiting to processing
    void add_to_processing(size_t n = 1);
    //! \brief Move n processing to completed
    void add_to_completed(size_t n = 1);

    //! \brief The rate of completion r (0<=r<=1) related to the progress
    double completion_rate() const;

    //! \brief If no other processing remains
    bool has_finished() const;

  private:
    size_t _num_waiting;
    size_t _num_processing;
    size_t _num_completed;

    mutex mutable _mux;
};

} // namespace Ariadne

#endif // THREADING_WORKLOAD_ADVANCEMENT_HPP
