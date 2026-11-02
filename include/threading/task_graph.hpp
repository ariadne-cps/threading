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

#include <cstddef>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

#include "threading/parallel.hpp"

namespace Ariadne {

/** @brief Static dependency-directed execution graph for CPU tasks.
 *  @details Dependencies enforce ordering, not data movement. Nodes become
 *  runnable as their prerequisites complete; independent nodes do not wait
 *  for a global topological-level barrier. Build and run on one owning thread:
 *  concurrent mutation, destruction, or invocation of run() is unsupported.
 *  The graph can be reused after run() returns.
 */
class TaskGraph {
  public:
    using TaskId=std::size_t;

    /** @brief Add an action and return its stable node identifier.
     *  @tparam F Callable convertible to std::function<void()>.
     *  @param fn Action to execute; must remain valid through run().
     *  @throws std::invalid_argument if the action is empty; the graph
     *          remains unchanged when this precondition is violated.
     *  @note Do not mutate the graph while it is executing.
     */
    template<class F>
    TaskId add_task(F&& fn) {
        // Reject invalid actions before mutating the graph.
        std::function<void()> action(std::forward<F>(fn));
        if(!action)
            throw std::invalid_argument("TaskGraph task cannot be empty");
        const auto id=_nodes.size();
        _nodes.push_back(Node{std::move(action),{}});
        return id;
    }

    /** @brief Require @p prerequisite to finish before @p successor runs.
     *  @throws std::out_of_range for an unknown node identifier.
     *  @throws std::invalid_argument for self or duplicate edges.
     *  @note Cycles are detected by run() before any action is invoked.
     */
    void add_dependency(TaskId prerequisite,TaskId successor);

    /** @brief Number of registered graph nodes. */
    std::size_t size() const noexcept { return _nodes.size(); }

    /** @brief Execute all ready nodes on the scheduler and calling thread.
     *  @param scheduler Scheduler that must outlive this call.
     *  @param options Grain validation, lane limit, rounding mode and optional
     *         shared nonblocking spawn budget.
     *  @param stop Optional cooperative cancellation. Already-started actions
     *         finish; no additional actions are dispatched after stop.
     *  @throws std::logic_error if the graph contains a cycle.
     *  @throws std::invalid_argument if options.grain_size is zero.
     *  @throws Any exception from an action, after all active actions finish.
     *  @note A stop request returns normally after joining active work; it
     *        does not report that every node executed. No execution order is
     *        guaranteed among independently ready nodes.
     */
    void run(ThreadPool& scheduler,ParallelOptions options={1u,0u},
             std::stop_token stop={});

  private:
    struct Node {
        std::function<void()> action;
        std::vector<TaskId> dependencies;
    };
    std::vector<Node> _nodes;
};

} // namespace Ariadne

#endif // THREADING_TASK_GRAPH_HPP
