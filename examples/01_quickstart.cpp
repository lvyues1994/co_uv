#include <chrono>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

using namespace std::chrono_literals;

auto mainTask(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 1ms));
    if (not expect(r, "sleep")) CO2_RETURN(1);
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, mainTask(loop));
}
