#include "allocation_counter.hpp"
#include "test.hpp"
#include <uvco/uvco.hpp>
namespace {
struct Plain {
    int operator()() const {
        return 7;
    }
};
struct Cooperative {
    int operator()(co2::stop_token token) const {
        return token.stop_possible() && !token.stop_requested() ? 7 : 0;
    }
};
template <class Op>
auto invoke(Op &op, std::size_t expected)
    CO2_BEG(co2::Task<>,(op,expected),std::size_t i=0,count=0;uvco::Result<int> r;) {
    for (i = 0; i < 2; ++i) {
        allocation_test::begin();
        CO2_AWAIT_SET(r, op);
        count = allocation_test::end();
        CHECK(r);
        CHECK(*r == 7);
        CHECK(count == expected);
    }
}
CO2_END
}
TEST_CASE("Work without token has no cancel-state allocation at construction or start") {
    uvco::Loop loop;
    allocation_test::begin();
    auto op = uvco::queueWork(loop, Plain{});
    auto n = allocation_test::end();
    CHECK(n == 0);
    uvco::run(loop, invoke(op, 0));
}
TEST_CASE("Work cooperative state is lazy and fresh for each submission") {
    uvco::Loop loop;
    allocation_test::begin();
    auto op = uvco::queueWork(loop, Cooperative{});
    auto n = allocation_test::end();
    CHECK(n == 0);
    uvco::run(loop, invoke(op, 1));
}
namespace {
struct Both {
    int operator()() const {
        return 0;
    }
    int operator()(co2::stop_token token) const {
        return token.stop_possible() ? 7 : 0;
    }
};
}
TEST_CASE("Work overload resolution still prefers cooperative overload") {
    uvco::Loop loop;
    auto op = uvco::queueWork(loop, Both{});
    uvco::run(loop, invoke(op, 1));
}
UVCO_TEST_MAIN
