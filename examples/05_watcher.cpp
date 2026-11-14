#include <chrono>
#include <string>
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>
#include "fail.hpp"

using namespace std::chrono_literals;

auto watchFile(uvco::Loop& loop) CO2_BEG(co2::Task<int>, (loop),
        uvco::Result<void> w; uvco::Result<uvco::fs::FsPollChange> change;
        uvco::Result<uvco::fs::FsPoll> watcher; uvco::Result<void> u;
        std::string path;) {
    path = "uvco-example-05.watch";
    CO2_AWAIT_SET(w, uvco::fs::writeFile(loop, path, std::string{"v1"}));
    if (not expect(w, "create file")) CO2_RETURN(1);

    watcher = uvco::fs::FsPoll::start(loop, path, 50ms);
    if (not expect(watcher, "FsPoll::start")) CO2_RETURN(1);
    // 等首次 stat 完成后再改文件，避免把变更抢在基线之前。
    CO2_AWAIT_SET(w, uvco::sleep(loop, 20ms));
    if (not expect(w, "settle")) CO2_RETURN(1);

    CO2_AWAIT_SET(w, uvco::fs::writeFile(loop, path, std::string{"v2-changed"}));
    if (not expect(w, "change file")) CO2_RETURN(1);
    CO2_AWAIT_SET(change, watcher->next());
    if (not expect(change, "FsPoll::next")) CO2_RETURN(1);

    CO2_AWAIT(watcher->close());
    CO2_AWAIT_SET(u, uvco::fs::unlink(loop, path));
    if (not expect(u, "unlink")) CO2_RETURN(1);
    CO2_RETURN(0);
}
CO2_END

int main() {
    uvco::Loop loop;
    return uvco::run(loop, watchFile(loop));
}
