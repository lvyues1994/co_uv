#include <chrono>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

using namespace std::chrono_literals;

auto demo(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<void> timed; uvco::Result<void> fast;
        uvco::Result<uvco::WhenAnyResult<void, void>> any;) {
    CO2_AWAIT_SET(timed, uvco::timeout(loop, 5ms, uvco::sleep(loop, 1s)));
    if (timed) CO2_RETURN(fail("expected timed_out"));
    if (timed.error() != uvco::Errc::timed_out) CO2_RETURN(fail("timeout error"));

    CO2_AWAIT_SET(fast, uvco::timeout(loop, 1s, uvco::sleep(loop, 1ms)));
    if (not expect(fast, "timeout of short sleep")) CO2_RETURN(1);

    CO2_AWAIT_SET(any, uvco::whenAny(uvco::sleep(loop, 1ms), uvco::sleep(loop, 1s)));
    if (not expect(any, "whenAny") || any->index() != 0)
        CO2_RETURN(fail("whenAny winner"));
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, demo(loop));
}
