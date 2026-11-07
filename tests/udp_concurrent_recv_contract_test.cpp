// 契约违规测试：同一个 UDP 套接字上同时有两个 recv()（docs/architecture.md §4.7）。

#include <cstdlib>
#include <iostream>
#include <vector>

#include <co2/co2.hpp>

#include "uvco/uvco.hpp"

namespace {

auto recvOnce(uvco::Udp& udp, std::vector<char>& buf)
    CO2_BEG(co2::Task<>, (udp, buf), uvco::Result<uvco::Datagram> d{uvco::Errc::canceled};) {
    CO2_AWAIT_SET(d, udp.recv(uvco::buffer(buf)));
}
CO2_END

auto scenario(uvco::Udp& udp, std::vector<char>& a, std::vector<char>& b)
    CO2_BEG(co2::Task<>, (udp, a, b)) {
    CO2_AWAIT(co2::whenAll(recvOnce(udp, a), recvOnce(udp, b))); // 第二个 recv 违规
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler([](co2::ContractViolation const& violation) {
        std::cout << "contract violation (expected): " << violation.condition << std::endl;
        std::_Exit(EXIT_SUCCESS);
    });
    uvco::Loop loop;
    uvco::Udp udp{loop};
    static_cast<void>(udp.bind(*uvco::SocketAddress::parse("127.0.0.1", 0)));
    std::vector<char> a(8), b(8);
    uvco::run(loop, scenario(udp, a, b));
    std::cerr << "two concurrent recv() calls were not rejected" << std::endl;
    return EXIT_FAILURE;
}
