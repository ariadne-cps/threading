/***************************************************************************
 *            buffer.hpp
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

/*! \file buffer.hpp
 *  \brief A multiple-thread-safe queue usable as a buffer.
 */

#ifndef THREADING_BUFFER_HPP
#define THREADING_BUFFER_HPP

#include <utility>
#include <mutex>
#include <condition_variable>
#include <queue>
#include "utility/macros.hpp"
#include "threading/using.hpp"

namespace Ariadne {

//! \brief Exception useful when the buffer is allowed to stay in the receiving condition
class BufferInterruptPullingException : public std::exception { };

//! \brief A class for handling a buffer
template<class E> class Buffer
{
  public:
    Buffer(size_t capacity) : _capacity(capacity), _interrupt(false) { ARIADNE_PRECONDITION(capacity > 0); }

    //! \brief Push an object into the buffer
    //! \details Will block if the capacity has been reached
    void push(E const& e) {
        unique_lock<mutex> locker(mux);
        _not_full.wait(locker, [this](){return _queue.size() < _capacity;});
        _queue.push(e);
        locker.unlock();
        _not_empty.notify_one();
    }

    //! \brief Pulls an object from the buffer
    //! \details Will block if the capacity is zero
    E pull() {
        unique_lock<mutex> locker(mux);
        _not_empty.wait(locker, [this](){return not _queue.empty() || _interrupt;});
        if (_interrupt and _queue.empty()) { _interrupt = false; throw BufferInterruptPullingException(); }
        E back = _queue.front();
        _queue.pop();
        locker.unlock();
        _not_full.notify_one();
        return back;
    }

    //! \brief The current size of the queue
    size_t size() const {
        lock_guard<mutex> locker(mux);
        return _queue.size();
    }

    //! \brief The maximum size for the queue
    size_t capacity() const {
        lock_guard<mutex> locker(mux);
        return _capacity;
    }

    //! \brief Change the capacity
    void set_capacity(size_t capacity) {
        ARIADNE_PRECONDITION(capacity>0);
        lock_guard<mutex> locker(mux);
        ARIADNE_ASSERT_MSG(capacity>=_queue.size(),"Reducing capacity below currenty buffer size is not allowed.");
        _capacity = capacity;
        _not_full.notify_all();
    }

    //! \brief Interrupt consuming in the case that the queue is empty and the buffer in the waiting state for input
    //! \details Needs to
    void interrupt_consuming() {
        {
            lock_guard<mutex> locker(mux);
            _interrupt = true;
        }
        _not_empty.notify_all();
    }

private:
    mutable mutex mux;
    condition_variable _not_empty;
    condition_variable _not_full;
    std::queue<E> _queue;
    size_t _capacity;
    bool _interrupt;
};

} // namespace Ariadne

#endif // THREADING_BUFFER_HPP
