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
//! The initial executor uses level barriers (not dynamic ready queues):
//! an unrelated node in a later wave may wait for a slow earlier wave.
//! No execution order is guaranteed inside one wave.
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

    //! Every task in a wave finishes before dependent waves start.
    //! On error, all tasks already started in that wave are joined;
    //! later waves are not executed. The caller is one execution lane.
    void run(ThreadPool& scheduler,ParallelOptions options={1u,0u}) {
        if(options.grain_size==0u)
            throw std::invalid_argument("TaskGraph grain_size must be positive");

        const auto count=_nodes.size();
        std::vector<std::vector<TaskId>> successors(count);
        std::vector<std::size_t> remaining(count,0u);
        for(TaskId id=0u;id<count;++id) {
            for(auto dependency:_nodes[id].dependencies)
                successors[dependency].push_back(id);
            remaining[id]=_nodes[id].dependencies.size();
        }

        std::vector<std::vector<TaskId>> waves;
        std::vector<TaskId> ready;
        ready.reserve(count);
        for(TaskId id=0u;id<count;++id)
            if(remaining[id]==0u)ready.push_back(id);

        std::size_t visited=0u;
        while(!ready.empty()) {
            visited+=ready.size();
            waves.push_back(std::move(ready));
            ready.clear();
            for(auto id:waves.back())
                for(auto successor:successors[id])
                    if(--remaining[successor]==0u)
                        ready.push_back(successor);
        }
        if(visited!=count)
            throw std::logic_error("TaskGraph contains a dependency cycle");

        for(auto const& wave:waves) {
            parallel_for(scheduler,0u,wave.size(),[&](std::size_t offset) {
                _nodes[wave[offset]].action();
            },options);
        }
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
