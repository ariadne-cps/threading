/***************************************************************************
 *            floating_point_environment.hpp
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
 */

/** @file threading/floating_point_environment.hpp
 *  @brief Declarations and utilities for floating point environment.
 */
#ifndef THREADING_FLOATING_POINT_ENVIRONMENT_HPP
#define THREADING_FLOATING_POINT_ENVIRONMENT_HPP

#include <cfenv>
#include <optional>
#include <stdexcept>

namespace Ariadne {

//! Set a rounding mode for the *current* thread and restore it on exit.
//! This covers the thread executing a task; it does not modify other
//! workers' floating-point environments or make reductions associative.
//! The compiler, compiler flags and arithmetic backend must respect dynamic
//! rounding (e.g. strict FP / fenv-aware compilation). This guard alone
//! is insufficient to establish rigor for floating-point expressions.
//! No rounding mode is changed when mode is std::nullopt.
class ScopedRoundingMode {
  public:
    /** @brief Set the calling thread's rounding mode for this scope.
     *  @param mode A supported FE_* rounding constant, or std::nullopt
     *         to leave the floating-point environment unchanged.
     *  @throws std::runtime_error if the existing rounding mode cannot be read.
     *  @throws std::invalid_argument if the requested mode is unsupported.
     */
    explicit ScopedRoundingMode(std::optional<int> mode);

    /** @brief Disallow copying a rounding guard. */
    ScopedRoundingMode(const ScopedRoundingMode &) = delete;
    /** @brief Disallow copy assignment of a rounding guard. */
    ScopedRoundingMode &operator=(const ScopedRoundingMode &) = delete;

    /** @brief Restore the original mode on the same thread. */
    ~ScopedRoundingMode() noexcept;

  private:
    int _previous = 0;
    bool _active = false;
};

} // namespace Ariadne
#endif // THREADING_FLOATING_POINT_ENVIRONMENT_HPP
