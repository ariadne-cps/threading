/***************************************************************************
 *            task_graph.hpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
/*
 *  This file is part of Threading.
 *  Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */
#ifndef THREADING_TASK_GRAPH_HPP
#define THREADING_TASK_GRAPH_HPP

#include <algorithm>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "threading/parallel.hpp"

namespace Ariadne {

//! Static CPU DAG of void tasks, e.g. independent DNN operations.
//! The owner constructs the graph and runs it; concurrent mutation,
//! destruction, or repeated runs are not supported. Each node executes
//! at most once per run. The graph may be reused after run() returns.
//! Each dependency is an ordering edge, not a data transport channel.
//! Cycle validation completes before any task is invoked.
//! Ready nodes may run as soon as their own predecessors complete;
//! unrelated slow nodes impose no global level barrier.
//! No order is guaranteed between independent nodes.
class TaskGraph {
  public:
    using TaskId=std::size_t;

    template<class F>
    TaskId add_task(F&& fn) {
        const auto id=_nodes.size();
        _nodes.push_back(Node{std::function<void()>(std::forward<F>(fn)),{}});
        if(!_nodes.back().action)
            throw std::invalid_argument("TaskGraph task cannot be empty");
        return id;
    }

    void add_dependency(TaskId prerequisite,TaskId successor) {
        if(prerequisite>=_nodes.size() || successor>=_nodes.size())
            throw std::out_of_range("TaskGraph task ID out of range");
        if(prerequisite==successor)
            throw std::invalid_argument("TaskGraph self-dependency");
        auto& dependencies=_nodes[successor].dependencies;
        if(std::find(dependencies.begin(),dependencies.end(),prerequisite)
                !=dependencies.end())
            throw std::invalid_argument("TaskGraph duplicate dependency");
        dependencies.push_back(prerequisite);
    }

    std::size_t size() const noexcept { return _nodes.size(); }

    //! Validate cycles before running; workers then consume a shared ready
    //! queue. When any node throws, active nodes finish, but no more nodes
    //! are dispatched. The first observed exception is propagated.
    //! The caller is one execution lane.
    void run(ThreadPool& scheduler,ParallelOptions options={1u,0u}) {
        if(options.grain_size==0u)
            throw std::invalid_argument("TaskGraph grain_size must be positive");

        const auto count=_nodes.size();
        if(count==0u)return;
        std::vector<std::vector<TaskId>> successors(count);
        std::vector<std::size_t> initial(count,0u);
        for(TaskId id=0u;id<count;++id) {
            for(auto dependency:_nodes[id].dependencies)
                successors[dependency].push_back(id);
            initial[id]=_nodes[id].dependencies.size();
        }

        // Kahn validation is complete before invoking application callbacks.
        auto validation=initial;
        std::deque<TaskId> visit;
        for(TaskId i=0u;i<count;++i)
            if(validation[i]==0u)visit.push_back(i);
        std::size_t visited=0u;
        while(!visit.empty()) {
            const auto id=visit.front();
            visit.pop_front();
            ++visited;
            for(auto successor:successors[id])
                if(--validation[successor]==0u)
                    visit.push_back(successor);
        }
        if(visited!=count)
            throw std::logic_error("TaskGraph contains a dependency cycle");

        // The graph's ready queue and dependency counters are protected
        // by one lock; all node writes happen-before dependent node reads.
        auto remaining=std::move(initial);
        std::deque<TaskId> ready;
        for(TaskId id=0u;id<count;++id)
            if(remaining[id]==0u)ready.push_back(id);
        std::mutex mutex;
        std::condition_variable changed;
        std::size_t completed=0u;
        bool failed=false;
        std::exception_ptr failure;

        const auto lanes=parallel_detail::lane_count(scheduler,count,options);
        auto execute=[&](std::size_t) {
            for(;;) {
                TaskId id;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock,[&] {
                        return failed || completed==count || !ready.empty();
                    });
                    if(failed || completed==count)return;
                    id=ready.front();
                    ready.pop_front();
                }
                std::exception_ptr error;
                try {
                    ScopedRoundingMode rounding(options.rounding_mode);
                    _nodes[id].action();
                } catch (...) {
                    error=std::current_exception();
                }
                {
                    std::lock_guard lock(mutex);
                    ++completed;
                    if(error && !failed) {
                        failed=true;
                        failure=error;
                    }
                    if(!failed)
                        for(auto successor:successors[id])
                            if(--remaining[successor]==0u)
                                ready.push_back(successor);
                }
                changed.notify_all();
            }
        };
        parallel_detail::execute_lanes(scheduler,lanes,execute);
        if(failure)std::rethrow_exception(failure);
    }

  private:
    struct Node {
        std::function<void()> action;
        std::vector<TaskId> dependencies;
    };
    std::vector<Node> _nodes;
};

} // namespace Ariadne

#endif // THREADING_TASK_GRAPH_HPP
