/***************************************************************************
 *            floating_point_environment.cpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
#include "threading/floating_point_environment.hpp"

namespace Ariadne {

ScopedRoundingMode::ScopedRoundingMode(std::optional<int> mode) {
        if(!mode)return;
        _previous=std::fegetround();
        if(_previous==-1)
            throw std::runtime_error("Cannot query current FP rounding mode");
        if(std::fesetround(*mode)!=0)
            throw std::invalid_argument("Unsupported FP rounding mode");
        _active=true;
    }

ScopedRoundingMode::~ScopedRoundingMode() noexcept {
        if(_active)(void)std::fesetround(_previous);
    }

} // namespace Ariadne
