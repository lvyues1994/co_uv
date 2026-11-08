#include <uvco/kcp.hpp>
#include <uvco/uvco.hpp>
#include <array>
#include "test.hpp"
namespace {
struct Closing {
    uv_handle_t *handle = nullptr;
    uv_close_cb callback = nullptr;
};
std::array<Closing, 64> closings{};
bool inNativeClose = false;
void wrappedClose(uv_handle_t *h) {
    uv_close_cb cb = nullptr;
    for (auto &c : closings)
        if (c.handle == h) {
            cb = c.callback;
            c = {};
            break;
        }
    if (!cb)
        std::abort();
    inNativeClose = true;
    cb(h);
    inNativeClose = false;
}
auto closeSession(uvco::KcpSession &s, bool &inlineResume) CO2_BEG(co2::Task<>,(s,inlineResume)) {
    CO2_AWAIT(s.close());
    inlineResume = inNativeClose;
    CO2_RETURN();
}
CO2_END
auto closeEndpoint(uvco::KcpEndpoint &e, bool &inlineResume) CO2_BEG(co2::Task<>,(e,inlineResume)) {
    CO2_AWAIT(e.close());
    inlineResume = inNativeClose;
    CO2_RETURN();
}
CO2_END
}
extern "C" void __real_uv_close(uv_handle_t *, uv_close_cb);
extern "C" void __wrap_uv_close(uv_handle_t *h, uv_close_cb cb) {
    if (!cb) {
        __real_uv_close(h, cb);
        return;
    }
    for (auto &c : closings)
        if (!c.handle) {
            c.handle = h;
            c.callback = cb;
            __real_uv_close(h, &wrappedClose);
            return;
        }
    std::abort();
}
TEST_CASE("KCP native close completion resumes through the schedule queue") {
    uvco::Loop loop;
    auto e = uvco::KcpEndpoint::create(loop, *uvco::SocketAddress::ipv4("127.0.0.1", 0));
    CHECK(e);
    auto s = e->openSession(*uvco::SocketAddress::ipv4("127.0.0.1", 9), 1);
    CHECK(s);
    bool inlineResume = false;
    uvco::run(loop, closeSession(*s, inlineResume));
    CHECK(!inlineResume);
    uvco::run(loop, closeEndpoint(*e, inlineResume));
    CHECK(!inlineResume);
    uvco::drain(loop);
}
UVCO_TEST_MAIN
