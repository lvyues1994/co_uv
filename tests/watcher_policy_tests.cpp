#include <limits>
#include <stdexcept>
#include "test.hpp"
#include "uvco/detail/watcher/delivery.hpp"
using namespace uvco;
namespace wd = uvco::detail::watcher;
namespace {
struct Merge { bool operator()(int& a, int const& b) const noexcept { if (a/10 != b/10) return false; a |= b; return true; } };
}
TEST_CASE("bounded FIFO merges adjacent values and errors but never exceptions") {
    wd::BoundedFifo<int, 2, Merge> queue;
    queue.push({11,{}}); queue.push({12,{}});
    CHECK(queue.size()==1); CHECK(queue.coalescedCount()==1);
    CHECK(*queue.pop().result==15);
    queue.push({Errc::invalid_argument,{}}); queue.push({Errc::invalid_argument,{}});
    CHECK(queue.size()==1); CHECK(queue.coalescedCount()==2);
    queue.push({Errc::invalid_argument,std::make_exception_ptr(std::runtime_error("projection"))});
    CHECK(queue.size()==2); CHECK(queue.coalescedCount()==2);
    queue.push({42,{}}); CHECK(queue.overflowCount()==1);
    CHECK(queue.pop().exception); CHECK(*queue.pop().result==42);
    queue.clear(); CHECK(queue.size()==0); CHECK(queue.overflowCount()==1);
}
TEST_CASE("latest only overwrites and legacy counter wraps to empty") {
    wd::LatestOnly<int> latest;
    latest.push({1,{}}); latest.push({2,{}}); CHECK(latest.size()==1);
    CHECK(*latest.pop().result==2); CHECK(latest.overflowCount()==1);
    wd::LegacyTimerCount count;
    count.push({(std::numeric_limits<std::uint64_t>::max)(),{}});
    count.push({1,{}}); CHECK(count.size()==0);
    count.push({2,{}}); CHECK(count.size()==1); CHECK(*count.pop().result==2);
    CHECK(count.size()==0);
}
UVCO_TEST_MAIN
