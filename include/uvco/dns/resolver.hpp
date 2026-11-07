#pragma once
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include <co2/co2.hpp>
#include "uvco/dns/dns.hpp"
#include "uvco/core/handle.hpp"
namespace uvco {
using AddressList = std::shared_ptr<std::vector<SocketAddress> const>;
struct DnsCacheOptions {
    bool enabled = false;
    std::size_t maxEntries = 256, maxBytes = 4 * 1024 * 1024;
    std::size_t maxInFlight = 32, maxSubscribers = 256;
    std::chrono::milliseconds maxAge{30000}; // local age, NOT authoritative DNS TTL
};
using DnsQuery = std::function<co2::Task<Result<std::vector<SocketAddress>>>(Loop&,std::string,std::string,addrinfo)>;
using DnsClock = std::function<std::chrono::steady_clock::time_point()>;
namespace detail {
inline auto resolverQuery(Loop& loop,std::string host,std::string service,addrinfo hints)
 CO2_BEG(co2::Task<Result<std::vector<SocketAddress>>>,(loop,host,service,hints),Result<std::vector<SocketAddress>> r;) {
    CO2_AWAIT_SET(r,getaddrinfo(loop,std::move(host),std::move(service),hints)); CO2_RETURN(std::move(r));
} CO2_END
struct DnsKey {
    std::string host,service;
    int family=0,socktype=0,protocol=0,flags=0;
    std::uint64_t epoch=0;
    bool operator==(DnsKey const& k) const noexcept {
        return host==k.host && service==k.service && family==k.family && socktype==k.socktype &&
            protocol==k.protocol && flags==k.flags && epoch==k.epoch;
    }
    addrinfo hints() const noexcept {addrinfo h{};h.ai_family=family;h.ai_socktype=socktype;h.ai_protocol=protocol;h.ai_flags=flags;return h;}
};
class ResolverBlock;
struct DnsFlight;
struct DnsSubscriber {
    DnsSubscriber* previous=nullptr;
    DnsSubscriber* next=nullptr;
    DnsFlight* flight=nullptr;
    ResolverBlock* owner=nullptr;
    co2::coroutine_handle<> waiter;
    CancelHook cancel;
    Result<AddressList> result{Errc::canceled};
    std::exception_ptr exception;
    bool suspended=false,pending=false;
};
struct DnsFlight {
    struct Completion {
        co2compat::FrameHeader header{&co2compat::noopDestroy,&step,false,false};
        DnsFlight* flight=nullptr;
        bool queued=false;
        static co2compat::FrameHeader* step(co2compat::FrameHeader* h);
        co2::coroutine_handle<> handle() noexcept {return co2::coroutine_handle<>::from_address(&header);}
    } completion;
    ResolverBlock* owner;
    DnsKey key;
    co2::Task<Result<std::vector<SocketAddress>>> task;
    co2::stop_source stop;
    DnsSubscriber* subscribers=nullptr;
    bool joinable=true;
    DnsFlight(ResolverBlock* b,DnsKey const& k,co2::Task<Result<std::vector<SocketAddress>>> t)
        :owner{b},key{k},task{std::move(t)} {completion.flight=this;}
};
// A native async reference, not Loop::KeepAlive, owns producer progress. ~Loop
// can therefore call loopClosing before requiring producers to have drained.
class ResolverBlock final : public HandleBlock {
    struct Entry {DnsKey key;AddressList addresses;std::chrono::steady_clock::time_point created;std::size_t bytes;};
  public:
    ResolverBlock(Loop& l,DnsCacheOptions options,DnsQuery query,DnsClock clock)
        :HandleBlock{l},options_{options},query_{std::move(query)},clock_{std::move(clock)} {}
    ~ResolverBlock() override = default;
    int init() noexcept {
        int status=uv_async_init(loop().raw(),&async_,[](uv_async_t*){});
        if(status==0){bindHandle();uv_unref(uvHandle());}return status;
    }
    std::uint64_t epoch() const noexcept {return epoch_;}
    std::size_t cacheSize() const noexcept {return cache_.size();}
    void invalidate() noexcept {
        cache_.clear();bytes_=0;++epoch_;
        for(auto& f:flights_)f->joinable=false; // also safe across epoch wrap
    }
    bool subscribe(DnsSubscriber& node,DnsKey const& key,co2::coroutine_handle<> waiter,co2::stop_token token) {
        UVCO_CONTRACT_CHECK(loop().isLoopThread());node.owner=this;
        if(stopping_){node.result=Errc::aborted;return false;}
        if(token.stop_requested()){node.result=Errc::canceled;return false;}
        try {
            if(options_.enabled) {
                auto now=clock_();
                if(stopping_){node.result=Errc::aborted;return false;}
                for(auto i=cache_.begin();i!=cache_.end();) {
                    if(now>=i->created && std::chrono::duration_cast<std::chrono::milliseconds>(now-i->created)>=options_.maxAge){bytes_-=i->bytes;i=cache_.erase(i);continue;}
                    if(i->key==key){node.result=i->addresses;cache_.splice(cache_.begin(),cache_,i);return false;}++i;
                }
            }
            if(subscribers_>=options_.maxSubscribers){node.result=Errc::busy;return false;}
            DnsFlight* found=nullptr;
            for(auto& f:flights_)if(f->joinable && f->key==key){found=f.get();break;}
            bool launch=false;
            if(!found) {
                if(flights_.size()>=options_.maxInFlight){node.result=Errc::busy;return false;}
                auto task=query_(loop(),key.host,key.service,key.hints());
                // Owned injection points may synchronously close/invalidate the
                // resolver. Never start work after its native keepalive closed.
                if(stopping_){node.result=Errc::aborted;return false;}
                if(!task){node.result=Errc::invalid_argument;return false;}
                auto flight=std::unique_ptr<DnsFlight>{new DnsFlight{this,key,std::move(task)}};
                found=flight.get();flights_.push_back(std::move(flight));launch=true;
            }
            node.flight=found;node.next=found->subscribers;
            if(node.next)node.next->previous=&node;
            found->subscribers=&node;node.pending=true;node.suspended=true;node.waiter=waiter;
            ++subscribers_;opBegin();uv_ref(uvHandle());
            node.cancel.arm(loop(),std::move(token),&ResolverBlock::cancelSubscriber,&node);
            if(launch)co2compat::TaskAccess::start(found->task,found->completion.handle(),found->stop.get_token());
            return true;
        }catch(std::bad_alloc const&){node.result=Errc::no_memory;return false;}
        catch(...){node.exception=std::current_exception();return false;}
    }
    Result<AddressList> resume(DnsSubscriber& node) {
        node.cancel.disarm();auto result=std::move(node.result);auto exception=std::move(node.exception);
        bool const suspended=node.suspended;node.suspended=false;
        if(suspended)opEnd(); // no this access after release
        if(exception)std::rethrow_exception(exception);
        return result;
    }
    void finish(DnsFlight& flight) noexcept {
        Result<AddressList> result=Errc::no_memory;std::exception_ptr exception;
        try {
            auto addresses=co2compat::TaskAccess::takeResult(flight.task);
            if(!addresses)result=addresses.error();
            else result=std::make_shared<std::vector<SocketAddress> const>(std::move(*addresses));
        }catch(std::bad_alloc const&){}catch(...){exception=std::current_exception();}
        if(result && !exception && flight.joinable && flight.key.epoch==epoch_ && !stopping_)cacheResult(flight.key,*result);
        while(flight.subscribers)complete(*flight.subscribers,stopping_?Result<AddressList>{Errc::aborted}:result,exception);
        for(auto i=flights_.begin();i!=flights_.end();++i)if(i->get()==&flight){flights_.erase(i);break;}
        if(flights_.empty()) {
            if(stopping_)closeInternally();
            else uv_unref(uvHandle());
        }
    }
  private:
    uv_handle_t* uvHandle() noexcept override {return reinterpret_cast<uv_handle_t*>(&async_);}
    void abortPending() noexcept override {} // subscribers aborted before producer drain
    void beginClose() noexcept override {
        if(stopping_)return;
        stopping_=true;invalidate();
        for(auto& f:flights_) {
            while(f->subscribers)complete(*f->subscribers,Errc::aborted,{});
            f->stop.request_stop();
        }
        if(flights_.empty())closeInternally();
    }
    void loopClosing() noexcept override {beginClose();}
    static void unlink(DnsSubscriber& n) noexcept {
        auto* f=n.flight;
        if(n.previous)n.previous->next=n.next;else f->subscribers=n.next;
        if(n.next)n.next->previous=n.previous;
        n.previous=n.next=nullptr;n.flight=nullptr;n.pending=false;
    }
    void complete(DnsSubscriber& n,Result<AddressList> result,std::exception_ptr exception) noexcept {
        unlink(n);--subscribers_;n.result=std::move(result);n.exception=std::move(exception);
        loop().schedule(n.waiter); // never resume subscriber inside producer/start/close stack
    }
    static void cancelSubscriber(void* p) noexcept {
        auto& n=*static_cast<DnsSubscriber*>(p);if(!n.pending)return;
        auto* f=n.flight;auto& owner=*n.owner;
        owner.complete(n,Errc::canceled,{});
        if(!f->subscribers){f->joinable=false;f->stop.request_stop();}
    }
    void cacheResult(DnsKey const& key,AddressList addresses) noexcept {
        if(!options_.enabled || options_.maxEntries==0 || options_.maxAge.count()<=0)return;
        std::size_t remaining=options_.maxBytes;
        for(std::size_t n:{sizeof(Entry),key.host.size(),key.service.size()}){if(n>remaining)return;remaining-=n;}
        if(addresses->size()>remaining/sizeof(SocketAddress))return;
        std::size_t cost=options_.maxBytes-remaining+addresses->size()*sizeof(SocketAddress);
        try {
            auto created=clock_();
            if(stopping_ || key.epoch!=epoch_)return;
            // Construct first: allocation failure must not evict otherwise valid cache.
            std::list<Entry> entry;entry.push_front(Entry{key,std::move(addresses),created,cost});
            while(!cache_.empty() && (cache_.size()>=options_.maxEntries || cost>options_.maxBytes-bytes_)){
                bytes_-=cache_.back().bytes;cache_.pop_back();
            }
            cache_.splice(cache_.begin(),entry);bytes_+=cost;
        }catch(...){/* Optional cache insertion cannot fail successful DNS delivery. */}
    }
    uv_async_t async_{};
    DnsCacheOptions options_;
    DnsQuery query_;DnsClock clock_;
    std::list<std::unique_ptr<DnsFlight>> flights_;
    std::list<Entry> cache_;
    std::size_t subscribers_=0,bytes_=0;
    std::uint64_t epoch_=0;
    bool stopping_=false;
};
inline co2compat::FrameHeader* DnsFlight::Completion::step(co2compat::FrameHeader* h) {
    auto& self=*reinterpret_cast<Completion*>(h);
    if(!self.queued){self.queued=true;self.flight->owner->loop().schedule(self.handle());}
    else self.flight->owner->finish(*self.flight); // destroys completion; no access after
    return nullptr;
}
class ResolveAwaiter {
  public:
    ResolveAwaiter(ResolverBlock& b,std::string host,std::string service,addrinfo const& hints)
        :block_{&b},key_{std::move(host),std::move(service),hints.ai_family,hints.ai_socktype,hints.ai_protocol,hints.ai_flags,b.epoch()} {}
    ResolveAwaiter(ResolveAwaiter&& other) noexcept :block_{std::move(other.block_)},key_{std::move(other.key_)} {UVCO_CONTRACT_CHECK(!other.started_);}
    ResolveAwaiter(ResolveAwaiter const&)=delete;
    bool await_ready() const noexcept {return false;}
    template<class P> bool await_suspend(co2::coroutine_handle<P> h) {
        UVCO_CONTRACT_CHECK(!started_);started_=true;
        return block_->subscribe(node_,key_,h,co2compat::stopTokenOf(h));
    }
    Result<AddressList> await_resume(){return block_->resume(node_);}
  private:
    HandleRef<ResolverBlock> block_;DnsKey key_;DnsSubscriber node_;bool started_=false;
};
} // namespace detail
class DnsResolver {
  public:
    explicit DnsResolver(Loop& loop,DnsCacheOptions options={},DnsQuery query={},DnsClock clock={}) {
        if(!query)query=detail::resolverQuery;
        if(!clock)clock=[] {return std::chrono::steady_clock::now();};
        auto block=std::unique_ptr<detail::ResolverBlock>{new detail::ResolverBlock{loop,options,std::move(query),std::move(clock)}};
        int status=block->init();if(status<0)throw std::system_error{make_error_code(static_cast<Errc>(status))};
        block_=block.release();
    }

