#include <stdexcept>
#include <string>
#include <vector>

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/core.hpp"

namespace {

auto answer(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop)) {
    CHECK(loop.isLoopThread());
    CO2_RETURN(42);
}
CO2_END

auto yields(uvco::Loop& loop, int count)
    CO2_BEG(co2::Task<int>, (loop, count), int i{};) {
    for (i = 0; i < count; ++i) CO2_AWAIT(co2::scheduleOn(loop));
    CO2_RETURN(i);
}
CO2_END

auto nothing(uvco::Loop& loop, bool& ran) CO2_BEG(co2::Task<>, (loop, ran)) {
    CO2_AWAIT(co2::scheduleOn(loop));
    ran = true;
}
CO2_END

struct Boom : std::runtime_error {
    Boom() : std::runtime_error{"boom"} {}
};

auto throws(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop)) {
    CO2_AWAIT(co2::scheduleOn(loop));
    throw Boom{};
}
CO2_END

auto interleave(uvco::Loop& loop, std::vector<std::string>& log, std::string name)
    CO2_BEG(co2::Task<>, (loop, log, name)) {
    log.push_back(name + "1");
    CO2_AWAIT(co2::scheduleOn(loop));
    log.push_back(name + "2");
    CO2_AWAIT(co2::scheduleOn(loop));
    log.push_back(name + "3");
}
CO2_END

auto both(uvco::Loop& loop, std::vector<std::string>& log)
    CO2_BEG(co2::Task<>, (loop, log)) {
    CO2_AWAIT(co2::whenAll(interleave(loop, log, "a"), interleave(loop, log, "b")));
}
CO2_END

auto readsToken(co2::stop_source& source) CO2_BEG(co2::Task<bool>, (source), co2::stop_token token;) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    CO2_RETURN(token == source.get_token());
}
CO2_END

} // namespace

TEST_CASE("Loop is current on its thread") {
    CHECK(uvco::Loop::current() == nullptr);
    {
        uvco::Loop loop;
        CHECK(uvco::Loop::current() == &loop);
        CHECK(loop.isLoopThread());
        CHECK(&uvco::Loop::from(loop.raw()) == &loop);
    }
    CHECK(uvco::Loop::current() == nullptr);
}

TEST_CASE("run() returns the task value (inline completion)") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, answer(loop)) == 42);
}

TEST_CASE("run() drives local scheduleOn hops") {
    uvco::Loop loop;
    bool ran = false;
    uvco::run(loop, nothing(loop, ran));
    CHECK(ran);
}

TEST_CASE("many local yields do not grow the stack") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, yields(loop, 200000)) == 200000);
}

TEST_CASE("run() can be called repeatedly on the same loop") {
    uvco::Loop loop;
    for (int i = 0; i < 100; ++i) CHECK(uvco::run(loop, yields(loop, 3)) == 3);
}

TEST_CASE("run() rethrows the task exception") {
    uvco::Loop loop;
    bool caught = false;
    try {
        static_cast<void>(uvco::run(loop, throws(loop)));
    } catch (Boom const&) {
        caught = true;
    }
    CHECK(caught);
}

TEST_CASE("local queue is FIFO and batches interleave") {
    uvco::Loop loop;
    std::vector<std::string> log;
    uvco::run(loop, both(loop, log));
    CHECK((log == std::vector<std::string>{"a1", "b1", "a2", "b2", "a3", "b3"}));
}

TEST_CASE("run() forwards the root stop_token") {
    uvco::Loop loop;
    co2::stop_source source;
    CHECK(uvco::run(loop, readsToken(source), source.get_token()));
}

UVCO_TEST_MAIN
