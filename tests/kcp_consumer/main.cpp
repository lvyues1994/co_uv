#include <uvco/kcp.hpp>
std::size_t otherLimit();
static bool copy(void *, uvco::ConstBuffer) noexcept {
    return true;
}
int main() {
    auto c = uvco::KcpCore::create(1, uvco::CoreOptions{}, {nullptr, &copy});
    return c && c->messageLimit() == 65536 && otherLimit() == 65536 ? 0 : 1;
}
