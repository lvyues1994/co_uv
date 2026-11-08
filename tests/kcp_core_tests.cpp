#include <cstdio>
#if __has_include(<uvco/kcp/core.hpp>)
#include <uvco/kcp/core.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while(false)
using namespace uvco;
struct Packet { std::array<char,1200> bytes{}; std::size_t len=0; std::uint64_t due=0; };
struct Wire {
    std::vector<Packet> packets = std::vector<Packet>(256);
    std::size_t count=0, emitted=0, losses=0;
    std::uint64_t now=0;
    bool lossy=false;
    static bool output(void* p,ConstBuffer b) noexcept {
        auto& w=*static_cast<Wire*>(p);
        ++w.emitted;
        if ((w.lossy && w.emitted%7==0) || w.count==w.packets.size()) { ++w.losses; return false; }
        if(b.size>1200) std::abort();
        auto& packet=w.packets[w.count++];
        std::memcpy(packet.bytes.data(),b.data,b.size);packet.len=b.size;
        packet.due=w.now+7+(w.emitted%5)*3;
        return true;
    }
    void deliver(KcpCore& target) {
        for(std::size_t n=count;n>0;--n) { // reproducible reordering
            auto i=n-1;
            if(packets[i].due<=now) {
                CHECK(target.input({packets[i].bytes.data(),packets[i].len}));
                packets[i]=packets[--count];
            }
        }
    }
};
static void transfer(bool loss,std::uint64_t start) {
    Wire ab,ba;ab.lossy=ba.lossy=loss;
    CoreOptions options;
    auto ar=KcpCore::create(42,options,{&ab,&Wire::output});
    auto br=KcpCore::create(42,options,{&ba,&Wire::output});CHECK(ar&&br);
    KcpCore a=std::move(*ar),b=std::move(*br); // stable output user after move
    std::vector<char> message(65536);
    for(std::size_t i=0;i<message.size();++i)message[i]=static_cast<char>(i%127);
    auto sent=a.sendMessage(buffer(message));CHECK(sent&&*sent==message.size());
    bool received=false;
    for(std::uint64_t t=start;t<start+60000;++t) {
        ab.now=ba.now=t;
        a.update(static_cast<std::uint32_t>(t));b.update(static_cast<std::uint32_t>(t));
        ab.deliver(b);ba.deliver(a);b.flush();a.flush();
        if(!received) {
            auto msg=b.recvMessage();
            if(msg) {CHECK(*msg==message);received=true;}
            else CHECK(msg.error()==Errc::again);
        }
        if(received&&a.waitingSegments()==0)break;
    }
    CHECK(received&&a.waitingSegments()==0);
    CHECK(b.recvMessage().error()==Errc::again);
    if(loss)CHECK(ab.losses+ba.losses>0);
    CHECK(a.sendMessage({nullptr,0}));
    for(std::uint64_t t=start+60000;t<start+61000;++t){
        ab.now=ba.now=t;a.update(static_cast<std::uint32_t>(t));b.update(static_cast<std::uint32_t>(t));ab.deliver(b);ba.deliver(a);
        auto empty=b.recvMessage();if(empty){CHECK(empty->empty());return;}
    }
    CHECK(false);
}
static void validation() {
    Wire w;
    CoreOptions o;
    CHECK(KcpCore::create(1,o,{nullptr,nullptr}).error()==Errc::invalid_argument);
    o.mtu=24;CHECK(KcpCore::create(1,o,{&w,&Wire::output}).error()==Errc::invalid_argument);
    o=CoreOptions{};o.messageLimit=1000000;
    auto r=KcpCore::create(5,o,{&w,&Wire::output});CHECK(r);
    std::vector<char> valid(127*1176,'v'),invalid(128*1176,'x');
    CHECK(r->messageLimit()==valid.size());CHECK(r->sendMessage(buffer(invalid)).error()==static_cast<Errc>(UV_EMSGSIZE));
    CHECK(r->waitingSegments()==0);CHECK(r->sendMessage(buffer(valid)));CHECK(r->waitingSegments()==127);
    CHECK(r->write(buffer(valid)).error()==Errc::invalid_argument);
    char bad[25]{};CHECK(!r->input({bad,sizeof bad}));CHECK(!r->failed());
    Wire ab,ba;auto a=KcpCore::create(7,CoreOptions{},{&ab,&Wire::output});auto b=KcpCore::create(7,CoreOptions{},{&ba,&Wire::output});CHECK(a&&b);
    std::vector<char> small(3,'q');CHECK(a->sendMessage(buffer(small)));a->update(0);a->update(20);CHECK(ab.count>0);
    std::vector<char> malformed(ab.packets[0].bytes.data(),ab.packets[0].bytes.data()+ab.packets[0].len);
    malformed.push_back('x');CHECK(!b->input(buffer(malformed)));CHECK(b->recvMessage().error()==Errc::again); // full datagram preflight
    CHECK(KcpCore::delayUntil(0xfffffff0u,0x10u)==32);
    CHECK(KcpCore::delayUntil(0x10u,0xfffffff0u)==0);
}
static void streamAndDeadLink() {
    Wire ab,ba;CoreOptions o;o.stream=true;
    auto a=KcpCore::create(9,o,{&ab,&Wire::output});o.messageLimit=1;auto b=KcpCore::create(9,o,{&ba,&Wire::output});CHECK(a&&b);
    CHECK(a->sendMessage({nullptr,0}).error()==Errc::invalid_argument);
    std::vector<char> payload(3000,'s');auto written=a->write(buffer(payload));CHECK(written&&*written==payload.size());
    std::size_t total=0;
    for(std::uint32_t t=0;t<5000&&total<payload.size();++t){
        ab.now=ba.now=t;a->update(t);b->update(t);ab.deliver(*b);ba.deliver(*a);
        auto unit=b->recvStreamUnit();if(unit){total+=unit->size();for(auto c:*unit)CHECK(c=='s');}
    }CHECK(total==payload.size());
    Wire blackhole;blackhole.count=blackhole.packets.size();o=CoreOptions{};o.deadLink=2;
    auto c=KcpCore::create(10,o,{&blackhole,&Wire::output});CHECK(c);CHECK(c->sendMessage({"x",1}));
    for(std::uint32_t t=0;t<10000&&!c->deadLink();t+=10)c->update(t);
    CHECK(c->deadLink());CHECK(c->outputDrops()>0);
}
int main(){validation();transfer(false,0);transfer(true,0);transfer(true,0xfffffff0ull);streamAndDeadLink();std::puts("KcpCore deterministic tests PASS");}
#else
int main(){std::fputs("FAIL: KcpCore has not been implemented\n",stderr);return 1;}
#endif
