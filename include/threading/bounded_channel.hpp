/***************************************************************************
 *            bounded_channel.hpp
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
#ifndef THREADING_BOUNDED_CHANNEL_HPP
#define THREADING_BOUNDED_CHANNEL_HPP

#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace Ariadne {

//! Capacity-bounded FIFO for communicating between independent jobs.
//! close() rejects further sends and wakes waiters; buffered items remain
//! receivable. Stop requests cancel waits without closing the channel.
//! A stopped receive() leaves buffered messages untouched, unlike try_receive().
//! The owner must ensure that no method is active when destroying the channel.
//! Blocking on a channel from every worker in a pool can starve its producers
//! or consumers: use nonblocking operations or provide dedicated progress.
template<class T>
class BoundedChannel {
  public:
    explicit BoundedChannel(std::size_t capacity) : _capacity(capacity) {
        if(capacity==0u)
            throw std::invalid_argument("BoundedChannel capacity must be positive");
    }

    BoundedChannel(BoundedChannel const&)=delete;
    BoundedChannel& operator=(BoundedChannel const&)=delete;
    BoundedChannel(BoundedChannel&&)=delete;
    BoundedChannel& operator=(BoundedChannel&&)=delete;

    std::size_t capacity() const noexcept { return _capacity; }

    std::size_t size() const {
        std::lock_guard lock(_mutex);
        return _items.size();
    }

    bool closed() const {
        std::lock_guard lock(_mutex);
        return _closed;
    }

    //! Return true only for the first close().
    bool close() {
        bool first_close;
        {
            std::lock_guard lock(_mutex);
            first_close=!_closed;
            _closed=true;
        }
        if(first_close) {
            _not_empty.notify_all();
            _not_full.notify_all();
        }
        return first_close;
    }

    template<class U> requires std::constructible_from<T,U&&>
    bool try_send(U&& item) {
        bool sent=false;
        {
            std::lock_guard lock(_mutex);
            if(!_closed && _items.size()<_capacity) {
                _items.emplace_back(std::forward<U>(item));
                sent=true;
            }
        }
        if(sent)_not_empty.notify_one();
        return sent;
    }

    //! False on cancellation or closure. Failed sends do not move from item.
    template<class U> requires std::constructible_from<T,U&&>
    bool send(U&& item,std::stop_token stop={}) {
        bool sent=false;
        {
            std::unique_lock lock(_mutex);
            _not_full.wait(lock,stop,[&] {
                return _closed || _items.size()<_capacity;
            });
            if(!stop.stop_requested() && !_closed) {
                _items.emplace_back(std::forward<U>(item));
                sent=true;
            }
        }
        if(sent)_not_empty.notify_one();
        return sent;
    }

    std::optional<T> try_receive() {
        std::optional<T> result;
        bool received=false;
        {
            std::lock_guard lock(_mutex);
            if(!_items.empty()) {
                result.emplace(std::move(_items.front()));
                _items.pop_front();
                received=true;
            }
        }
        if(received)_not_full.notify_one();
        return result;
    }

    //! Drain buffered messages after close; then return nullopt.
    //! A requested stop takes precedence over a buffered message.
    std::optional<T> receive(std::stop_token stop={}) {
        std::optional<T> result;
        bool received=false;
        {
            std::unique_lock lock(_mutex);
            _not_empty.wait(lock,stop,[&] {
                return _closed || !_items.empty();
            });
            if(!stop.stop_requested() && !_items.empty()) {
                result.emplace(std::move(_items.front()));
                _items.pop_front();
                received=true;
            }
        }
        if(received)_not_full.notify_one();
        return result;
    }

  private:
    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::condition_variable_any _not_empty;
    std::condition_variable_any _not_full;
    std::deque<T> _items;
    bool _closed=false;
};

} // namespace Ariadne

#endif // THREADING_BOUNDED_CHANNEL_HPP
