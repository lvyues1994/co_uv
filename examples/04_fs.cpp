#include <string>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

auto roundtrip(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<void> w; uvco::fs::StringResult text; uvco::Result<void> u;
        std::string path;) {
    path = "uvco-example-04.dat";
    CO2_AWAIT_SET(w, uvco::fs::writeFile(loop, path, std::string{"uvco-fs"}));
    if (not expect(w, "writeFile")) CO2_RETURN(1);
    CO2_AWAIT_SET(text, uvco::fs::readFile(loop, path));
    if (not expect(text, "readFile") || *text != "uvco-fs") CO2_RETURN(fail("file contents"));
    CO2_AWAIT_SET(u, uvco::fs::unlink(loop, path));
    if (not expect(u, "unlink")) CO2_RETURN(1);
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, roundtrip(loop));
}
