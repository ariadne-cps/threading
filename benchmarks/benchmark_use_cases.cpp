/***************************************************************************
 *            benchmark_use_cases.cpp
 *
 *  Copyright 2026 Luca Geretti
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

#include "threading/parallel_first.hpp"
#include "threading/task_graph.hpp"
#include "threading/thread_manager.hpp"

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

namespace {

struct Options {
    std::string mode="all";
    unsigned workers=4u;
    unsigned cases=128u;
    unsigned work=2048u;
    unsigned batch_grain=16u;
    unsigned samples=7u;
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

Options parse_options(int argc,char** argv) {
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string option=argv[i];
        if(option=="--help") {
            std::cout << "Synthetic BDD/CDCL/DNN threading workload benchmark\n"
                << "  --mode cdcl|dnn-forward|dnn-gradient|all\n"
                << "  --workers N   dedicated pool workers, 0..64 (default 4)\n"
                << "  --cases N     independent candidates / batch items, 1..4096 (default 128)\n"
                << "  --work N      CDCL-only integer mixing rounds, 1..100000 (default 2048)\n"
                << "  --batch-grain N  DNN batch items per graph group, 1..4096 (default 16)\n"
                << "                   1 reproduces the original one-item-per-stage DAG\n"
                << "  --samples N   odd 3..101 (default 7)\n"
                << "The CDCL case is a portfolio analogue, NOT a satisfiability solver.\n"
                << "DNN workloads are CPU dense forward/gradient kernels, NOT training a model.\n";
            std::exit(0);
        }
        if(i+1>=argc)
            throw std::invalid_argument("missing value after "+option);
        const std::string value=argv[++i];
        if(option=="--mode")options.mode=value;
        else if(option=="--workers")options.workers=parse_positive(value,64u);
        else if(option=="--cases")options.cases=parse_positive(value,4096u);
        else if(option=="--work")options.work=parse_positive(value,100000u);
        else if(option=="--batch-grain")options.batch_grain=parse_positive(value,4096u);
        else if(option=="--samples")options.samples=parse_positive(value,101u);
        else throw std::invalid_argument("unknown option "+option);
    }
    if(options.mode!="cdcl" && options.mode!="dnn-forward" &&
       options.mode!="dnn-gradient" && options.mode!="all")
        throw std::invalid_argument("invalid benchmark mode");
    if(options.cases==0u || options.work==0u || options.batch_grain==0u ||
       options.samples<3u || options.samples%2u==0u)
        throw std::invalid_argument("invalid count, work or samples");
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

double median(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    return values[values.size()/2u];
}

struct Statistics {
    double median_ms;
    double min_ms;
    double max_ms;
    double mad_ms;
};

Statistics stats(std::vector<double> const& samples) {
    if(samples.empty())throw std::runtime_error("empty sample set");
    const auto middle=median(samples);
    std::vector<double> deviation;
    deviation.reserve(samples.size());
    for(double value:samples) {
        if(!std::isfinite(value) || value<0.0)
            throw std::runtime_error("invalid sample duration");
        deviation.push_back(std::abs(value-middle));
    }
    auto extremes=std::minmax_element(samples.begin(),samples.end());
    return {middle,*extremes.first,*extremes.second,median(deviation)};
}

template<class F>
double timed(F&& function) {
    const auto before=std::chrono::steady_clock::now();
    function();
    const auto after=std::chrono::steady_clock::now();
    return std::chrono::duration<double,std::milli>(after-before).count();
}

void print_pair(std::string const& mode,Options const& options,
                Statistics serial,Statistics parallel,
                std::size_t graph_nodes=0u) {
    std::cout << std::fixed << std::setprecision(5)
              << "mode=" << mode << " workers=" << options.workers
              << " cases=" << options.cases << " samples=" << options.samples;
    if(mode=="cdcl")
        std::cout << " work=" << options.work;
    else
        std::cout << " batch_grain=" << options.batch_grain
                  << " graph_nodes=" << graph_nodes;
    std::cout << '\n'
              << "  sequential median/min/max/MAD ms: "
              << serial.median_ms << '/' << serial.min_ms << '/'
              << serial.max_ms << '/' << serial.mad_ms << '\n'
              << "  parallel   median/min/max/MAD ms: "
              << parallel.median_ms << '/' << parallel.min_ms << '/'
              << parallel.max_ms << '/' << parallel.mad_ms << '\n';
    if(parallel.median_ms>0.0)
        std::cout << "  median ratio sequential/parallel: "
                  << serial.median_ms/parallel.median_ms << "x\n";
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
    WorkState parallel_state(batch,gradient);
    ThreadPool pool(options.workers);

    auto sequential=[&] {
        for(std::size_t b=0u;b<batch;++b) {
            dense_hidden(network,reference_state,b);
            dense_output(network,reference_state,b);
            if(gradient)dense_gradient(network,reference_state,b);
        }
    };

    // Each group has a dependency chain hidden -> output -> gradient.
    // Samples within a group are independent: grouping changes only task
    // granularity, not the arithmetic order within any individual sample.
    // In particular, --batch-grain 1 recreates the original task graph.
    const std::size_t grain=options.batch_grain;
    const std::size_t groups=1u+(batch-1u)/grain;
    TaskGraph graph;
    for(std::size_t group=0u;group<groups;++group) {
        const std::size_t begin=group*grain;
        const std::size_t end=begin+std::min(grain,batch-begin);
        auto hidden=graph.add_task([&,begin,end] {
            for(std::size_t b=begin;b<end;++b)
                dense_hidden(network,parallel_state,b);
        });
        auto output=graph.add_task([&,begin,end] {
            for(std::size_t b=begin;b<end;++b)
                dense_output(network,parallel_state,b);
        });
        graph.add_dependency(hidden,output);
        if(gradient) {
            auto backward=graph.add_task([&,begin,end] {
                for(std::size_t b=begin;b<end;++b)
                    dense_gradient(network,parallel_state,b);
            });
            graph.add_dependency(output,backward);
        }
    }
    auto concurrent=[&] { graph.run(pool,ParallelOptions{1u,0u}); };

    sequential();
    concurrent();
    const auto checksum=fingerprint(reference_state);
    if(fingerprint(parallel_state)!=checksum || pool.queue_size()!=0u)
        throw std::runtime_error("DNN warm-up checksum or queue mismatch");

    std::vector<double> seq_times,par_times;
    for(unsigned sample=0u;sample<options.samples;++sample) {
        if(sample%2u==0u) {
            seq_times.push_back(timed(sequential));
            par_times.push_back(timed(concurrent));
        } else {
            par_times.push_back(timed(concurrent));
            seq_times.push_back(timed(sequential));
        }
        if(fingerprint(reference_state)!=checksum ||
           fingerprint(parallel_state)!=checksum || pool.queue_size()!=0u)
            throw std::runtime_error("DNN sample checksum or queue mismatch");
    }
    print_pair(gradient?"dnn-gradient":"dnn-forward",
               options,stats(seq_times),stats(par_times),graph.size());
}

} // namespace

int main(int argc,char** argv) {
    try {
        // Avoid background workers from the process-wide manager while
        // independently measuring the selected ThreadPool worker count.
        ThreadManager::instance().set_concurrency(0u);
        const auto options=parse_options(argc,argv);
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
