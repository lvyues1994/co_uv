#include <algorithm>
#include <cstring>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
struct Source {
 std::string bytes;std::size_t pos=0;bool canceled=false;
 struct Read {Source* source;uvco::MutableBuffer buf;bool await_ready()const{return true;}
 void await_suspend(co2::coroutine_handle<>)const{}
 uvco::Result<std::size_t> await_resume(){if(source->canceled){source->canceled=false;return uvco::Errc::canceled;}
 if(source->pos==source->bytes.size())return uvco::Errc::eof;
 auto n=std::min(std::size_t{2},std::min(buf.size,source->bytes.size()-source->pos));std::memcpy(buf.data,source->bytes.data()+source->pos,n);source->pos+=n;return n;}};
 Read read(uvco::MutableBuffer b){return {this,b};}
};
auto sequence(uvco::BufferedReader<Source>& reader)
 CO2_BEG(co2::Task<bool>,(reader),uvco::Result<std::string> r;) {
 CO2_AWAIT_SET(r,reader.readLine(1));if(r || r.error()!=static_cast<uvco::Errc>(UV_ENOBUFS))CO2_RETURN(false);
 CO2_AWAIT_SET(r,reader.readLine(2));if(!r || *r!="ab")CO2_RETURN(false);
 CO2_AWAIT_SET(r,reader.readExactly(2));if(!r || *r!="cd")CO2_RETURN(false);
 CO2_AWAIT_SET(r,reader.readLine(2));if(!r || *r!="ef")CO2_RETURN(false);
 CO2_AWAIT_SET(r,reader.readToEnd(2));if(r)CO2_RETURN(false);
 CO2_AWAIT_SET(r,reader.readToEnd(3));if(!r || *r!="end")CO2_RETURN(false);
 CO2_AWAIT_SET(r,reader.readExactly(1));CO2_RETURN(!r && r.error()==uvco::Errc::eof);
} CO2_END
}
TEST_CASE("BufferedReader retains read-ahead across exact, line, caps and EOF"){
 uvco::Loop loop;Source source{"ab\ncdef\nend",0,false};uvco::BufferedReader<Source> reader{source};CHECK(uvco::run(loop,sequence(reader)));
}
UVCO_TEST_MAIN
