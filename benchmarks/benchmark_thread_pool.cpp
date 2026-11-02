/***************************************************************************
 *            benchmark_thread_pool.cpp
 *
 *  Copyright 2026 Luca Geretti
 *
 ****************************************************************************/
/*
 * This file is part of Threading.
 *
 * Threading is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Threading is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Threading. If not, see <https://www.gnu.org/licenses/>.
 */
#include "threading/thread_pool.hpp"
#include "threading/task_group.hpp"
#include "threading/thread_manager.hpp"
#include "threading/threading_command_line_interface.hpp"
#include "logging/logging.hpp"

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Ariadne;

namespace {

struct Options {
    unsigned tasks = 4096u;
    unsigned work = 128u;
    unsigned depth = 9u;
    unsigned cutoff_depth = 0u;
    unsigned samples = 9u;
    std::string mode = "both";
    bool sweep = false;
    bool cutoff_sweep = false;
};

template<class UInt>
UInt parse_unsigned(String const& value) {
    const std::string text(value);
    if(text.empty() || text.front()=='-')
        throw std::invalid_argument("expected a nonnegative integer");
    std::size_t used = 0u;
    const auto parsed = std::stoull(text,&used);
    if(used != text.size() || parsed > std::numeric_limits<UInt>::max())
        throw std::invalid_argument("integer out of range");
    return static_cast<UInt>(parsed);
}

List<ArgumentParser> benchmark_parsers(Options& options) {
    List<ArgumentParser> parsers;
    parsers.append(make_valued_argument_parser(
        "j","tasks","Independent task count (1..1000000; default 4096)",
        [&options](String const& value) -> VoidFunction {
            const unsigned n = parse_unsigned<unsigned>(value);
            if(n==0u || n>1000000u)
                throw std::invalid_argument("task count out of range");
            return [&options,n] { options.tasks=n; };
        }));
    parsers.append(make_valued_argument_parser(
        "w","work","Integer mixing rounds per leaf (1..1000000; default 128)",
        [&options](String const& value) -> VoidFunction {
            const unsigned n = parse_unsigned<unsigned>(value);
            if(n==0u || n>1000000u)
                throw std::invalid_argument("work count out of range");
            return [&options,n] { options.work=n; };
        }));
    parsers.append(make_valued_argument_parser(
        "d","depth","Recursive binary tree depth (0..14; default 9)",
        [&options](String const& value) -> VoidFunction {
            const unsigned n = parse_unsigned<unsigned>(value);
            if(n>14u)
                throw std::invalid_argument("recursive depth out of range");
            return [&options,n] { options.depth=n; };
        }));
    parsers.append(make_valued_argument_parser(
        "q","cutoff-depth","Stop forking at or below this remaining depth (0..14; default 0)",
        [&options](String const& value) -> VoidFunction {
            const unsigned n=parse_unsigned<unsigned>(value);
            if(n>14u)
                throw std::invalid_argument("cutoff depth out of range");
            return [&options,n] { options.cutoff_depth=n; };
        }));
    parsers.append(make_valued_argument_parser(
        "n","samples","Odd sample count (3..101; default 9), plus warm-up",
        [&options](String const& value) -> VoidFunction {
            const unsigned n = parse_unsigned<unsigned>(value);
            if(n<3u || n>101u || n%2u==0u)
                throw std::invalid_argument("samples must be odd, in [3,101]");
            return [&options,n] { options.samples=n; };
        }));
    parsers.append(make_valued_argument_parser(
        "m","mode","Workload: flat, recursive, micro, breakdown, handle-compare, both (default both)",
        [&options](String const& value) -> VoidFunction {
            const std::string mode(value);
            if(mode!="flat" && mode!="recursive" && mode!="micro" && mode!="breakdown" && mode!="handle-compare" && mode!="both")
                throw std::invalid_argument("invalid mode");
            return [&options,mode] { options.mode=mode; };
        }));
    parsers.append(make_unvalued_argument_parser(
        "","sweep","Compare 1,2,4,8 workers x work=16,128,1024",
        [&options] { options.sweep=true; }));
    parsers.append(make_unvalued_argument_parser(
        "","cutoff-sweep","Compare recursive cutoffs 0,2,...,depth at fixed workers/work",
        [&options] { options.cutoff_sweep=true; }));
    return parsers;
}

// Deterministic unsigned arithmetic; modulo-2^64 overflow is intentional.
std::uint64_t work_kernel(std::uint64_t value,unsigned rounds) {
    for(unsigned i=0u; i<rounds; ++i) {
        value ^= value >> 30;
        value *= UINT64_C(0xbf58476d1ce4e5b9);
        value ^= value >> 27;
        value *= UINT64_C(0x94d049bb133111eb);
        value ^= value >> 31;
    }
    return value;
}

constexpr std::uint64_t left_tag=UINT64_C(0x9e3779b97f4a7c15);
constexpr std::uint64_t right_tag=UINT64_C(0xd1b54a32d192ed03);

std::uint64_t flat_reference(unsigned tasks,unsigned work) {
    std::uint64_t total=0;
    for(unsigned i=0u;i<tasks;++i)
        total+=work_kernel(left_tag+static_cast<std::uint64_t>(i),work);
    return total;
}

template<class Scheduler>
std::uint64_t flat_run(Scheduler& scheduler,unsigned tasks,unsigned work) {
    TaskGroup<Scheduler> group(scheduler);
    std::vector<TaskHandle<Scheduler,std::uint64_t>> handles;
    handles.reserve(tasks);
    for(unsigned i=0u;i<tasks;++i) {
        handles.push_back(group.fork([i,work] {
            return work_kernel(left_tag+static_cast<std::uint64_t>(i),work);
        }));
    }
    std::uint64_t total=0;
    for(auto const& handle:handles)total+=handle.get();
    group.join();
    return total;
}

std::uint64_t combine(std::uint64_t left,std::uint64_t right) {
    return (left ^ std::rotl(right,17)) + left_tag;
}

std::uint64_t recursive_reference(unsigned depth,std::uint64_t seed,unsigned work) {
    if(depth==0u)return work_kernel(seed,work);
    const auto left=recursive_reference(depth-1u,seed^left_tag,work);
    const auto right=recursive_reference(depth-1u,seed+right_tag,work);
    return combine(left,right);
}

template<class Scheduler>
std::uint64_t recursive_run(Scheduler& scheduler,unsigned depth,
                            std::uint64_t seed,unsigned work,
                            unsigned cutoff_depth) {
    // Evaluate the entire remaining subtree with the original reference
    // recurrence. Only scheduling changes; every leaf and combine remains.
    if(depth<=cutoff_depth)
        return recursive_reference(depth,seed,work);
    TaskGroup<Scheduler> group(scheduler);
    auto left=group.fork([&scheduler,depth,seed,work,cutoff_depth] {
        return recursive_run(scheduler,depth-1u,seed^left_tag,work,cutoff_depth);
    });
    const auto right=recursive_run(scheduler,depth-1u,seed+right_tag,work,cutoff_depth);
    const auto result=combine(left.get(),right);
    group.join();
    return result;
}

// Single-child structured fork/join, matching the BDD recursion pattern:
// fork one branch, compute independent work locally, then join.
std::uint64_t micro_reference(unsigned tasks,unsigned work) {
    std::uint64_t total=0;
    for(unsigned i=0u;i<tasks;++i) {
        const auto seed=static_cast<std::uint64_t>(i);
        const auto left=work_kernel(left_tag+seed,work);
        const auto right=work_kernel(right_tag+seed,work);
        total+=combine(left,right);
    }
    return total;
}

std::uint64_t micro_run(ThreadPool& scheduler,unsigned tasks,unsigned work) {
    std::uint64_t total=0;
    for(unsigned i=0u;i<tasks;++i) {
        TaskGroup group(scheduler);
        auto left=group.fork([i,work] {
            return work_kernel(left_tag+static_cast<std::uint64_t>(i),work);
        });
        const auto right=work_kernel(right_tag+static_cast<std::uint64_t>(i),work);
        total+=combine(left.get(),right);
        group.join();
    }
    return total;
}

template<class F>
double timed_sample(F&& compute,std::uint64_t reference) {
    const auto start=std::chrono::steady_clock::now();
    const auto result=compute();
    const auto finish=std::chrono::steady_clock::now();
    if(result!=reference)
        throw std::runtime_error("checksum mismatch between scheduler and reference");
    return std::chrono::duration<double,std::milli>(finish-start).count();
}

double median(std::vector<double> values) {
    if(values.empty())throw std::runtime_error("empty timing collection");
    std::sort(values.begin(),values.end());
    return values[values.size()/2u];
}

struct Statistics {
    double median_ms;
    double minimum_ms;
    double maximum_ms;
    double mad_ms;
};

Statistics statistics(std::vector<double> const& values) {
    if(values.empty())throw std::runtime_error("no benchmark samples");
    for(double value:values)
        if(!(std::isfinite(value) && value>0.0))
            throw std::runtime_error("nonpositive or nonfinite sample");
    const double middle=median(values);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for(double value:values)deviations.push_back(std::abs(value-middle));
    const auto bounds=std::minmax_element(values.begin(),values.end());
    return {middle,*bounds.first,*bounds.second,median(std::move(deviations))};
}

// Construct exactly one pool at a time, keeping all other pool workers
// absent. Worker startup, a complete warm-up and destruction are excluded
// from the clock. Every measured sample uses a freshly warmed pool.
double measure_pool(std::string const& mode, Options const& options,
                    unsigned workers, std::uint64_t reference) {
    ThreadPool scheduler(workers);
    auto run=[&]() -> std::uint64_t {
        if(mode=="flat")return flat_run(scheduler,options.tasks,options.work);
        return recursive_run(scheduler,options.depth,left_tag,options.work,
                             options.cutoff_depth);
    };
    if(run()!=reference)
        throw std::runtime_error("warm-up checksum mismatch");
    if(scheduler.queue_size()!=0u)
        throw std::runtime_error("warm-up left queued tasks");
    const double elapsed=timed_sample(run,reference);
    if(scheduler.queue_size()!=0u)
        throw std::runtime_error("timed sample left queued tasks");
    return elapsed;
}

Statistics compare(std::string const& mode, Options const& options,
                   unsigned workers) {
    const auto reference=mode=="flat"
        ? flat_reference(options.tasks,options.work)
        : recursive_reference(options.depth,left_tag,options.work);
    std::vector<double> samples;
    samples.reserve(options.samples);
    for(unsigned sample=0u;sample<options.samples;++sample)
        samples.push_back(measure_pool(mode,options,workers,reference));
    return statistics(samples);
}

struct MicroComparison {
    Statistics sequential;
    Statistics fork_join;
    double paired_delta_ns_per_fork;
};

// Measure identical work with/without a fork, alternating run order.
// Differences include task construction, scheduling and joining; they are
// not a direct measurement of any one ThreadPool primitive.
MicroComparison compare_micro(Options const& options,unsigned workers) {
    const auto reference=micro_reference(options.tasks,options.work);
    std::vector<double> sequential_samples, fork_samples, paired_deltas;
    sequential_samples.reserve(options.samples);
    fork_samples.reserve(options.samples);
    paired_deltas.reserve(options.samples);

    for(unsigned sample=0u;sample<options.samples;++sample) {
        ThreadPool scheduler(workers);
        auto sequential=[&] { return micro_reference(options.tasks,options.work); };
        auto fork_join=[&] { return micro_run(scheduler,options.tasks,options.work); };
        if(fork_join()!=reference || sequential()!=reference)
            throw std::runtime_error("micro warm-up checksum mismatch");
        if(scheduler.queue_size()!=0u)
            throw std::runtime_error("micro warm-up left queued tasks");

        double sequential_ms=0.0, fork_ms=0.0;
        if(sample%2u==0u) {
            sequential_ms=timed_sample(sequential,reference);
            fork_ms=timed_sample(fork_join,reference);
        } else {
            fork_ms=timed_sample(fork_join,reference);
            sequential_ms=timed_sample(sequential,reference);
        }
        if(scheduler.queue_size()!=0u)
            throw std::runtime_error("micro sample left queued tasks");
        sequential_samples.push_back(sequential_ms);
        fork_samples.push_back(fork_ms);
        paired_deltas.push_back((fork_ms-sequential_ms)*1.0e6/
                                static_cast<double>(options.tasks));
    }
    return {statistics(sequential_samples),statistics(fork_samples),
            median(std::move(paired_deltas))};
}


enum class BreakdownStage : unsigned {
    Sequential, Future, Ticket, Handle, Group
};

constexpr unsigned breakdown_stage_count=5u;

// These paths intentionally overlap: per-stage deltas are not additive.
// Run only with a zero-worker pool, so a local reference never escapes scope.
std::uint64_t breakdown_run(ThreadPool& scheduler,unsigned tasks,
                            unsigned work,BreakdownStage stage) {
    if(stage==BreakdownStage::Sequential)
        return micro_reference(tasks,work);
    if(stage==BreakdownStage::Group)
        return micro_run(scheduler,tasks,work);
    std::uint64_t total=0;
    for(unsigned i=0u;i<tasks;++i) {
        const auto seed=static_cast<std::uint64_t>(i);
        const auto left_work=[i,work] {
            return work_kernel(left_tag+static_cast<std::uint64_t>(i),work);
        };
        if(stage==BreakdownStage::Ticket) {
            std::uint64_t left=0u;
            auto ticket=scheduler.enqueue_ticket([&left,left_work] {
                left=left_work();
            });
            const auto right=work_kernel(right_tag+seed,work);
            if(!scheduler.try_run_task(ticket))
                throw std::runtime_error("zero-worker ticket was not pending");
            total+=combine(left,right);
        } else {
            auto task=std::make_shared<std::packaged_task<std::uint64_t()>>(left_work);
            auto future=task->get_future().share();
            if(stage==BreakdownStage::Future) {
                (*task)();
                const auto right=work_kernel(right_tag+seed,work);
                total+=combine(future.get(),right);
            } else {
                auto ticket=scheduler.enqueue_ticket([task] { (*task)(); });
                TaskHandle<ThreadPool,std::uint64_t> handle(
                    scheduler,std::move(future),std::move(ticket));
                const auto right=work_kernel(right_tag+seed,work);
                total+=combine(handle.get(),right);
            }
        }
    }
    return total;
}

struct BreakdownComparison {
    std::array<Statistics,breakdown_stage_count> stats;
    std::array<double,breakdown_stage_count> delta_ns_per_task;
};

BreakdownComparison compare_breakdown(Options const& options) {
    const auto reference=micro_reference(options.tasks,options.work);
    std::array<std::vector<double>,breakdown_stage_count> measurements;
    std::array<std::vector<double>,breakdown_stage_count> deltas;
    for(unsigned stage=0u;stage<breakdown_stage_count;++stage) {
        measurements[stage].reserve(options.samples);
        deltas[stage].reserve(options.samples);
    }
    for(unsigned sample=0u;sample<options.samples;++sample) {
        ThreadPool scheduler(0);
        std::array<double,breakdown_stage_count> elapsed{};
        for(unsigned stage=0u;stage<breakdown_stage_count;++stage) {
            const auto selected=static_cast<BreakdownStage>(stage);
            if(breakdown_run(scheduler,options.tasks,options.work,selected)!=reference)
                throw std::runtime_error("breakdown warm-up checksum mismatch");
            if(scheduler.queue_size()!=0u)
                throw std::runtime_error("breakdown warm-up left queued tasks");
        }
        // Rotate the measurement order to limit fixed ordering bias.
        for(unsigned offset=0u;offset<breakdown_stage_count;++offset) {
            const unsigned stage=(offset+sample)%breakdown_stage_count;
            auto run=[&] {
                return breakdown_run(scheduler,options.tasks,options.work,
                                     static_cast<BreakdownStage>(stage));
            };
            elapsed[stage]=timed_sample(run,reference);
            if(scheduler.queue_size()!=0u)
                throw std::runtime_error("breakdown sample left queued tasks");
        }
        for(unsigned stage=0u;stage<breakdown_stage_count;++stage) {
            measurements[stage].push_back(elapsed[stage]);
            deltas[stage].push_back(
                (elapsed[stage]-elapsed[0])*1.0e6/
                static_cast<double>(options.tasks));
        }
    }
    BreakdownComparison result{};
    for(unsigned stage=0u;stage<breakdown_stage_count;++stage) {
        result.stats[stage]=statistics(measurements[stage]);
        result.delta_ns_per_task[stage]=median(std::move(deltas[stage]));
    }
    return result;
}

void print_statistics(std::string const& label,Statistics const& values,
                      std::string const& unit) {
    LOGGING_PRINTLN(std::left<<std::setw(19)<<label<<std::right
        <<std::fixed<<std::setprecision(3)
        <<std::setw(13)<<values.median_ms
        <<std::setw(13)<<values.minimum_ms
        <<std::setw(13)<<values.maximum_ms
        <<std::setw(13)<<values.mad_ms<<"  "<<unit)
}

void report_breakdown(Options const& options) {
    const auto values=compare_breakdown(options);
    constexpr std::array<char const*,breakdown_stage_count> labels={
        "Sequential","Future only","Ticket only","Handle, no group","TaskGroup"
    };
    LOGGING_PRINTLN(" ")
    LOGGING_PRINTLN("Workload=breakdown | workers=0 | work="<<options.work
        <<" | iterations="<<options.tasks<<" | samples="<<options.samples)
    LOGGING_PRINTLN(std::left<<std::setw(19)<<"Execution"
        <<std::right<<std::setw(13)<<"Median"
        <<std::setw(13)<<"Min"
        <<std::setw(13)<<"Max"
        <<std::setw(13)<<"MAD"<<"  Unit")
    LOGGING_PRINTLN(std::string(81u,'-'))
    for(unsigned stage=0u;stage<breakdown_stage_count;++stage) {
        print_statistics(labels[stage],values.stats[stage],"ms");
        LOGGING_PRINTLN("  paired delta vs sequential: "
            <<std::fixed<<std::setprecision(1)
            <<values.delta_ns_per_task[stage]<<" ns/iteration")
    }
    LOGGING_PRINTLN("Stages overlap; differences are NOT additive component costs.")
}


// Paired A/B experiment within the same current TaskHandle implementation.
// "Callback" reproduces the legacy acquisition *representation* (a
// type-erased lambda), not the complete historical binary or TaskGroup.
std::uint64_t handle_variant_run(ThreadPool& scheduler,unsigned tasks,
                                 unsigned work,bool direct_ticket) {
    std::uint64_t total=0u;
    for(unsigned i=0u;i<tasks;++i) {
        const auto left_work=[i,work] {
            return work_kernel(left_tag+static_cast<std::uint64_t>(i),work);
        };
        auto task=std::make_shared<std::packaged_task<std::uint64_t()>>(left_work);
        auto future=task->get_future().share();
        auto ticket=scheduler.enqueue_ticket([task] { (*task)(); });
        // Both paths make an owning joiner and compute the right branch
        // locally; there is one outstanding fork per iteration.
        auto consume=[&](TaskHandle<ThreadPool,std::uint64_t> handle) {
            std::function<void()> joiner=[handle] {
                static_cast<void>(handle.get());
            };
            const auto right=work_kernel(right_tag+
                static_cast<std::uint64_t>(i),work);
            total+=combine(handle.get(),right);
            joiner();
        };
        if(direct_ticket) {
            consume(TaskHandle<ThreadPool,std::uint64_t>(
                scheduler,std::move(future),std::move(ticket)));
        } else {
            auto try_execute=[scheduler=&scheduler,ticket] {
                return scheduler->try_run_task(ticket);
            };
            consume(TaskHandle<ThreadPool,std::uint64_t>(
                scheduler,std::move(future),
                std::function<bool()>(try_execute)));
        }
    }
    return total;
}

struct HandleComparison {
    Statistics callback;
    Statistics direct;
    Statistics group;
    double paired_direct_minus_callback_ns;
    double paired_group_minus_direct_ns;
};

HandleComparison compare_handles(Options const& options,unsigned workers) {
    const auto reference=micro_reference(options.tasks,options.work);
    std::array<std::vector<double>,3u> elapsed;
    std::vector<double> direct_deltas,group_deltas;
    for(auto& samples:elapsed)samples.reserve(options.samples);
    direct_deltas.reserve(options.samples);
    group_deltas.reserve(options.samples);
    for(unsigned sample=0u;sample<options.samples;++sample) {
        ThreadPool scheduler(workers);
        auto run=[&](unsigned variant) -> std::uint64_t {
            if(variant==0u)
                return handle_variant_run(scheduler,options.tasks,options.work,false);
            if(variant==1u)
                return handle_variant_run(scheduler,options.tasks,options.work,true);
            return micro_run(scheduler,options.tasks,options.work);
        };
        for(unsigned variant=0u;variant<3u;++variant) {
            if(run(variant)!=reference)
                throw std::runtime_error("handle-compare warm-up checksum mismatch");
            if(scheduler.queue_size()!=0u)
                throw std::runtime_error("handle-compare warm-up left queued tasks");
        }
        std::array<double,3u> times{};
        // Rotate order to reduce systematic warming/order bias.
        for(unsigned offset=0u;offset<3u;++offset) {
            const unsigned variant=(sample+offset)%3u;
            times[variant]=timed_sample([&] { return run(variant); },reference);
            if(scheduler.queue_size()!=0u)
                throw std::runtime_error("handle-compare sample left queued tasks");
        }
        for(unsigned variant=0u;variant<3u;++variant)
            elapsed[variant].push_back(times[variant]);
        direct_deltas.push_back((times[1]-times[0])*1.0e6/
                               static_cast<double>(options.tasks));
        group_deltas.push_back((times[2]-times[1])*1.0e6/
                              static_cast<double>(options.tasks));
    }
    return {statistics(elapsed[0]),statistics(elapsed[1]),
            statistics(elapsed[2]),median(std::move(direct_deltas)),
            median(std::move(group_deltas))};
}

void report_handles(Options const& options,unsigned workers) {
    const auto values=compare_handles(options,workers);
    LOGGING_PRINTLN(" ")
    LOGGING_PRINTLN("Workload=handle-compare | workers="<<workers
        <<" | work="<<options.work<<" | forks="<<options.tasks
        <<" | samples="<<options.samples)
    LOGGING_PRINTLN(std::left<<std::setw(19)<<"Execution"
        <<std::right<<std::setw(13)<<"Median"
        <<std::setw(13)<<"Min"
        <<std::setw(13)<<"Max"
        <<std::setw(13)<<"MAD"<<"  Unit")
    LOGGING_PRINTLN(std::string(81u,'-'))
    print_statistics("Callback handle",values.callback,"ms");
    print_statistics("Direct ticket",values.direct,"ms");
    print_statistics("TaskGroup",values.group,"ms");
    LOGGING_PRINTLN("Paired delta (direct - callback): "
        <<std::fixed<<std::setprecision(1)
        <<values.paired_direct_minus_callback_ns<<" ns/fork")
    LOGGING_PRINTLN("Paired delta (TaskGroup - direct): "
        <<std::fixed<<std::setprecision(1)
        <<values.paired_group_minus_direct_ns<<" ns/fork")
    LOGGING_PRINTLN("Callback uses the current TaskHandle with its legacy callable constructor.")
}

void report(std::string const& mode,Options const& options,unsigned workers) {
    if(mode=="micro") {
        const auto times=compare_micro(options,workers);
        LOGGING_PRINTLN(" ")
        LOGGING_PRINTLN("Workload=micro | workers="<<workers
            <<" | work="<<options.work<<" | forks="<<options.tasks
            <<" | samples="<<options.samples)
        LOGGING_PRINTLN(std::left<<std::setw(19)<<"Execution"
            <<std::right<<std::setw(13)<<"Median"
            <<std::setw(13)<<"Min"
            <<std::setw(13)<<"Max"
            <<std::setw(13)<<"MAD"<<"  Unit")
        LOGGING_PRINTLN(std::string(81u,'-'))
        print_statistics("Sequential",times.sequential,"ms");
        print_statistics("TaskGroup",times.fork_join,"ms");
        LOGGING_PRINTLN("Median paired difference: "<<std::fixed
            <<std::setprecision(1)<<times.paired_delta_ns_per_fork
            <<" ns/fork (TaskGroup - sequential)")
        return;
    }
    auto times=compare(mode,options,workers);
    LOGGING_PRINTLN(" ")
    LOGGING_PRINTLN("Workload="<<mode<<" | workers="<<workers
        <<" | work="<<options.work<<" | samples="<<options.samples
        <<(mode=="recursive"
            ? " | cutoff=" + std::to_string(options.cutoff_depth)
                + " | forks=" + std::to_string(
                    (1u<<(options.depth-options.cutoff_depth))-1u)
            : std::string{}))
    LOGGING_PRINTLN(std::left<<std::setw(19)<<"Scheduler"
        <<std::right<<std::setw(13)<<"Median"
        <<std::setw(13)<<"Min"
        <<std::setw(13)<<"Max"
        <<std::setw(13)<<"MAD"<<"  Unit")
    LOGGING_PRINTLN(std::string(81u,'-'))
    print_statistics("ThreadPool",times,"ms");
}

bool has_concurrency_option(List<String> const& arguments) {
    return std::find(arguments.begin(),arguments.end(),String("--concurrency"))
               !=arguments.end()
        || std::find(arguments.begin(),arguments.end(),String("-c"))
               !=arguments.end();
}

bool has_work_option(List<String> const& arguments) {
    return std::find(arguments.begin(),arguments.end(),String("--work"))
               !=arguments.end()
        || std::find(arguments.begin(),arguments.end(),String("-w"))
               !=arguments.end();
}

bool has_help_option(List<String> const& arguments) {
    return std::find(arguments.begin(),arguments.end(),String("--help"))
               !=arguments.end()
        || std::find(arguments.begin(),arguments.end(),String("-h"))
               !=arguments.end();
}

} // namespace

