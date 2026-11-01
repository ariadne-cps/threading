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
        "m","mode","Workload: flat, recursive, both (default both)",
        [&options](String const& value) -> VoidFunction {
            const std::string mode(value);
            if(mode!="flat" && mode!="recursive" && mode!="both")
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

struct Comparison {
    Statistics fifo;
    Statistics stealing;
    Statistics paired_ratio;
};

// Construct exactly one pool at a time, keeping all other pool workers
// absent. Worker startup, a complete warm-up and destruction are excluded
// from the clock. Every measured sample uses a freshly warmed pool.
double measure_pool(std::string const& mode, Options const& options,
                    unsigned workers, std::uint64_t reference,
                    SchedulingPolicy policy) {
    ThreadPool scheduler(workers,policy);
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

Comparison compare(std::string const& mode, Options const& options,
                   unsigned workers) {
    const auto reference=mode=="flat"
        ? flat_reference(options.tasks,options.work)
        : recursive_reference(options.depth,left_tag,options.work);
    std::vector<double> fifo_samples,stealing_samples,paired;
    fifo_samples.reserve(options.samples);
    stealing_samples.reserve(options.samples);
    paired.reserve(options.samples);

    for(unsigned sample=0u;sample<options.samples;++sample) {
        double fifo_ms=0.0,stealing_ms=0.0;
        // Alternate the order of separately constructed, warmed pools.
        if(sample%2u==0u) {
            fifo_ms=measure_pool(mode,options,workers,reference,SchedulingPolicy::FIFO);
            stealing_ms=measure_pool(mode,options,workers,reference,SchedulingPolicy::WorkStealing);
        } else {
            stealing_ms=measure_pool(mode,options,workers,reference,SchedulingPolicy::WorkStealing);
            fifo_ms=measure_pool(mode,options,workers,reference,SchedulingPolicy::FIFO);
        }
        if(!(std::isfinite(fifo_ms) && std::isfinite(stealing_ms)
             && fifo_ms>0.0 && stealing_ms>0.0))
            throw std::runtime_error("invalid paired measurement");
        fifo_samples.push_back(fifo_ms);
        stealing_samples.push_back(stealing_ms);
        paired.push_back(fifo_ms/stealing_ms);
    }
    return {statistics(fifo_samples),statistics(stealing_samples),
            statistics(paired)};
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

void report(std::string const& mode,Options const& options,unsigned workers) {
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
    print_statistics("ThreadPool",times.fifo,"ms");
    print_statistics("ThreadPool[WorkStealing]",times.stealing,"ms");
    print_statistics("Paired FIFO/steal",times.paired_ratio,"x");
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
        if(options.cutoff_sweep && options.mode=="flat")
            throw std::invalid_argument(
                "--cutoff-sweep requires a recursive workload");

        LOGGING_PRINTLN("ThreadPool / ThreadPool[WorkStealing] benchmark (Release recommended)")
        LOGGING_PRINTLN("tasks="<<options.tasks<<"  depth="<<options.depth
            <<"  samples="<<options.samples<<"  mode="
            <<(options.cutoff_sweep?"recursive (cutoff sweep)":options.mode))
        LOGGING_PRINTLN("Each pool runs alone; a new pool is warmed per sample.")
        LOGGING_PRINTLN("Median / Min / Max / MAD are per-sample statistics.")
        LOGGING_PRINTLN("Paired FIFO/steal is the median of per-pair ratios; >1 favors stealing.")

        auto measure_configuration=[&](unsigned nworkers,unsigned rounds) {
            Options selected=options;
            selected.work=rounds;
            if(selected.mode=="flat" || selected.mode=="both")
                report("flat",selected,nworkers);
            if(selected.mode=="recursive" || selected.mode=="both")
                report("recursive",selected,nworkers);
        };

        if(options.cutoff_sweep) {
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
