// 契约违规测试：同一个流上同时有两个 read()（docs/architecture.md §4.6）。

#include <cstdlib>
#include <iostream>
#include <vector>

#include <co2/co2.hpp>

#include "uvco/uvco.hpp"

namespace {

auto readOnce(uvco::Tcp& s, std::vector<char>& buf)
    CO2_BEG(co2::Task<>, (s, buf), uvco::Result<std::size_t> n;) {
    CO2_AWAIT_SET(n, s.read(uvco::buffer(buf)));
}
CO2_END

auto scenario(uvco::Tcp& server, uvco::Tcp& client, std::vector<char>& a, std::vector<char>& b)
    CO2_BEG(co2::Task<>, (server, client, a, b), uvco::Result<void> r;
            uvco::Result<uvco::SocketAddress> addr;) {
    addr = server.localAddress();
    CO2_AWAIT_SET(r, client.connect(*addr));
    CO2_AWAIT(co2::whenAll(readOnce(client, a), readOnce(client, b))); // 第二个 read 违规
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    uvco::Loop loop;
    uvco::Tcp server{loop};
    uvco::Tcp client{loop};
    static_cast<void>(server.bind(*uvco::SocketAddress::parse("127.0.0.1", 0)));
    static_cast<void>(server.listen());
    std::vector<char> a(8), b(8);
    uvco::run(loop, scenario(server, client, a, b));
    std::cerr << "two concurrent read() calls were not rejected" << std::endl;
    return EXIT_FAILURE;
}
