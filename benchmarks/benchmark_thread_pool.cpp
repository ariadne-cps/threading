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
#include "threading/work_stealing_pool.hpp"
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
    unsigned samples = 9u;
    std::string mode = "both";
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
                            std::uint64_t seed,unsigned work) {
    if(depth==0u)return work_kernel(seed,work);
    TaskGroup<Scheduler> group(scheduler);
    auto left=group.fork([&scheduler,depth,seed,work] {
        return recursive_run(scheduler,depth-1u,seed^left_tag,work);
    });
    const auto right=recursive_run(scheduler,depth-1u,seed+right_tag,work);
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

double median(std::vector<double> times) {
    std::sort(times.begin(),times.end());
    return times[times.size()/2u];
}

struct Comparison { double fifo_ms; double steal_ms; };

Comparison compare(std::string const& mode,Options const& options,unsigned workers) {
    // The same number of workers, tasks and computations for each scheduler.
    // Construct both pools outside all timed samples.
    ThreadPool fifo(workers);
    WorkStealingPool stealing(workers);
    const std::uint64_t reference=(mode=="flat")
        ?flat_reference(options.tasks,options.work)
        :recursive_reference(options.depth,left_tag,options.work);

    auto run=[&](auto& scheduler)->std::uint64_t {
        if(mode=="flat")return flat_run(scheduler,options.tasks,options.work);
        return recursive_run(scheduler,options.depth,left_tag,options.work);
    };

    std::vector<double> fifo_samples,stealing_samples;
    fifo_samples.reserve(options.samples);
    stealing_samples.reserve(options.samples);
    // Sample zero warms both pools; alternate order to reduce fixed bias.
    for(unsigned sample=0u;sample<=options.samples;++sample) {
        double a=0.0,b=0.0;
        if(sample%2u==0u) {
            a=timed_sample([&] { return run(fifo); },reference);
            b=timed_sample([&] { return run(stealing); },reference);
        } else {
            b=timed_sample([&] { return run(stealing); },reference);
            a=timed_sample([&] { return run(fifo); },reference);
        }
        if(sample!=0u) {
            fifo_samples.push_back(a);
            stealing_samples.push_back(b);
        }
    }
    return {median(std::move(fifo_samples)),median(std::move(stealing_samples))};
}

void report(std::string const& workload,Comparison result) {
    if(result.fifo_ms<=0.0 || result.steal_ms<=0.0 ||
       !std::isfinite(result.fifo_ms) || !std::isfinite(result.steal_ms))
        throw std::runtime_error("invalid timing measurement");
    LOGGING_PRINTLN(std::left<<std::setw(14)<<workload
        <<std::right<<std::setw(18)<<std::fixed<<std::setprecision(3)<<result.fifo_ms
        <<std::setw(19)<<result.steal_ms
        <<std::setw(18)<<result.fifo_ms/result.steal_ms<<"x")
}

bool has_concurrency_option(List<String> const& arguments) {
    return std::find(arguments.begin(),arguments.end(),String("--concurrency"))
               !=arguments.end()
        || std::find(arguments.begin(),arguments.end(),String("-c"))
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

        LOGGING_PRINTLN("ThreadPool / WorkStealingPool benchmark")
        LOGGING_PRINTLN("workers="<<workers<<"  tasks="<<options.tasks
            <<"  work="<<options.work<<"  depth="<<options.depth
            <<"  samples="<<options.samples)
        LOGGING_PRINTLN(std::string(73u,'='))
        LOGGING_PRINTLN(std::left<<std::setw(14)<<"Workload"
            <<std::right<<std::setw(18)<<"ThreadPool (ms)"
            <<std::setw(19)<<"Stealing (ms)"
            <<std::setw(19)<<"FIFO/stealing")
        LOGGING_PRINTLN(std::string(73u,'-'))
        if(options.mode=="flat" || options.mode=="both")
            report("flat",compare("flat",options,workers));
        if(options.mode=="recursive" || options.mode=="both")
            report("recursive",compare("recursive",options,workers));
        LOGGING_PRINTLN(std::string(73u,'='))
        LOGGING_PRINTLN("Ratio > 1 means the work-stealing pool was faster.")
        LOGGING_PRINTLN("Median elapsed milliseconds after one warm-up; "
                        "worker startup excluded; all checksums verified.")
        return 0;
    } catch(std::exception const& error) {
        LOGGING_PRINTLN("benchmark_thread_pool: "<<error.what())
        return 1;
    }
}
