/***************************************************************************
 *            concurrency_budget.hpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
/*
 * This file is part of Threading.
 * Threading is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 or (at your option)
 * any later version. No warranty is provided.
 */
#ifndef THREADING_CONCURRENCY_BUDGET_HPP
#define THREADING_CONCURRENCY_BUDGET_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace Ariadne {

//! Shared admission permits; NOT a reservation of hardware cores.
//! Use try_acquire() from pool tasks when the releaser could be queued
//! behind the waiter: blocking worker threads can deadlock.
//! The budget must outlive all issued Permit objects.
class ConcurrencyBudget {
  public:
    class Permit {
        friend class ConcurrencyBudget;
      public:
        Permit(Permit const&)=delete;
        Permit& operator=(Permit const&)=delete;

        Permit(Permit&& other) noexcept
            : _budget(std::exchange(other._budget,nullptr)) { }

        Permit& operator=(Permit&& other) noexcept {
            if(this!=&other) {
                release();
                _budget=std::exchange(other._budget,nullptr);
            }
            return *this;
        }

        ~Permit() noexcept { release(); }

        void release() noexcept {
            if(auto* budget=std::exchange(_budget,nullptr))
                budget->_release();
        }

        explicit operator bool() const noexcept { return _budget!=nullptr; }

      private:
        explicit Permit(ConcurrencyBudget* budget) noexcept
            : _budget(budget) { }
        ConcurrencyBudget* _budget;
    };

    explicit ConcurrencyBudget(std::size_t capacity)
        : _capacity(capacity),_available(capacity) {
        if(capacity==0u)
            throw std::invalid_argument("ConcurrencyBudget capacity must be positive");
    }

    ConcurrencyBudget(ConcurrencyBudget const&)=delete;
    ConcurrencyBudget& operator=(ConcurrencyBudget const&)=delete;
    ConcurrencyBudget(ConcurrencyBudget&&)=delete;
    ConcurrencyBudget& operator=(ConcurrencyBudget&&)=delete;

    std::size_t capacity() const noexcept { return _capacity; }

    std::size_t available() const {
        std::lock_guard lock(_mutex);
        return _available;
    }

    std::optional<Permit> try_acquire() {
        std::lock_guard lock(_mutex);
        if(_available==0u)return std::nullopt;
        --_available;
        return std::optional<Permit>(Permit(this));
    }

    //! Wait until a permit is available or stop is requested.
    //! Prefer try_acquire() from tasks running on a bounded pool.
    std::optional<Permit> acquire(std::stop_token stop={}) {
        std::unique_lock lock(_mutex);
        _changed.wait(lock,stop,[this] { return _available!=0u; });
        if(stop.stop_requested())return std::nullopt;
        --_available;
        return std::optional<Permit>(Permit(this));
    }

  private:
    void _release() noexcept {
        {
            std::lock_guard lock(_mutex);
            ++_available;
        }
        _changed.notify_one();
    }

    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::condition_variable_any _changed;
    std::size_t _available;
};

} // namespace Ariadne

#endif // THREADING_CONCURRENCY_BUDGET_HPP
