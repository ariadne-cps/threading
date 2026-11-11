/***************************************************************************
 *            parallel_first.hpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
/*
 *  This file is part of Ariadne Threading.
 *  Ariadne Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  Ariadne Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

/** @file threading/parallel_first.hpp
 *  @brief Declarations and utilities for parallel first.
 */
#ifndef THREADING_PARALLEL_FIRST_HPP
#define THREADING_PARALLEL_FIRST_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include "threading/parallel.hpp"

namespace Ariadne {

//! Run up to max_lanes independent candidate searches concurrently.
//! A candidate is fn(index,stop_token), returning std::optional<T>.
//! The first successful publication wins; this is NOT index ordering.
//! All running jobs are joined before return, even on exceptions.
//! Functions must be thread-safe and periodically check the stop token.
//! No time limit or forced interruption is provided. An external stop
//! token can cancel the search, returning an empty optional unless one
//! result was published earlier. Exceptions still propagate after join.
//! Zero max_lanes uses up to pool.num_threads()+1 lanes (caller included).
template<class F>
auto parallel_first_result(ThreadPool& scheduler,std::size_t candidates,
                           F&& fn,std::size_t max_lanes=0u,
                           std::stop_token external_stop={})
    -> std::invoke_result_t<F&,std::size_t,std::stop_token> {
    using Optional=std::invoke_result_t<F&,std::size_t,std::stop_token>;
    static_assert(std::is_same_v<Optional,std::optional<typename Optional::value_type>>,
                  "parallel_first_result requires std::optional<T>");
    using T=typename Optional::value_type;

    std::optional<T> winner;
    if(candidates==0u || external_stop.stop_requested())return winner;

    const auto available=scheduler.num_threads()+1u;
    const auto lanes=std::min(candidates,
        max_lanes==0u ? available : std::min(max_lanes,available));
    std::stop_source stop;
    std::stop_callback propagate(external_stop,[&] {
        stop.request_stop();
    });
    std::atomic<std::size_t> next{0u};
    std::mutex publish_mutex;

    auto run_lane=[&](std::size_t) {
        try {
            while(!stop.stop_requested()) {
                const auto index=next.fetch_add(1u,std::memory_order_relaxed);
                if(index>=candidates)break;
                Optional value=std::invoke(fn,index,stop.get_token());
                if(value) {
                    bool published=false;
                    {
                        std::lock_guard lock(publish_mutex);
                        if(!winner && !stop.stop_requested()) {
                            winner.emplace(std::move(*value));
                            published=true;
                        }
                    }
                    if(published)stop.request_stop();
                    break;
                }
            }
        } catch (...) {
            stop.request_stop();
            throw;
        }
    };
    parallel_detail::execute_lanes(scheduler,lanes,run_lane);
    return winner;
}

} // namespace Ariadne

#endif // THREADING_PARALLEL_FIRST_HPP
