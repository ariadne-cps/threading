/***************************************************************************
 *            concurrency_budget.cpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
#include "threading/concurrency_budget.hpp"
#include <stdexcept>
#include <utility>

namespace Ariadne {

ConcurrencyBudget::Permit::Permit(ConcurrencyBudget* budget) noexcept
    : _budget(budget) {}

ConcurrencyBudget::Permit::Permit(Permit&& other) noexcept
    : _budget(std::exchange(other._budget,nullptr)) {}

ConcurrencyBudget::Permit& ConcurrencyBudget::Permit::operator=(Permit&& other) noexcept {
    if(this!=&other) {
        release();
        _budget=std::exchange(other._budget,nullptr);
    }
    return *this;
}

ConcurrencyBudget::Permit::~Permit() noexcept { release(); }

void ConcurrencyBudget::Permit::release() noexcept {
    if(auto* budget=std::exchange(_budget,nullptr))budget->_release();
}

ConcurrencyBudget::ConcurrencyBudget(std::size_t capacity)
    : _capacity(capacity),_available(capacity) {
    if(capacity==0u)
        throw std::invalid_argument("ConcurrencyBudget capacity must be positive");
}

std::size_t ConcurrencyBudget::available() const {
    std::lock_guard lock(_mutex);
    return _available;
}

std::optional<ConcurrencyBudget::Permit> ConcurrencyBudget::try_acquire() {
    std::lock_guard lock(_mutex);
    if(_available==0u)return std::nullopt;
    --_available;
    return std::optional<Permit>(Permit(this));
}

std::optional<ConcurrencyBudget::Permit>
ConcurrencyBudget::acquire(std::stop_token stop) {
    std::unique_lock lock(_mutex);
    _changed.wait(lock,stop,[this] { return _available!=0u; });
    if(stop.stop_requested())return std::nullopt;
    --_available;
    return std::optional<Permit>(Permit(this));
}

void ConcurrencyBudget::_release() noexcept {
    {
        std::lock_guard lock(_mutex);
        ++_available;
    }
    _changed.notify_one();
}

} // namespace Ariadne
