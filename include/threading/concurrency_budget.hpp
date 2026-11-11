/***************************************************************************
 *            concurrency_budget.hpp
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

/** @file threading/concurrency_budget.hpp
 *  @brief Declarations and utilities for concurrency budget.
 */
#ifndef THREADING_CONCURRENCY_BUDGET_HPP
#define THREADING_CONCURRENCY_BUDGET_HPP

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>

namespace Ariadne {

/** @brief Thread-safe capacity permits for shared workload admission.
 *  @details This is not CPU affinity or reservation of physical cores.
 *  Permits have RAII ownership and must not outlive their budget.
 */
class ConcurrencyBudget {
  public:
    /** @brief Move-only RAII reservation returned by acquire/try_acquire.
     *  @note The parent budget must outlive every outstanding permit.
     */
    class Permit {
        friend class ConcurrencyBudget;
      public:
        Permit(Permit const&)=delete;
        Permit& operator=(Permit const&)=delete;
        Permit(Permit&& other) noexcept;
        Permit& operator=(Permit&& other) noexcept;
        ~Permit() noexcept;
        /** @brief Return capacity exactly once; safe on moved-from permits. */
        void release() noexcept;
        explicit operator bool() const noexcept { return _budget!=nullptr; }
      private:
        explicit Permit(ConcurrencyBudget* budget) noexcept;
        ConcurrencyBudget* _budget;
    };

    /** @brief Create a nonzero-capacity admission budget.
     *  @throws std::invalid_argument if capacity is zero.
     */
    explicit ConcurrencyBudget(std::size_t capacity);
    ConcurrencyBudget(ConcurrencyBudget const&)=delete;
    ConcurrencyBudget& operator=(ConcurrencyBudget const&)=delete;
    ConcurrencyBudget(ConcurrencyBudget&&)=delete;
    ConcurrencyBudget& operator=(ConcurrencyBudget&&)=delete;

    /** @brief Maximum number of simultaneous permits. */
    std::size_t capacity() const noexcept { return _capacity; }
    /** @brief Available permits at the instant of the synchronized read. */
    std::size_t available() const;
    /** @brief Acquire without blocking; returns empty when exhausted. */
    std::optional<Permit> try_acquire();
    /** @brief Wait for admission or cooperative cancellation.
     *  @param stop Cancellation token; a requested stop returns empty.
     *  @warning Waiting inside a bounded thread pool can deadlock when
     *           the permit releaser is queued behind the waiting task.
     *           Prefer try_acquire() for nested parallelism.
     */
    std::optional<Permit> acquire(std::stop_token stop={});

  private:
    void _release() noexcept;

    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::condition_variable_any _changed;
    std::size_t _available;
};

} // namespace Ariadne

#endif // THREADING_CONCURRENCY_BUDGET_HPP
