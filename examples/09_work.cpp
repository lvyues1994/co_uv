#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

static int workSeven() { return 7; }

static void workToken(co2::stop_token token) {
    static_cast<void>(token.stop_requested());
}

auto compute(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<int> r; uvco::Result<void> done;) {
    CO2_AWAIT_SET(r, uvco::queueWork(loop, workSeven));
    if (not expect(r, "queueWork") || *r != 7) CO2_RETURN(fail("work result"));

    // 无 token 的 callable 不能协作取消；带 token 的可以轮询。两者都在
    // after-work 之后回到 loop。已经算完的结果不会被晚到的取消丢掉。
    CO2_AWAIT_SET(done, uvco::queueWork(loop, workToken));
    if (not expect(done, "queueWork token")) CO2_RETURN(1);
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, compute(loop));
}
