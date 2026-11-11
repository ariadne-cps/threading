/***************************************************************************
 *            benchmark_use_cases.cpp
 *
 *  Copyright 2026 Luca Geretti
 *
 ****************************************************************************/
/*
 *  This file is part of Ariadne Threading.
 *
 *  Ariadne Threading is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Ariadne Threading is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Ariadne Threading. If not, see <https://www.gnu.org/licenses/>.
 */

#include "threading/parallel.hpp"
#include "threading/parallel_first.hpp"
#include "threading/task_graph.hpp"
#include "threading/thread_manager.hpp"
#include "ariadne-benchmarking.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Ariadne;
namespace Bench = Ariadne::Benchmarking;

namespace {

struct Options {
    std::string mode="all";
    unsigned workers=4u;
    unsigned cases=128u;
    unsigned work=2048u;
    unsigned batch_grain=16u;
    unsigned samples=7u;
    unsigned warmups=0u;
    Bench::SummaryFields summary_fields=Bench::default_summary_fields();
};

unsigned parse_positive(std::string const& value,unsigned maximum) {
    if(value.empty() || value.front()=='-')
        throw std::invalid_argument("expected nonnegative integer");
    std::size_t consumed=0u;
    auto parsed=std::stoull(value,&consumed);
    if(consumed!=value.size() || parsed>maximum)
        throw std::invalid_argument("argument out of range");
    return static_cast<unsigned>(parsed);
}

// Workloads retain their domain-specific options but reuse Benchmarking CLI.
std::optional<Options> parse_options(int argc,char** argv) {
    Options options;
    Bench::CommandLine cli;
    cli.add(CommandLineContribution("use-cases", {
        make_valued_argument_parser("", "mode",
            "Workload: cdcl, dnn-forward, dnn-gradient, all (default all)",
            [&options](String const& value) -> VoidFunction {
                const std::string result(value);
                if(result!="cdcl" && result!="dnn-forward" && result!="dnn-gradient" && result!="all")
                    throw std::invalid_argument("invalid benchmark mode");
                return [&options,result] { options.mode=result; };
            }),
        make_valued_argument_parser("", "workers",
            "Dedicated pool workers, 0..64 (default 4)",
            [&options](String const& value) -> VoidFunction {
                const unsigned result=parse_positive(std::string(value),64u);
                
                return [&options,result] { options.workers=result; };
            }),
        make_valued_argument_parser("", "cases",
            "Independent candidates / batch items, 1..4096 (default 128)",
            [&options](String const& value) -> VoidFunction {
                const unsigned result=parse_positive(std::string(value),4096u);
                if(result==0u)throw std::invalid_argument("cases must be positive");
                return [&options,result] { options.cases=result; };
            }),
        make_valued_argument_parser("", "work",
            "CDCL integer mixing rounds, 1..100000 (default 2048)",
            [&options](String const& value) -> VoidFunction {
                const unsigned result=parse_positive(std::string(value),100000u);
                if(result==0u)throw std::invalid_argument("work must be positive");
                return [&options,result] { options.work=result; };
            }),
        make_valued_argument_parser("", "batch-grain",
            "DNN batch items per graph group, 1..4096 (default 16)",
            [&options](String const& value) -> VoidFunction {
                const unsigned result=parse_positive(std::string(value),4096u);
                if(result==0u)throw std::invalid_argument("batch-grain must be positive");
                return [&options,result] { options.batch_grain=result; };
            }),
        make_valued_argument_parser("", "samples",
            "Odd sample count, 3..101 (default 7)",
            [&options](String const& value) -> VoidFunction {
                const unsigned result=parse_positive(std::string(value),101u);
                if(result<3u || result%2u==0u)throw std::invalid_argument("samples must be odd, 3..101");
                return [&options,result] { options.samples=result; };
            })
    }));
    List<String> args;
    for(int i=0;i<argc;++i)args.append(String(argv[i]));
    const bool help=std::find(args.begin(),args.end(),String("--help"))!=args.end()
        || std::find(args.begin(),args.end(),String("-h"))!=args.end();
    if(not cli.acquire(args)) {
        if(help) {
            std::cout << "CDCL is a portfolio analogue, not a SAT solver.\n"
                << "DNN kernels compare TaskGraph and parallel_for with identical math.\n"
                << "They are CPU-dense kernels, not full neural network training.\n";
            return std::nullopt;
        }
        throw std::invalid_argument("invalid benchmark command line");
    }
    options.warmups=cli.warmups();
    options.summary_fields=cli.summary_fields();
    return options;
}

std::uint64_t mix(std::uint64_t x,unsigned rounds) {
    for(unsigned i=0u;i<rounds;++i) {
        x^=x>>30;
        x*=UINT64_C(0xbf58476d1ce4e5b9);
        x^=x>>27;
        x*=UINT64_C(0x94d049bb133111eb);
        x^=x>>31;
    }
    return x;
}

constexpr std::uint64_t seed=UINT64_C(0x9e3779b97f4a7c15);

using Statistics = Bench::Statistics;

Statistics stats(std::vector<double> const& times) {
    Bench::Samples samples;
    for (double value : times) samples.add(value);
    return Bench::summarize(samples);
}

template<class F>
double timed(F&& function) {
    return Bench::measure_ns(std::forward<F>(function));
}

// Report the same paired workloads and speedups with Benchmarking's table engine.
void print_pair(std::string const& mode,Options const& options,
                Statistics serial,Statistics parallel,
                std::size_t graph_nodes=0u) {
    const std::string config = "workers=" + std::to_string(options.workers)
        + " cases=" + std::to_string(options.cases)
        + " samples=" + std::to_string(options.samples)
        + (mode=="cdcl" ? " work=" + std::to_string(options.work)
                        : " graph_nodes=" + std::to_string(graph_nodes));
    Bench::Report report(mode + " benchmark (ns)", options.summary_fields);
    report.add(config, "sequential",serial);
    report.add(config, "parallel",parallel);
    std::cout << report.table();
    if(parallel.median>0.0)
        std::cout << "  median ratio sequential/parallel: "
                  << serial.median/parallel.median << "x\n";
}

void print_dnn_comparison(std::string const& mode,Options const& options,
                          Statistics serial,Statistics graph,Statistics fused,
                          std::size_t graph_nodes,std::size_t groups) {
    const std::string config = "workers=" + std::to_string(options.workers)
        + " cases=" + std::to_string(options.cases)
        + " samples=" + std::to_string(options.samples)
        + " batch_grain=" + std::to_string(options.batch_grain)
        + " graph_nodes=" + std::to_string(graph_nodes)
        + " groups=" + std::to_string(groups);
    Bench::Report report(mode + " benchmark (ns)", options.summary_fields);
    report.add(config, "sequential",serial);
    report.add(config, "task_graph",graph);
    report.add(config, "parallel_for",fused);
    std::cout << report.table();
    if(graph.median>0.0)
        std::cout << "  median ratio sequential/task_graph: "
                  << serial.median/graph.median << "x\n";
    if(fused.median>0.0) {
        std::cout << "  median ratio sequential/parallel_for: "
                  << serial.median/fused.median << "x\n";
        std::cout << "  median ratio task_graph/parallel_for: "
                  << graph.median/fused.median << "x\n";
    }
}

// CDCL analogue: independent search strategies, exactly one conclusive
// candidate at the final index. No clauses, propagation, conflicts or
// theories are modelled. Checking the digest on every candidate forces
// the synthetic work to be performed before rejecting a candidate.
void benchmark_cdcl(Options const& options) {
    ThreadPool pool(options.workers);
    const std::size_t candidates=options.cases;
    const auto target=mix(seed+candidates-1u,options.work);
    auto search=[&](std::size_t index,std::stop_token stop)
            -> std::optional<std::uint64_t> {
        if(stop.stop_requested())return std::nullopt;
        const auto digest=mix(seed+index,options.work);
        if(digest==target && index+1u==candidates)
            return digest;
        return std::nullopt;
    };
    auto sequential=[&] {
        for(std::size_t index=0u;index<candidates;++index) {
            auto result=search(index,{});
            if(result)return result;
        }
        return std::optional<std::uint64_t>{};
    };
    auto concurrent=[&] {
        return parallel_first_result(pool,candidates,search);
    };
    const auto reference=sequential();
    if(!reference || *reference!=target || concurrent()!=reference)
        throw std::runtime_error("CDCL analogue warm-up result mismatch");
    for (std::uint64_t i=0u; i<options.warmups; ++i)
        if(sequential()!=reference || concurrent()!=reference || pool.queue_size()!=0u)
            throw std::runtime_error("CDCL additional warm-up mismatch");

    std::vector<double> seq_times,par_times;
    for(unsigned sample=0u;sample<options.samples;++sample) {
        std::optional<std::uint64_t> seq,par;
        if(sample%2u==0u) {
            seq_times.push_back(timed([&] { seq=sequential(); }));
            par_times.push_back(timed([&] { par=concurrent(); }));
        } else {
            par_times.push_back(timed([&] { par=concurrent(); }));
            seq_times.push_back(timed([&] { seq=sequential(); }));
        }
        if(seq!=reference || par!=reference || pool.queue_size()!=0u)
            throw std::runtime_error("CDCL analogue checksum or queue mismatch");
    }
    print_pair("cdcl",options,stats(seq_times),stats(par_times));
}

constexpr std::size_t input_width=32u;
constexpr std::size_t hidden_width=32u;
constexpr std::size_t output_width=8u;

// A small two-dense-layer CPU neural network, with ReLU hidden units.
// Gradient mode computes d(loss)/dW1 and d(loss)/dW2 for squared-output
// loss, but deliberately does NOT update weights or run an optimizer.
struct Network {
    std::array<double,hidden_width*input_width> w1{};
    std::array<double,output_width*hidden_width> w2{};
    std::vector<double> input;

