#include <string>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

#ifndef UVCO_EXAMPLE_HELPER
#error "UVCO_EXAMPLE_HELPER must be the compiled echo helper path"
#endif

auto captureEcho(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<uvco::CaptureResult> r; uvco::ProcessOptions options;
        uvco::CaptureOptions io;) {
    options.file = UVCO_EXAMPLE_HELPER;
    io.input = "hello\n";
    CO2_AWAIT_SET(r, uvco::communicate(loop, options, io));
    if (not expect(r, "communicate")) CO2_RETURN(1);
    if (r->status.exitStatus != 0 || r->status.termSignal != 0)
        CO2_RETURN(fail("helper exit status"));
    if (r->stdoutData.find("echo:hello") == std::string::npos)
        CO2_RETURN(fail("helper stdout"));
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, captureEcho(loop));
}