    static Result<DnsResolver> create(Loop& loop,DnsCacheOptions options={},DnsQuery query={},DnsClock clock={}) {
        try { return DnsResolver{loop,options,std::move(query),std::move(clock)}; }
        catch(std::bad_alloc const&) { return Errc::no_memory; }
        catch(std::system_error const& e) {
            if(e.code().category()!=uv_category())throw;
            return static_cast<Errc>(e.code().value());
        }
    }
    DnsResolver(DnsResolver const&)=delete;
    DnsResolver& operator=(DnsResolver const&)=delete;
    DnsResolver(DnsResolver&& other) noexcept :block_{std::exchange(other.block_,nullptr)} {}
    ~DnsResolver(){if(block_)static_cast<void>(block_->release({}));}
    Loop& loop() const noexcept {UVCO_CONTRACT_CHECK(block_);return block_->loop();}
    UVCO_NODISCARD detail::ResolveAwaiter resolve(std::string host,std::string service={},addrinfo const& hints={}) {
        static_cast<void>(loop());return detail::ResolveAwaiter{*block_,std::move(host),std::move(service),hints};
    }
    void invalidate() noexcept {static_cast<void>(loop());block_->invalidate();}
    std::size_t cacheSize() const noexcept {static_cast<void>(loop());return block_->cacheSize();}
    UVCO_NODISCARD detail::CloseAwaiter close() noexcept {return detail::CloseAwaiter{std::exchange(block_,nullptr)};}
  private:
    detail::ResolverBlock* block_=nullptr;
};
} // namespace uvco
