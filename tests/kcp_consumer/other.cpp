#include <uvco/kcp.hpp>
static bool copy(void*,uvco::ConstBuffer) noexcept {return true;}
std::size_t otherLimit(){auto c=uvco::KcpCore::create(2,uvco::CoreOptions{},{nullptr,&copy});
    return c?c->messageLimit():0;}