    explicit Network(std::size_t batch) : input(batch*input_width) {
        for(std::size_t i=0u;i<w1.size();++i)
            w1[i]=static_cast<double>(static_cast<int>((i*17u+3u)%89u)-44)/1024.0;
        for(std::size_t i=0u;i<w2.size();++i)
            w2[i]=static_cast<double>(static_cast<int>((i*29u+7u)%83u)-41)/1024.0;
        for(std::size_t i=0u;i<input.size();++i)
            input[i]=static_cast<double>(static_cast<int>((i*31u+11u)%101u)-50)/256.0;
    }
};

struct WorkState {
    std::vector<double> hidden;
    std::vector<double> output;
    std::vector<double> grad1;
    std::vector<double> grad2;

    explicit WorkState(std::size_t batch,bool gradient)
        : hidden(batch*hidden_width),output(batch*output_width),
          grad1(gradient?batch*hidden_width*input_width:0u),
          grad2(gradient?batch*output_width*hidden_width:0u) { }
};

void dense_hidden(Network const& net,WorkState& state,std::size_t batch) {
    for(std::size_t h=0u;h<hidden_width;++h) {
        double value=0.0;
        for(std::size_t i=0u;i<input_width;++i)
            value+=net.w1[h*input_width+i]*net.input[batch*input_width+i];
        state.hidden[batch*hidden_width+h]=std::max(value,0.0);
    }
}

void dense_output(Network const& net,WorkState& state,std::size_t batch) {
    for(std::size_t o=0u;o<output_width;++o) {
        double value=0.0;
        for(std::size_t h=0u;h<hidden_width;++h)
            value+=net.w2[o*hidden_width+h]*state.hidden[batch*hidden_width+h];
        state.output[batch*output_width+o]=value;
    }
}

void dense_gradient(Network const& net,WorkState& state,std::size_t batch) {
    std::array<double,hidden_width> delta{};
    for(std::size_t h=0u;h<hidden_width;++h) {
        double total=0.0;
        for(std::size_t o=0u;o<output_width;++o) {
            const auto output=state.output[batch*output_width+o];
            state.grad2[batch*(output_width*hidden_width)+o*hidden_width+h]
                =2.0*output*state.hidden[batch*hidden_width+h];
            total+=2.0*output*net.w2[o*hidden_width+h];
        }
        delta[h]=state.hidden[batch*hidden_width+h]>0.0?total:0.0;
    }
    for(std::size_t h=0u;h<hidden_width;++h)
        for(std::size_t i=0u;i<input_width;++i)
            state.grad1[batch*(hidden_width*input_width)+h*input_width+i]
                =delta[h]*net.input[batch*input_width+i];
}

// Mix bit patterns in a fixed order, including per-sample gradients when
// requested. Fingerprints are checked outside measured sections.
std::uint64_t fingerprint(WorkState const& state) {
    std::uint64_t checksum=seed;
    auto consume=[&](std::vector<double> const& values) {
        for(double value:values)
            checksum=std::rotl(checksum,11)^std::bit_cast<std::uint64_t>(value);
    };
    consume(state.hidden);
    consume(state.output);
    consume(state.grad1);
    consume(state.grad2);
    return checksum;
}

void benchmark_dnn(Options const& options,bool gradient) {
    const std::size_t batch=options.cases;
    Network network(batch);
    WorkState reference_state(batch,gradient);
    WorkState graph_state(batch,gradient);
    WorkState for_state(batch,gradient);
    ThreadPool pool(options.workers);

    auto sequential=[&] {
        for(std::size_t b=0u;b<batch;++b) {
            dense_hidden(network,reference_state,b);
            dense_output(network,reference_state,b);
            if(gradient)dense_gradient(network,reference_state,b);
        }
    };


    // The graph retains separate hidden -> output -> gradient nodes per
    // group, while parallel_for runs all three stages per sample inside
    // one callback per group. The numerical work is otherwise identical.
    const std::size_t grain=options.batch_grain;
    const std::size_t groups=1u+(batch-1u)/grain;
    TaskGraph graph;
    for(std::size_t group=0u;group<groups;++group) {
        const std::size_t begin=group*grain;
        const std::size_t end=begin+std::min(grain,batch-begin);
        auto hidden=graph.add_task([&,begin,end] {
            for(std::size_t b=begin;b<end;++b)
                dense_hidden(network,graph_state,b);
        });
        auto output=graph.add_task([&,begin,end] {
            for(std::size_t b=begin;b<end;++b)
                dense_output(network,graph_state,b);
        });
        graph.add_dependency(hidden,output);
        if(gradient) {
            auto backward=graph.add_task([&,begin,end] {
                for(std::size_t b=begin;b<end;++b)
                    dense_gradient(network,graph_state,b);
            });
            graph.add_dependency(output,backward);
        }
    }
    auto concurrent_graph=[&] { graph.run(pool,ParallelOptions{1u,0u}); };
    auto concurrent_for=[&] {
        parallel_for(pool,0u,groups,[&](std::size_t group) {
            const std::size_t begin=group*grain;
            const std::size_t end=begin+std::min(grain,batch-begin);
            for(std::size_t b=begin;b<end;++b) {
                dense_hidden(network,for_state,b);
                dense_output(network,for_state,b);
                if(gradient)dense_gradient(network,for_state,b);
            }
        },ParallelOptions{1u,0u});
    };

    // Construct the graph and allocate all output buffers before timing.
    sequential();
    concurrent_graph();
    concurrent_for();
    const auto checksum=fingerprint(reference_state);
    if(fingerprint(graph_state)!=checksum ||
       fingerprint(for_state)!=checksum || pool.queue_size()!=0u)
        throw std::runtime_error("DNN warm-up fingerprint or queue mismatch");
    for (std::uint64_t i=0u; i<options.warmups; ++i) {
        sequential();
        concurrent_graph();
        concurrent_for();
        if(fingerprint(reference_state)!=checksum ||
           fingerprint(graph_state)!=checksum ||
           fingerprint(for_state)!=checksum || pool.queue_size()!=0u)
            throw std::runtime_error("DNN additional warm-up mismatch");
    }

    std::vector<double> seq_times,graph_times,for_times;
    seq_times.reserve(options.samples);
    graph_times.reserve(options.samples);
    for_times.reserve(options.samples);
    for(unsigned sample=0u;sample<options.samples;++sample) {
        // Rotate order to reduce systematic warm-cache and frequency bias.
        // Each timing includes execution/dispatch, but not fingerprinting.
        auto time_serial=[&] { seq_times.push_back(timed(sequential)); };
        auto time_graph=[&] { graph_times.push_back(timed(concurrent_graph)); };
        auto time_for=[&] { for_times.push_back(timed(concurrent_for)); };
        switch(sample%3u) {
          case 0u:
            time_serial(); time_graph(); time_for(); break;
          case 1u:
            time_graph(); time_for(); time_serial(); break;
          default:
            time_for(); time_serial(); time_graph(); break;
        }
        if(fingerprint(reference_state)!=checksum ||
           fingerprint(graph_state)!=checksum ||
           fingerprint(for_state)!=checksum || pool.queue_size()!=0u)
            throw std::runtime_error("DNN sample fingerprint or queue mismatch");
    }
    print_dnn_comparison(gradient?"dnn-gradient":"dnn-forward",
                         options,stats(seq_times),stats(graph_times),
                         stats(for_times),graph.size(),groups);
}

} // namespace

int main(int argc,char** argv) {
    try {
        // Avoid background workers from the process-wide manager while
        // independently measuring the selected ThreadPool worker count.
        ThreadManager::instance().set_concurrency(0u);
        const auto parsed=parse_options(argc,argv);
        if(not parsed) return 0;
        const auto& options=*parsed;
        if(options.mode=="cdcl" || options.mode=="all")
            benchmark_cdcl(options);
        if(options.mode=="dnn-forward" || options.mode=="all")
            benchmark_dnn(options,false);
        if(options.mode=="dnn-gradient" || options.mode=="all")
            benchmark_dnn(options,true);
        return 0;
    } catch(std::exception const& error) {
        std::cerr << "Benchmark failure: " << error.what() << '\n';
        return 1;
    }
}
