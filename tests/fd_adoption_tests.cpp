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
UVCO_TEST_MAIN
