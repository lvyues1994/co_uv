#include <chrono>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

using namespace std::chrono_literals;

auto nap(uvco::Loop& loop) CO2_BEG(co2::Task<uvco::Result<void>>, (loop), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r, uvco::sleep(loop, 5s));
    CO2_RETURN(r);
}
CO2_END

int main() {
    uvco::Loop loop;
    co2::stop_source source;
    source.request_stop();
    auto const r = uvco::run(loop, nap(loop), source.get_token());
    if (r) return fail("expected canceled sleep");
    if (r.error() != uvco::Errc::canceled) return fail("cancellation error");
    return 0;
}
