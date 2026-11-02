/***************************************************************************
 *            parallel.hpp
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

#ifndef THREADING_PARALLEL_HPP
#define THREADING_PARALLEL_HPP

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "threading/floating_point_environment.hpp"
#include "threading/task_group.hpp"
#include "threading/thread_pool.hpp"

namespace Ariadne {

//! Limits for CPU data parallelism. max_tasks counts the caller as one lane.
//! Zero max_tasks uses at most pool.num_threads()+1 lanes. A lane owns a
//! contiguous partition, keeping task count bounded even for huge ranges.
//! This is a per-call limit, not a reservation against nested submissions.
struct ParallelOptions {
    std::size_t grain_size=256u;
    std::size_t max_tasks=0u;
    std::optional<int> rounding_mode=std::nullopt;
};

namespace parallel_detail {

inline std::size_t lane_count(ThreadPool const& scheduler,
                              std::size_t count,ParallelOptions options) {
    if(options.grain_size==0u)
        throw std::invalid_argument("parallel grain_size must be positive");
    if(count==0u)return 0u;
    const std::size_t chunks=1u+(count-1u)/options.grain_size;
    const std::size_t workers=scheduler.num_threads();
    const std::size_t available=workers+1u; // caller participates
    const std::size_t requested=options.max_tasks==0u
        ? available : std::min(options.max_tasks,available);
    return std::min(chunks,requested);
}

inline std::pair<std::size_t,std::size_t>
lane_interval(std::size_t count,std::size_t lanes,std::size_t lane) {
    const std::size_t base=count/lanes;
    const std::size_t extra=count%lanes;
    const std::size_t begin=lane*base+std::min(lane,extra);
    return {begin,begin+base+(lane<extra ? 1u : 0u)};
}

//! Run the caller lane and join every submitted lane even if either fails.
template<class F>
void execute_lanes(ThreadPool& scheduler,std::size_t lanes,F&& run_lane) {
    if(lanes==1u) {
        run_lane(0u);
        return;
    }
    TaskGroup group(scheduler);
    for(std::size_t lane=1u;lane<lanes;++lane)
        group.fork([&run_lane,lane] { run_lane(lane); });
    std::exception_ptr failure;
    try {
        run_lane(0u);
    } catch (...) {
        failure=std::current_exception();
    }
    try {
        group.join();
    } catch (...) {
        if(!failure)failure=std::current_exception();
    }
    if(failure)std::rethrow_exception(failure);
}

} // namespace parallel_detail

//! Apply fn(index) once for each index in [first,last).
//! A shared fn instance is invoked concurrently: it must be thread-safe.
//! Partitions are static; there is no guarantee of parallel speedup.
template<class F>
void parallel_for(ThreadPool& scheduler,std::size_t first,
                  std::size_t last,F&& fn,ParallelOptions options={}) {
    if(first>last)
        throw std::invalid_argument("parallel_for: first exceeds last");
    const auto count=last-first;
    const auto lanes=parallel_detail::lane_count(scheduler,count,options);
    if(lanes==0u)return;
    auto run_lane=[&](std::size_t lane) {
        ScopedRoundingMode rounding(options.rounding_mode);
        const auto [a,b]=parallel_detail::lane_interval(count,lanes,lane);
        for(std::size_t i=a;i<b;++i)
            std::invoke(fn,first+i);
    };
    parallel_detail::execute_lanes(scheduler,lanes,run_lane);
}

//! Transform each index with map(index), then combine partial results.
//! identity must be a two-sided identity for reduce. Each lane reduces a
//! fixed contiguous partition, and lane results combine in index order.
//! The reduction is deterministic for a fixed lane count, subject to the
//! map/reduce functions and their side effects. Floating-point answers may
//! change with lane count because regrouping changes rounding.
//! Optional rounding_mode applies independently to each execution lane and
//! the final combination, restoring each calling thread on scope exit.
//! Strict floating-point compilation is still required for rigor.
//! map and reduce may be used concurrently: both must be thread-safe.
template<class T,class Map,class Reduce>
T parallel_reduce(ThreadPool& scheduler,std::size_t first,
                  std::size_t last,T identity,Map&& map,Reduce&& reduce,
                  ParallelOptions options={}) {
    if(first>last)
        throw std::invalid_argument("parallel_reduce: first exceeds last");
    const auto count=last-first;
    const auto lanes=parallel_detail::lane_count(scheduler,count,options);
    if(lanes==0u)return identity;
    std::vector<T> partials(lanes,identity);
    auto run_lane=[&](std::size_t lane) {
        ScopedRoundingMode rounding(options.rounding_mode);
        const auto [a,b]=parallel_detail::lane_interval(count,lanes,lane);
        T result=identity;
        for(std::size_t i=a;i<b;++i)
            result=std::invoke(reduce,std::move(result),std::invoke(map,first+i));
        partials[lane]=std::move(result);
    };
    parallel_detail::execute_lanes(scheduler,lanes,run_lane);
    ScopedRoundingMode rounding(options.rounding_mode);
    T result=std::move(partials[0u]);
    for(std::size_t lane=1u;lane<lanes;++lane)
        result=std::invoke(reduce,std::move(result),std::move(partials[lane]));
    return result;
}

} // namespace Ariadne

#endif // THREADING_PARALLEL_HPP
