#pragma once
#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <co2/co2.hpp>
#include "uvco/dns/resolver.hpp"
#include "uvco/stream/tcp.hpp"
#include "uvco/sync/event.hpp"
#include "uvco/sync/timeout.hpp"
namespace uvco {
using TcpConnector = std::function<co2::Task<Result<Tcp>>(Loop&,SocketAddress)>;
struct ConnectOptions {
    std::chrono::milliseconds resolutionDelay{50},attemptDelay{250},minimumDelay{100};
    std::chrono::milliseconds perAttemptTimeout{5000},overallTimeout{30000};
    std::size_t maxConcurrent=4;
    // Owned injection points. Policy sorts within each family's system order;
    // the coordinator interleaves families, not a complete RFC6724 implementation.
    std::function<void(std::vector<SocketAddress>&)> addressPolicy;
    TcpConnector connector;
    DnsClock clock;
};
namespace detail {
inline auto connectOne(Loop& loop,SocketAddress address)
 CO2_BEG(co2::Task<Result<Tcp>>,(loop,address),Tcp socket{loop};Result<void> r;) {
    CO2_AWAIT_SET(r,socket.connect(address));
    if(!r){CO2_AWAIT(socket.close());CO2_RETURN(r.error());}
    CO2_RETURN(std::move(socket));
} CO2_END
class CatchConnectTask {
  public:
    explicit CatchConnectTask(co2::Task<> task):task_{std::move(task)}{}
    bool await_ready() const noexcept {return task_.await_ready();}
    template<class P> co2::coroutine_handle<> await_suspend(co2::coroutine_handle<P> h) noexcept {return task_.await_suspend(h);}
    std::exception_ptr await_resume() noexcept {try{task_.await_resume();return {};}catch(...){return std::current_exception();}}
  private: co2::Task<> task_;
};
class ConnectGroupAwaiter {
  public:
    ConnectGroupAwaiter(co2::Task<> task,co2::stop_token token):task_{std::move(task)},token_{std::move(token)}{}
    bool await_ready() const noexcept {return false;}
    template<class P> co2::coroutine_handle<> await_suspend(co2::coroutine_handle<P> h) noexcept {
        return co2compat::TaskAccess::arm(task_,h,token_);
    }
    void await_resume(){co2compat::TaskAccess::takeResult(task_);}
  private:co2::Task<> task_;co2::stop_token token_;
};
inline bool sameAddress(SocketAddress const& a,SocketAddress const& b) noexcept {
    if(a.family()!=b.family() || a.port()!=b.port())return false;
    if(a.isIpv4())return reinterpret_cast<sockaddr_in const*>(a.raw())->sin_addr.s_addr==reinterpret_cast<sockaddr_in const*>(b.raw())->sin_addr.s_addr;
    auto* x=reinterpret_cast<sockaddr_in6 const*>(a.raw());auto* y=reinterpret_cast<sockaddr_in6 const*>(b.raw());
    return x->sin6_scope_id==y->sin6_scope_id && std::memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(in6_addr))==0;
}
struct ConnectSlot {
    explicit ConnectSlot(Loop& loop):ready{loop}{}
    Event ready;SocketAddress address;std::size_t index=0;bool busy=false;
};
struct ConnectRaceState {
    Loop& loop;DnsResolver& resolver;ConnectOptions options;Event changed;
    co2::stop_source stop;co2::stop_token parent;CancelHook parentCancel;
    std::vector<std::unique_ptr<ConnectSlot>> slots;
    std::vector<SocketAddress> family[2],used;
    std::size_t position[2]{0,0},active=0;
    bool dnsDone[2]{false,false},done=false,numeric=false,fastFailure=false;
    Errc dnsError[2]{static_cast<Errc>(UV_EAI_NONAME),static_cast<Errc>(UV_EAI_NONAME)};
    std::vector<Errc> errors;
    Tcp winner;Errc error=Errc::canceled;std::exception_ptr exception;
    using Time=std::chrono::steady_clock::time_point;
    Time started,lastLaunch{},v4Ready{};int lastFamily=-1;
    ConnectRaceState(Loop& l,DnsResolver& r,ConnectOptions o):loop{l},resolver{r},options{std::move(o)},changed{l} {
        if(!options.connector)options.connector=connectOne;
        if(!options.clock)options.clock=[] {return std::chrono::steady_clock::now();};

    }
    void finish(Errc e) noexcept {if(done)return;error=e;done=true;stop.request_stop();changed.set();}
    void fail(std::exception_ptr e) noexcept {if(!exception)exception=std::move(e);finish(Errc::canceled);}
    static void canceled(void* p) noexcept {static_cast<ConnectRaceState*>(p)->finish(Errc::canceled);}
    void success(Tcp& socket) {
        if(done)return;
        if(parent.stop_requested()){finish(Errc::canceled);return;}
        if(std::chrono::duration_cast<std::chrono::milliseconds>(options.clock()-started)>=options.overallTimeout){finish(Errc::timed_out);return;}
        winner=std::move(socket);finish(Errc::canceled); // error unused for a committed winner
    }
    bool available(int f) {
        auto& p=position[f];
        while(p<family[f].size()){
            bool duplicate=false;for(auto const& a:used)if(sameAddress(a,family[f][p])){duplicate=true;break;}
            if(!duplicate)return true;
            ++p;
        }
        return false;
    }
    int choose(Time now) {
        bool a=available(0),b=available(1);
        if(lastFamily<0 && !a && b && !dnsDone[0] && now-v4Ready<options.resolutionDelay)return -1;
        int prefer=lastFamily==0?1:0;
        if((prefer==0?a:b))return prefer;
        if((prefer==0?b:a))return 1-prefer;
        return -1;
    }
};
inline auto connectDns(ConnectRaceState& s,std::string host,std::uint16_t port,int family)
 CO2_BEG(co2::Task<>,(s,host,port,family),Result<AddressList> r;addrinfo hints{};) {
    hints.ai_family=family==0?AF_INET6:AF_INET;hints.ai_socktype=SOCK_STREAM;hints.ai_protocol=IPPROTO_TCP;
    CO2_AWAIT_SET(r,s.resolver.resolve(std::move(host),std::to_string(port),hints));
    if(r){s.family[family]=**r;if(s.options.addressPolicy)s.options.addressPolicy(s.family[family]);}
    else s.dnsError[family]=r.error();
    if(family==1)s.v4Ready=s.options.clock();
    s.dnsDone[family]=true;s.changed.set();
} CO2_END
inline auto connectWorker(ConnectRaceState& s,ConnectSlot& slot)
 CO2_BEG(co2::Task<>,(s,slot),Result<void> ready;Result<Tcp> result{Errc::canceled};) {
    while(!s.done){
        CO2_AWAIT_SET(ready,slot.ready.wait());
        if(!ready || s.done)break;
        slot.ready.reset();
        CO2_AWAIT_SET(result,timeout(s.loop,s.options.perAttemptTimeout,s.options.connector(s.loop,slot.address)));
        slot.busy=false;--s.active;
        if(result)s.success(*result);
        else {s.errors[slot.index]=result.error();s.fastFailure=true;}
        s.changed.set();
        if(result){CO2_AWAIT(result->close());}
    }
} CO2_END
inline auto connectController(ConnectRaceState& s)
 CO2_BEG(co2::Task<>,(s),Result<void> wake;ConnectRaceState::Time now;std::chrono::milliseconds delay;
    int family=-1;ConnectSlot* idle=nullptr;) {
    while(!s.done){
        now=s.options.clock();
        if(s.parent.stop_requested()){s.finish(Errc::canceled);break;}
        if(std::chrono::duration_cast<std::chrono::milliseconds>(now-s.started)>=s.options.overallTimeout){s.finish(Errc::timed_out);break;}
        family=s.choose(now);idle=nullptr;
        for(auto& slot:s.slots)if(!slot->busy){idle=slot.get();break;}
        if(family>=0 && idle && (s.used.empty() || now-s.lastLaunch>=(s.fastFailure?s.options.minimumDelay:s.options.attemptDelay))){
            idle->address=s.family[family][s.position[family]++];
            s.used.push_back(idle->address);idle->index=s.errors.size();s.errors.push_back(Errc::canceled);
            idle->busy=true;++s.active;s.lastFamily=family;s.lastLaunch=now;s.fastFailure=false;idle->ready.set();
            continue;
        }
        if(s.dnsDone[0] && s.dnsDone[1] && !s.available(0) && !s.available(1) && s.active==0){
            s.finish(s.errors.empty()?s.dnsError[0]:s.errors.back());break;
        }
        delay=std::chrono::duration_cast<std::chrono::milliseconds>(s.options.overallTimeout-std::chrono::duration_cast<std::chrono::milliseconds>(now-s.started));
        if(family>=0 && idle && !s.used.empty())delay=(std::min)(delay,std::chrono::duration_cast<std::chrono::milliseconds>(
            (s.fastFailure?s.options.minimumDelay:s.options.attemptDelay)-(now-s.lastLaunch)));
        if(s.used.empty() && s.available(1) && !s.dnsDone[0])delay=(std::min)(delay,std::chrono::duration_cast<std::chrono::milliseconds>(s.options.resolutionDelay-(now-s.v4Ready)));
        if(delay.count()<1)delay=std::chrono::milliseconds{1};
        s.changed.reset();
        CO2_AWAIT_SET(wake,timeout(s.loop,delay,s.changed.wait()));
        if(!wake && wake.error()!=Errc::timed_out){s.finish(wake.error());break;}
    }
} CO2_END
inline auto guardConnectTask(ConnectRaceState& s,co2::Task<> task,ConnectSlot* slot=nullptr)
 CO2_BEG(co2::Task<>,(s,task,slot),std::exception_ptr exception;) {
    CO2_AWAIT_SET(exception,CatchConnectTask{std::move(task)});
    if(slot && slot->busy){slot->busy=false;--s.active;}
    if(exception)s.fail(std::move(exception));
} CO2_END
inline auto connectGroup(ConnectRaceState& s,std::string host,std::uint16_t port)
 CO2_BEG(co2::Task<>,(s,host,port),std::vector<co2::Task<>> tasks;) {
    // Allocate ALL task frames before starting any, so construction exceptions
    // cannot abandon an already-started native operation.
    if(!s.numeric){
        tasks.push_back(guardConnectTask(s,connectDns(s,host,port,0)));
        tasks.push_back(guardConnectTask(s,connectDns(s,std::move(host),port,1)));
    }
    for(auto& slot:s.slots)tasks.push_back(guardConnectTask(s,connectWorker(s,*slot),slot.get()));
    tasks.push_back(guardConnectTask(s,connectController(s)));
    CO2_AWAIT(co2::whenAll(std::move(tasks)));
} CO2_END
} // namespace detail
// RFC8305 core subset: parallel family lookup, 50ms resolution preference,
// interleaving and paced bounded attempts. No full RFC6724/NAT64 policy claims.
inline auto connect(Loop& loop,DnsResolver& resolver,std::string host,std::uint16_t port,ConnectOptions options={})
 CO2_BEG(co2::Task<Result<Tcp>>,(loop,resolver,host,port,options),
    detail::ConnectRaceState state{loop,resolver,std::move(options)};co2::stop_token token;Result<SocketAddress> numeric{Errc::invalid_argument};) {
    if(&resolver.loop()!=&loop || host.find('\0')!=std::string::npos || state.options.maxConcurrent==0 || state.options.maxConcurrent>64 ||
       state.options.minimumDelay<std::chrono::milliseconds{10} || state.options.attemptDelay<state.options.minimumDelay ||
       state.options.attemptDelay>std::chrono::milliseconds{2000} || (state.options.resolutionDelay.count()<0 || state.options.resolutionDelay>std::chrono::milliseconds{2000}) ||
       state.options.overallTimeout.count()<=0 || state.options.perAttemptTimeout.count()<=0)CO2_RETURN(Errc::invalid_argument);
    CO2_AWAIT_SET(token,co2::getStopToken());state.parent=token;
    if(token.stop_requested())CO2_RETURN(Errc::canceled);
    state.started=state.options.clock();
    numeric=SocketAddress::parse(host,port);
    if(numeric){state.numeric=true;state.dnsDone[0]=state.dnsDone[1]=true;state.family[numeric->isIpv6()?0:1].push_back(*numeric);}
    for(std::size_t i=0;i<state.options.maxConcurrent;++i)state.slots.emplace_back(new detail::ConnectSlot{loop});
    state.parentCancel.arm(loop,token,&detail::ConnectRaceState::canceled,&state);
    CO2_AWAIT((detail::ConnectGroupAwaiter{detail::connectGroup(state,std::move(host),port),state.stop.get_token()}));
    state.parentCancel.disarm();
    if(state.exception){CO2_AWAIT(state.winner.close());std::rethrow_exception(state.exception);}
    if(state.winner)CO2_RETURN(std::move(state.winner));
    CO2_RETURN(state.error);
} CO2_END
} // namespace uvco
