#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/stream/tty.hpp"
#include "uvco/core/run.hpp"
#include "uvco/sync/timeout.hpp"
#ifndef _WIN32
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <dirent.h>
#endif
namespace {
struct Pty {
    int master=-1,slave=-1;
    Pty() {
        master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK); CHECK(master>=0);
        CHECK(grantpt(master)==0); CHECK(unlockpt(master)==0);
        slave=::open(ptsname(master),O_RDWR|O_NOCTTY); CHECK(slave>=0);
        struct winsize size{}; size.ws_col=80; size.ws_row=24; CHECK(ioctl(slave,TIOCSWINSZ,&size)==0);
    }
    ~Pty(){if(slave>=0)static_cast<void>(::close(slave));if(master>=0)static_cast<void>(::close(master));}
};
struct Frame {
    uvco::detail::co2compat::FrameHeader header{&uvco::detail::co2compat::noopDestroy,&step,false,false}; int count=0;
    static uvco::detail::co2compat::FrameHeader* step(uvco::detail::co2compat::FrameHeader* h) {
        ++reinterpret_cast<Frame*>(h)->count; return nullptr;
    }
    co2::coroutine_handle<> handle() { return co2::coroutine_handle<>::from_address(&header); }
};
struct Tty : uvco::Tty {
    explicit Tty(uvco::Tty&& tty) : uvco::Tty{std::move(tty)} {}
    auto& impl() { return block(); }
};
template<class P> void drive(uvco::Loop& loop, P pred) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{5};
    while(!pred() && std::chrono::steady_clock::now()<deadline) {
        static_cast<void>(uv_run(loop.raw(),UV_RUN_NOWAIT)); std::this_thread::yield();
    }
    CHECK(pred());
}
auto read(uvco::Tty& tty, char* buffer, std::size_t length)
    CO2_BEG(co2::Task<uvco::Result<std::size_t>>, (tty,buffer,length),
            std::size_t total{}; uvco::Result<std::size_t> r;) {
    while(total<length) {
        CO2_AWAIT_SET(r,uvco::timeout(tty.loop(),std::chrono::seconds{5},tty.read(uvco::buffer(buffer+total,length-total))));
        if(!r) CO2_RETURN(r.error());
        total+=*r;
    }
    CO2_RETURN(total);
} CO2_END
auto write(uvco::Tty& tty)
    CO2_BEG(co2::Task<uvco::Result<void>>, (tty), uvco::Result<void> r;) {
    CO2_AWAIT_SET(r,tty.write(uvco::buffer("out",3))); CO2_RETURN(r);
} CO2_END
}
TEST_CASE("TTY duplicate streams through a PTY and never adopts the original descriptor") {
    Pty p; uvco::Loop loop; auto made=uvco::Tty::duplicate(loop,p.slave); CHECK(made);
    CHECK(fcntl(p.slave,F_GETFD)>=0); CHECK(made->setMode(uvco::TtyMode::Io));
    auto size=made->getSize(); CHECK(size); CHECK(size->width==80 && size->height==24);
    CHECK(::write(p.master,"in!",3)==3); char input[3]; auto n=uvco::run(loop,read(*made,input,3));
    CHECK(n && *n==3); CHECK(std::memcmp(input,"in!",3)==0); CHECK(uvco::run(loop,write(*made)));
    char output[3]; std::size_t total=0;
    drive(loop,[&] {
        if(total==3) return true;
        auto count=::read(p.master,output+total,3-total);
        if(count>0) total+=static_cast<std::size_t>(count);
        return total==3;
    });
    CHECK(std::memcmp(output,"out",3)==0);
    auto attempted=made->tryWrite(uvco::buffer("try",3)); CHECK(attempted && *attempted>0 && *attempted<=3);
    total=0;
    drive(loop,[&] {
        if(total==*attempted) return true;
        auto count=::read(p.master,output+total,*attempted-total);
        if(count>0) total+=static_cast<std::size_t>(count);
        return total==*attempted;
    });
    CHECK(std::memcmp(output,"try",*attempted)==0); CHECK(made->restore());
    static_cast<void>(made->close()); loop.runUntilIdle(); CHECK(fcntl(p.slave,F_GETFD)>=0);
}
TEST_CASE("TTY read reservation blocks mode changes, cancellation and close restore modes safely") {
    Pty p; uvco::Loop loop; termios original{}; CHECK(tcgetattr(p.slave,&original)==0);
    auto made=uvco::Tty::duplicate(loop,p.slave); CHECK(made); Tty tty{std::move(*made)};
    CHECK(tty.setMode(uvco::TtyMode::Raw)); Frame f; co2::stop_source source; char data[16];
    CHECK(tty.impl().suspendRead(f.handle(),uvco::buffer(data),source.get_token()));
    CHECK(tty.setMode(uvco::TtyMode::Normal).error()==uvco::Errc::busy);
    source.request_stop(); drive(loop,[&]{return f.count==1;}); CHECK(tty.impl().resumeRead().error()==uvco::Errc::canceled);
    CHECK(tty.impl().suspendRead(f.handle(),uvco::buffer(data),{}));
    auto* block=&tty.impl(); uvco::detail::HandleRef<uvco::detail::TtyBlock> lease{block};
    static_cast<void>(tty.close()); loop.runUntilIdle(); CHECK(f.count==2);
    CHECK(block->resumeRead().error()==uvco::Errc::aborted);
    termios restored{}; CHECK(tcgetattr(p.slave,&restored)==0); CHECK(restored.c_lflag==original.c_lflag);
}
TEST_CASE("Loop-first TTY teardown restores terminal mode and leaves an orphan owner safe") {
    Pty p; termios original{}; CHECK(tcgetattr(p.slave,&original)==0); uvco::Tty tty;
    {
        uvco::Loop loop; auto made=uvco::Tty::duplicate(loop,p.slave); CHECK(made); tty=std::move(*made);
        CHECK(tty.setMode(uvco::TtyMode::Raw));
    }
    termios restored{}; CHECK(tcgetattr(p.slave,&restored)==0); CHECK(restored.c_lflag==original.c_lflag);
    static_cast<void>(tty.close()); CHECK(fcntl(p.slave,F_GETFD)>=0);
}
#ifdef __linux__
namespace { int descriptors() { auto* dir=opendir("/proc/self/fd"); CHECK(dir); int n=0; while(readdir(dir)) ++n; closedir(dir); return n; } }
TEST_CASE("TTY reopened descriptor and auxiliary duplicate do not leak") {
    {Pty warm; uvco::Loop loop; auto t=uvco::Tty::duplicate(loop,warm.slave); CHECK(t);}
    int const before=descriptors();
    for(int i=0;i<32;++i) {Pty p; uvco::Loop loop; auto tty=uvco::Tty::duplicate(loop,p.slave); CHECK(tty);}
    CHECK(descriptors()==before);
}
#endif
#endif
TEST_CASE("TTY invalid descriptor is a Result error") {
    uvco::Loop loop; CHECK(!uvco::Tty::duplicate(loop,-1));
#ifndef _WIN32
    int fd[2]; CHECK(pipe(fd)==0); CHECK(!uvco::Tty::duplicate(loop,fd[0]));
    CHECK(fcntl(fd[0],F_GETFD)>=0); CHECK(::close(fd[0])==0); CHECK(::close(fd[1])==0);
#endif
}
#ifdef _WIN32
TEST_CASE("Windows console duplicate uses output capability when an actual console is available") {
    if(uv_guess_handle(1)!=UV_TTY) {
        std::cout<<"[ SKIP ] redirected stdout is not a native Windows console\n"; return;
    }
    uvco::Loop loop; auto tty=uvco::Tty::duplicate(loop,1); CHECK(tty); CHECK(tty->isWritable());
    CHECK(tty->getSize()); CHECK(!tty->setMode(uvco::TtyMode::Raw));
    static_cast<void>(tty->close()); loop.runUntilIdle();
}
#endif
UVCO_TEST_MAIN
