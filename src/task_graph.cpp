/***************************************************************************
 *            task_graph.cpp
 *
 *  Copyright  2026  Luca Geretti
 *
 ****************************************************************************/
#include "threading/task_graph.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>

namespace Ariadne {

void TaskGraph::add_dependency(TaskId prerequisite, TaskId successor) {
    if (prerequisite >= _nodes.size() || successor >= _nodes.size())
        throw std::out_of_range("TaskGraph task ID out of range");
    if (prerequisite == successor)
        throw std::invalid_argument("TaskGraph self-dependency");
    auto& dependencies = _nodes[successor].dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), prerequisite) != dependencies.end())
        throw std::invalid_argument("TaskGraph duplicate dependency");
    dependencies.push_back(prerequisite);
}

void TaskGraph::run(ThreadPool& scheduler, ParallelOptions options,
                    std::stop_token stop) {
    if (options.grain_size == 0u)
        throw std::invalid_argument("TaskGraph grain_size must be positive");

    const auto count = _nodes.size();
    if (count == 0u)
        return;
    std::vector<std::vector<TaskId>> successors(count);
    std::vector<std::size_t> initial(count, 0u);
    for (TaskId id = 0u; id < count; ++id) {
        for (auto dependency: _nodes[id].dependencies)
            successors[dependency].push_back(id);
        initial[id] = _nodes[id].dependencies.size();
    }

    // Kahn validation is complete before invoking application callbacks.
    auto validation = initial;
    std::deque<TaskId> visit;
    for (TaskId i = 0u; i < count; ++i)
        if (validation[i] == 0u)
            visit.push_back(i);
    std::size_t visited = 0u;
    while (!visit.empty()) {
        const auto id = visit.front();
        visit.pop_front();
        ++visited;
        for (auto successor: successors[id])
            if (--validation[successor] == 0u)
                visit.push_back(successor);
    }
    if (visited != count)
        throw std::logic_error("TaskGraph contains a dependency cycle");

    // The graph's ready queue and dependency counters are protected
    // by one lock; all node writes happen-before dependent node reads.
    auto remaining = std::move(initial);
    std::deque<TaskId> ready;
    for (TaskId id = 0u; id < count; ++id)
        if (remaining[id] == 0u)
            ready.push_back(id);
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t completed = 0u;
    bool failed = false;
    std::exception_ptr failure;

    // Locking the same mutex as the wait predicate prevents a stop
    // notification from being lost between predicate check and wait.
    std::stop_callback notify_on_stop(stop, [&] {
        std::lock_guard lock(mutex);
        changed.notify_all();
    });
    parallel_detail::LaneReservation reservation(scheduler, count, options);
    const auto lanes = reservation.lanes();
    auto execute = [&](std::size_t) {
        for (;;) {
            TaskId id{};
            bool has_task = false;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    return failed || stop.stop_requested() ||
                           completed == count || !ready.empty();
                });
                if (!(failed || stop.stop_requested() || completed == count)) {
                    id = ready.front();
                    ready.pop_front();
                    has_task = true;
                }
            }
            if (!has_task)
                return;
            std::exception_ptr error;
            try {
                ScopedRoundingMode rounding(options.rounding_mode);
                _nodes[id].action();
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard lock(mutex);
                ++completed;
                if (error && !failed) {
                    failed = true;
                    failure = error;
                }
                if (!failed && !stop.stop_requested())
                    for (auto successor: successors[id])
                        if (--remaining[successor] == 0u)
                            ready.push_back(successor);
            }
            changed.notify_all();
        }
    };
    parallel_detail::execute_lanes(scheduler, lanes, execute);
    if (failure)
        std::rethrow_exception(failure);
}

} // namespace Ariadne
