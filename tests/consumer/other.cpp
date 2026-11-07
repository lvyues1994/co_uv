#include <uvco/uvco.hpp>
auto other(uvco::Loop& loop) CO2_BEG(co2::Task<int>,(loop),uvco::Result<void> r;) {
    CO2_AWAIT_SET(r,uvco::yieldNextTurn(loop));
    CO2_RETURN(r ? 7 : 0);
} CO2_END
