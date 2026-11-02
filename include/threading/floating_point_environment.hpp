/***************************************************************************
 *            floating_point_environment.hpp
 *
 *  Copyright  2026  Luca Geretti
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
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
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
    explicit ScopedRoundingMode(std::optional<int> mode) {
        if(!mode)return;
        _previous=std::fegetround();
        if(_previous==-1)
            throw std::runtime_error("Cannot query current FP rounding mode");
        if(std::fesetround(*mode)!=0)
            throw std::invalid_argument("Unsupported FP rounding mode");
        _active=true;
    }

    ScopedRoundingMode(ScopedRoundingMode const&)=delete;
    ScopedRoundingMode& operator=(ScopedRoundingMode const&)=delete;

    ~ScopedRoundingMode() noexcept {
        if(_active)(void)std::fesetround(_previous);
    }

  private:
    int _previous=0;
    bool _active=false;
};

} // namespace Ariadne
#endif // THREADING_FLOATING_POINT_ENVIRONMENT_HPP
