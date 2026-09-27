/***************************************************************************
 *            workload_advancement.cpp
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

#include "utility/macros.hpp"
#include "threading/workload_advancement.hpp"

namespace Ariadne {

using std::lock_guard;

WorkloadAdvancement::WorkloadAdvancement(size_t initial) : _num_waiting(initial), _num_processing(0), _num_completed(0) { }

size_t WorkloadAdvancement::waiting() const {
    lock_guard<mutex> lock(_mux);
    return _num_waiting;
}

size_t WorkloadAdvancement::processing() const {
    lock_guard<mutex> lock(_mux);
    return _num_processing;
}

size_t WorkloadAdvancement::completed() const {
    lock_guard<mutex> lock(_mux);
    return _num_completed;
}

size_t WorkloadAdvancement::total() const {
    lock_guard<mutex> lock(_mux);
    return _num_waiting + _num_processing + _num_completed;
}

void WorkloadAdvancement::add_to_waiting(size_t n) {
    ARIADNE_PRECONDITION(n > 0);
    lock_guard<mutex> lock(_mux);
    _num_waiting+=n;
}

void WorkloadAdvancement::add_to_processing(size_t n) {
    lock_guard<mutex> lock(_mux);
    ARIADNE_PRECONDITION(n <= _num_waiting);
    _num_waiting-=n;
    _num_processing+=n;
}

void WorkloadAdvancement::add_to_completed(size_t n) {
    lock_guard<mutex> lock(_mux);
    ARIADNE_PRECONDITION(n <=_num_processing);
    _num_processing-=n;
    _num_completed+=n;
}

double WorkloadAdvancement::completion_rate() const {
    lock_guard<mutex> lock(_mux);
    auto total = static_cast<double>(_num_waiting + _num_processing + _num_completed);
    return total == 0.0 ? 1.0 : static_cast<double>(_num_completed) / total;
}

bool WorkloadAdvancement::has_finished() const {
    lock_guard<mutex> lock(_mux);
    return _num_processing == 0 and _num_waiting == 0;
}

} // namespace Ariadne
