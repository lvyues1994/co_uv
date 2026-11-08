#pragma once
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace uvco_bench {
struct Summary { double p50,p99,max; };
inline Summary summarize(std::vector<double> values) {
    if(values.empty())throw std::invalid_argument("empty latency sample");
    std::sort(values.begin(),values.end());
    auto rank=[&](double p){return values[static_cast<std::size_t>(std::ceil(p*static_cast<double>(values.size())))-1];};
    return {rank(.50),rank(.99),values.back()};
}
}
