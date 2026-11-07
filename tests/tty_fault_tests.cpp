#include <cerrno>
#include <cstdlib>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include "test.hpp"
#include "uvco/stream/tty.hpp"
namespace { bool failNew=false,failInit=false; int auxiliary=-1; }
extern "C" void* UVCO_TEST_REAL_NEW(std::size_t);
extern "C" void* UVCO_TEST_WRAP_NEW(std::size_t n) {
    if(failNew){failNew=false;throw std::bad_alloc{};}
    return UVCO_TEST_REAL_NEW(n);
}
extern "C" int __real_uv_tty_init(uv_loop_t*,uv_tty_t*,uv_file,int);
extern "C" int __wrap_uv_tty_init(uv_loop_t* l,uv_tty_t* h,uv_file fd,int readable) {
    if(failInit){auxiliary=fd;return UV_ENOSYS;}
    return __real_uv_tty_init(l,h,fd,readable);
}
namespace {
int descriptors() {auto* d=opendir("/proc/self/fd");CHECK(d);int n=0;while(readdir(d))++n;closedir(d);return n;}
struct Pty {
    int master=-1,slave=-1;
    Pty(){master=posix_openpt(O_RDWR|O_NOCTTY);CHECK(master>=0);CHECK(grantpt(master)==0);CHECK(unlockpt(master)==0);
        slave=open(ptsname(master),O_RDWR|O_NOCTTY);CHECK(slave>=0);}
    ~Pty(){if(slave>=0)static_cast<void>(close(slave));if(master>=0)static_cast<void>(close(master));}
};
}
TEST_CASE("TTY init failure closes auxiliary dup, not caller fd, and never closes an uninitialized handle") {
    Pty p;uvco::Loop loop;int const before=descriptors();failInit=true;
    auto tty=uvco::Tty::duplicate(loop,p.slave);failInit=false;
    CHECK(!tty);CHECK(tty.error()==static_cast<uvco::Errc>(UV_ENOSYS));CHECK(auxiliary>=0);
    CHECK(fcntl(auxiliary,F_GETFD)==-1 && errno==EBADF);CHECK(fcntl(p.slave,F_GETFD)>=0);
    CHECK(descriptors()==before);loop.runUntilIdle();
}
TEST_CASE("TTY control-block allocation failure releases duplicated descriptor") {
    Pty p;uvco::Loop loop;int const before=descriptors();failNew=true;
    auto tty=uvco::Tty::duplicate(loop,p.slave);failNew=false;
    CHECK(!tty);CHECK(tty.error()==uvco::Errc::no_memory);CHECK(fcntl(p.slave,F_GETFD)>=0);
    CHECK(descriptors()==before);
}
UVCO_TEST_MAIN