int main(int argc,const char* argv[]) {
    // ThreadingCommandLineInterface attaches the Logger thread registry.
    auto& manager=ThreadManager::instance();
    Logger::instance().configuration().set_verbosity(1u);
    try {
        Options options;
        ThreadingCommandLineInterface command_line;
        command_line.add(CommandLineContribution(
            "thread-pool-benchmark",benchmark_parsers(options)));

        List<String> arguments;
        for(int i=0;i<argc;++i)arguments.append(String(argv[i]));
        const bool selected_concurrency=has_concurrency_option(arguments);
        if(!command_line.acquire(arguments))
            return has_help_option(arguments)?0:1;

        // Inherited -c/--concurrency selects the benchmark worker count.
        // Release any workers used to apply this option before measuring the
        // independent pools; otherwise idle ThreadManager workers distort it.
        const auto workers=selected_concurrency
            ?static_cast<unsigned>(manager.concurrency())
            :std::min(4u,std::max(1u,std::thread::hardware_concurrency()));
        manager.set_concurrency(0);

        if(options.sweep && (selected_concurrency || has_work_option(arguments)))
            throw std::invalid_argument(
                "--sweep cannot be combined with --concurrency or --work");
        if(options.cutoff_depth>options.depth)
            throw std::invalid_argument("--cutoff-depth must not exceed --depth");
        if(options.cutoff_sweep && options.sweep)
            throw std::invalid_argument(
                "--cutoff-sweep cannot be combined with --sweep");
        if(options.cutoff_sweep && options.cutoff_depth!=0u)
            throw std::invalid_argument(
                "--cutoff-sweep cannot be combined with --cutoff-depth");
        if(options.cutoff_sweep && options.mode!="recursive" && options.mode!="both")
            throw std::invalid_argument(
                "--cutoff-sweep requires a recursive workload");

        if(options.mode=="breakdown" &&
           (workers!=0u || options.sweep || options.cutoff_sweep))
            throw std::invalid_argument(
                "breakdown requires --concurrency 0 and no sweep");

        LOGGING_PRINTLN("ThreadPool work-stealing benchmark (Release recommended)")
        LOGGING_PRINTLN("tasks="<<options.tasks<<"  depth="<<options.depth
            <<"  samples="<<options.samples<<"  mode="
            <<(options.cutoff_sweep?"recursive (cutoff sweep)":options.mode))
        LOGGING_PRINTLN("A fresh pool is warmed per sample.")
        LOGGING_PRINTLN("Median / Min / Max / MAD are per-sample statistics.")
        LOGGING_PRINTLN("One scheduling policy: worker-local LIFO, FIFO stealing and global FIFO.")
        LOGGING_PRINTLN("Micro mode: one fork and one local branch per iteration.")

        auto measure_configuration=[&](unsigned nworkers,unsigned rounds) {
            Options selected=options;
            selected.work=rounds;
            if(selected.mode=="flat" || selected.mode=="both")
                report("flat",selected,nworkers);
            if(selected.mode=="recursive" || selected.mode=="both")
                report("recursive",selected,nworkers);
            if(selected.mode=="micro")
                report("micro",selected,nworkers);
            if(selected.mode=="handle-compare")
                report_handles(selected,nworkers);
        };

        if(options.mode=="breakdown") {
            report_breakdown(options);
        } else if(options.cutoff_sweep) {
            LOGGING_PRINTLN("Cutoff sweep: recursive only, workers="<<workers
                <<", work="<<options.work)
            // Evaluate even cutoffs, plus the full sequential baseline
            // when depth is odd. The checksum remains identical throughout.
            for(unsigned cutoff=0u;cutoff<=options.depth;cutoff+=2u) {
                Options selected=options;
                selected.cutoff_depth=cutoff;
                report("recursive",selected,workers);
            }
            if(options.depth%2u!=0u) {
                Options selected=options;
                selected.cutoff_depth=options.depth;
                report("recursive",selected,workers);
            }
        } else if(options.sweep) {
            LOGGING_PRINTLN("Sweep workers={1,2,4,8} x work={16,128,1024}")
            for(unsigned nworkers : {1u,2u,4u,8u})
                for(unsigned rounds : {16u,128u,1024u})
                    measure_configuration(nworkers,rounds);
        } else {
            measure_configuration(workers,options.work);
        }
        LOGGING_PRINTLN(" ")
        LOGGING_PRINTLN("Elapsed time includes task submission, execution and join,")
        LOGGING_PRINTLN("but excludes pool construction, warm-up and destruction.")
        return 0;
    } catch(std::exception const& error) {
        LOGGING_PRINTLN("benchmark_thread_pool: "<<error.what())
        return 1;
    }
}
