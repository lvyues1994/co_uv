#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {auto read(uvco::BufferedReader<uvco::Pipe>& reader)
 CO2_BEG(co2::Task<uvco::Result<std::string>>,(reader),uvco::Result<std::string> r;) {
 CO2_AWAIT_SET(r,reader.readToEnd(100));CO2_RETURN(std::move(r));
} CO2_END}
TEST_CASE("Pipe open adopts fd only on success and works with BufferedReader"){
 uvco::Loop loop;int fds[2];CHECK(::pipe(fds)==0);CHECK(::write(fds[1],"hello",5)==5);CHECK(::close(fds[1])==0);
 auto stream=uvco::Pipe::open(loop,fds[0]);CHECK(stream);uvco::BufferedReader<uvco::Pipe> reader{*stream};
 auto r=uvco::run(loop,read(reader));CHECK(r);CHECK(*r=="hello");static_cast<void>(stream->close());loop.runUntilIdle();
 CHECK(::fcntl(fds[0],F_GETFD)==-1);
}
TEST_CASE("Tcp open failure leaves caller fd owned"){
 uvco::Loop loop;int fds[2];CHECK(::pipe(fds)==0);auto r=uvco::Tcp::open(loop,static_cast<uv_os_sock_t>(-1));CHECK(!r);
 CHECK(::fcntl(fds[0],F_GETFD)>=0);CHECK(::close(fds[0])==0);CHECK(::close(fds[1])==0);loop.runUntilIdle();
 int fd=::socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);auto tcp=uvco::Tcp::open(loop,fd);CHECK(tcp);static_cast<void>(tcp->close());loop.runUntilIdle();CHECK(::fcntl(fd,F_GETFD)==-1);
}
#ifdef __linux__
TEST_CASE("duplicate fd adoption failure cannot close the original owner descriptor"){
 uvco::Loop loop;int fd=::socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
 auto first=uvco::Tcp::open(loop,fd);CHECK(first);auto address=uvco::SocketAddress::ipv4("127.0.0.1",0);CHECK(address);
 CHECK(first->bind(*address));CHECK(first->listen());auto duplicate=uvco::Tcp::open(loop,fd);
 CHECK(!duplicate);CHECK(duplicate.error()==static_cast<uvco::Errc>(UV_EEXIST));
 uv_run(loop.raw(),UV_RUN_NOWAIT);CHECK(::fcntl(fd,F_GETFD)>=0);
 static_cast<void>(first->close());loop.runUntilIdle();CHECK(::fcntl(fd,F_GETFD)==-1);
 int fds[2];CHECK(::pipe(fds)==0);auto pipe=uvco::Pipe::open(loop,fds[0]);CHECK(pipe);
 CHECK(uv_read_start(reinterpret_cast<uv_stream_t*>(pipe->raw()),[](uv_handle_t*,std::size_t,uv_buf_t* b){b->base=nullptr;b->len=0;},[](uv_stream_t*,ssize_t,uv_buf_t const*){})==0);
 auto duplicatePipe=uvco::Pipe::open(loop,fds[0]);CHECK(!duplicatePipe);
 CHECK(duplicatePipe.error()==static_cast<uvco::Errc>(UV_EEXIST));uv_run(loop.raw(),UV_RUN_NOWAIT);CHECK(::fcntl(fds[0],F_GETFD)>=0);
 static_cast<void>(pipe->close());loop.runUntilIdle();CHECK(::fcntl(fds[0],F_GETFD)==-1);CHECK(::close(fds[1])==0);
}
#endif
UVCO_TEST_MAIN
