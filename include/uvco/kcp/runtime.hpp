#pragma once
#include "uvco/kcp/core.hpp"
#include "uvco/core/handle.hpp"
#include "uvco/core/cancel.hpp"
#include "uvco/core/wait_slot.hpp"
#include "uvco/udp.hpp"
#include "uvco/timer.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace uvco {
struct KcpSessionOptions {
    CoreOptions core;
    bool immediateFlushAfterInput = true;
    std::size_t highWater = 256, lowWater = 128, hardLimit = 512;
    std::size_t outputLimit = 256;
};
struct KcpEndpointOptions {
    std::size_t mtu = 1200, packetLimit = 2048, sessionLimit = 256;
};
class KcpSession;
namespace detail {
class KcpSessionBlock;
inline std::uint64_t kcpNow() noexcept { return uv_hrtime() / UINT64_C(1000000); }
inline bool kcpSamePeer(SocketAddress const& a, SocketAddress const& b) noexcept {
    if(a.family()!=b.family() || a.port()!=b.port()) return false;
    if(a.isIpv4()) {
        auto x=reinterpret_cast<sockaddr_in const*>(a.raw());
        auto y=reinterpret_cast<sockaddr_in const*>(b.raw());
        return x->sin_addr.s_addr==y->sin_addr.s_addr;
    }
    if(a.isIpv6()) {
        auto x=reinterpret_cast<sockaddr_in6 const*>(a.raw());
        auto y=reinterpret_cast<sockaddr_in6 const*>(b.raw());
        return x->sin6_scope_id==y->sin6_scope_id &&
            std::memcmp(&x->sin6_addr,&y->sin6_addr,sizeof(x->sin6_addr))==0;
    }
    return false;
}
class KcpEndpointBlock final : public HandleBlock {
  public:
    struct Packet {
        std::vector<char> data;
        std::size_t length=0;
        KcpSessionBlock* owner=nullptr;
        SocketAddress peer;
        bool queued=false;
        explicit Packet(std::size_t mtu):data(mtu){}
    };
    struct Completion {
        co2compat::FrameHeader header{&co2compat::noopDestroy,&step,false,false};
        KcpEndpointBlock* owner=nullptr;
        bool rx=false, deferred=false;
        static co2compat::FrameHeader* step(co2compat::FrameHeader* h) {
            auto* self=reinterpret_cast<Completion*>(h);
            if(!self->deferred) {
                self->deferred=true;
                self->owner->loop().schedule(co2::coroutine_handle<>::from_address(h));
            } else {
                self->deferred=false;
                self->owner->finishPump(self->rx);
            }
            return nullptr;
        }
        co2::coroutine_handle<> handle() noexcept {
            return co2::coroutine_handle<>::from_address(&header);
        }
    };
    KcpEndpointBlock(Loop& loop,Udp udp,KcpEndpointOptions options)
        :HandleBlock{loop},udp_{std::move(udp)},options_{options},receiveBuffer_(65507) {
        sessions_.reserve(options.sessionLimit);
        packets_.reserve(options.packetLimit);
        for(std::size_t i=0;i<options.packetLimit;++i)packets_.emplace_back(options.mtu);
        rxCompletion_.owner=txCompletion_.owner=this;
        rxCompletion_.rx=true;
    }
    int init() noexcept {
        int s=uv_async_init(loop().raw(),&anchor_,[](uv_async_t*){});
        if(s==0)bindHandle();
        return s;
    }
    void start() {
        rxTask_=receivePump(this);
        rxActive_=true;
        co2compat::TaskAccess::start(rxTask_,rxCompletion_.handle(),stop_.get_token());
    }
    Udp& udp() noexcept {return udp_;}
    KcpEndpointOptions const& options() const noexcept {return options_;}
    bool stopping() const noexcept {return stopping_;}
    bool hasCapacity() const noexcept {return sessions_.size()<options_.sessionLimit;}
    KcpSessionBlock* find(SocketAddress const&,std::uint32_t) noexcept;
    void add(KcpSessionBlock&);
    void remove(KcpSessionBlock&) noexcept;
    bool copyOutput(KcpSessionBlock&,ConstBuffer) noexcept;
    void kickTx() noexcept;
    void stop(Errc error=Errc::aborted) noexcept;
    void abandon() noexcept {static_cast<void>(release({}));}
  protected:
    uv_handle_t* uvHandle() noexcept override {return reinterpret_cast<uv_handle_t*>(&anchor_);}
    void abortPending() noexcept override {}
    void beginClose() noexcept override {stop();}
    void loopClosing() noexcept override {stop();}
  private:
    static auto receivePump(KcpEndpointBlock* self)
        CO2_BEG(co2::Task<>,(self),Result<Datagram> packet{Errc::canceled};) {
        while(!self->stopping_) {
            CO2_AWAIT_SET(packet,self->udp_.recv(buffer(self->receiveBuffer_)));
            if(!packet) {if(!self->stopping_)self->stop(packet.error());break;}
            self->route(*packet);
        }
        CO2_AWAIT(self->udp_.close());
        CO2_RETURN();
    } CO2_END
    static auto transmitPump(KcpEndpointBlock* self)
        CO2_BEG(co2::Task<>,(self),Packet* packet=nullptr;Result<void> result;) {
        while((packet=self->nextPacket())!=nullptr) {
            self->activePacket_=packet;
            CO2_AWAIT_SET(result,self->udp_.send(packet->peer,{packet->data.data(),packet->length}));
            self->completePacket(*packet,result);
            self->activePacket_=nullptr;
        }
        CO2_RETURN();
    } CO2_END
    void route(Datagram const&) noexcept;
    Packet* nextPacket() noexcept;
    void completePacket(Packet&,Result<void>) noexcept;
    void finishPump(bool rx) noexcept;
    void maybeClose() noexcept {
        if(stopping_&&!rxActive_&&!txActive_)closeInternally();
    }
    uv_async_t anchor_{};
    Udp udp_;
    KcpEndpointOptions options_;
    std::vector<char> receiveBuffer_;
    std::vector<KcpSessionBlock*> sessions_;
    std::vector<Packet> packets_;
    std::size_t cursor_=0;
    co2::stop_source stop_;
    co2::Task<> rxTask_,txTask_;
    Completion rxCompletion_,txCompletion_;
    Packet* activePacket_=nullptr;
    bool rxActive_=false,txActive_=false,stopping_=false;
};

class KcpSessionBlock final : public HandleBlock {
  public:
    KcpSessionBlock(Loop& loop,KcpEndpointBlock& endpoint,SocketAddress peer,
                    std::uint32_t conv,KcpSessionOptions options)
        :HandleBlock{loop},endpoint_{&endpoint},peer_{peer},conv_{conv},options_{options} {}
    Result<void> prepare() {
        auto core=KcpCore::create(conv_,options_.core,{this,&output});
        if(!core)return core.error();
        try {core_.reset(new KcpCore{std::move(*core)});}
        catch(std::bad_alloc const&){return Errc::no_memory;}
        if((core_->messageLimit()+core_->mtu()-25)/(core_->mtu()-24)>options_.hardLimit)
            return Errc::invalid_argument;
        return {};
    }
    int init() noexcept {
        int s=TimerBackend::init(loop().raw(),&timer_);
        if(s==0)bindHandle();
        return s;
    }
    SocketAddress const& peer() const noexcept {return peer_;}
    std::uint32_t conv() const noexcept {return conv_;}
    std::size_t packetCount() const noexcept {return packetCount_;}
    std::size_t outputLimit() const noexcept {return options_.outputLimit;}
    bool stopping() const noexcept {return stopping_;}
    void retainPacket() noexcept {holdBegin();++packetCount_;}
    void releasePacket() noexcept {
        --packetCount_;
        checkDrain();
        maybeClose();
        holdEnd(); // last access
    }
    void retainRegistration() noexcept {holdBegin();}
    void releaseRegistration() noexcept {holdEnd();}
    void drive(ConstBuffer const* packet=nullptr) noexcept {
        HandleRef<KcpSessionBlock> keep{this};
        if(stopping_)return;
        auto const now=kcpNow();
        if(lastUpdate_!=0&&(now<lastUpdate_ || now-lastUpdate_>=UINT64_C(0x80000000))) {
            stop(Errc::timed_out);return;
        }
        lastUpdate_=now;
        auto const now32=static_cast<std::uint32_t>(now);
        core_->update(now32);
        bool flushed=false;
        if(packet) {
            // Malformed datagrams are dropped before any protocol mutation.
            if(!KcpCore::validDatagram(*packet,conv_,core_->mtu(),core_->streamMode()))return;
            auto r=core_->input(*packet);
            if(!r){stop(r.error());return;}
            if(options_.immediateFlushAfterInput){core_->flush();flushed=true;}
        }
        fulfillReceive();
        fulfillRead();
        admitSender();
        if(stopping_)return;
        if(core_->deadLink()){stop(Errc::timed_out);return;}
        checkDrain();
        auto delay=KcpCore::delayUntil(now32,core_->check(now32));
        if(delay==0&&!flushed){core_->flush();delay=KcpCore::delayUntil(now32,core_->check(now32));}
        if(delay==0){++dueStreak_;delay=dueStreak_>=2?static_cast<std::uint32_t>(options_.core.interval):1;}
        else dueStreak_=0;
        uv_update_time(loop().raw());
        int const s=TimerBackend::start(&timer_,&onTimer,delay,0);
        if(s<0){stop(static_cast<Errc>(s));return;}
        endpoint_->kickTx(); // no syscall or scheduling from core output callback
    }
    bool beginReceive(co2::coroutine_handle<> waiter,co2::stop_token token,Errc& error) {
        if(!reserve(receive_,receiveCancel_,waiter,token,&cancelReceive,error))return false;
        receiveToken_=std::move(token);
        fulfillReceive();
        return true;
    }
    bool beginSend(co2::coroutine_handle<> waiter,co2::stop_token token,KcpMessage message,Errc& error) {
        if(!reserve(send_,sendCancel_,waiter,token,&cancelSend,error))return false;
        sendToken_=std::move(token);
        pendingMessage_=std::move(message);
        if(pendingMessage_.size()>core_->messageLimit())complete(send_,static_cast<Errc>(UV_EMSGSIZE));
        else drive();
        return true;
    }
    bool beginDrain(co2::coroutine_handle<> waiter,co2::stop_token token,Errc& error) {
        if(!reserve(drain_,drainCancel_,waiter,std::move(token),&cancelDrain,error))return false;
        checkDrain();return true;
    }
    bool beginRead(co2::coroutine_handle<> waiter,co2::stop_token token,MutableBuffer data,Errc& error) {
        if(!reserve(read_,readCancel_,waiter,token,&cancelRead,error))return false;
        readToken_=std::move(token);readBuffer_=data;
        if(data.size==0)complete(read_,Result<std::size_t>{std::size_t{0}});
        else if(!data.data)complete(read_,Errc::invalid_argument);
        else fulfillRead();
        return true;
    }
    std::size_t messageLimit() const noexcept {return core_->messageLimit();}
    Result<std::size_t> takeRead() noexcept {readBuffer_={};return take(read_,readCancel_);}
    Result<KcpMessage> takeReceive() noexcept {return take(receive_,receiveCancel_);}
    Result<void> takeSend() noexcept {return take(send_,sendCancel_);}
    Result<void> takeDrain() noexcept {return take(drain_,drainCancel_);}
    void stop(Errc reason=Errc::aborted) noexcept {
        if(stopping_)return;
        stopping_=true;reason_=reason;
        static_cast<void>(TimerBackend::stop(&timer_));
        if(send_.pending())complete(send_,reason);
        if(receive_.pending())complete(receive_,reason);
        if(read_.pending())complete(read_,reason);
        if(drain_.pending())complete(drain_,reason);
        pendingMessage_.clear();
        endpoint_->remove(*this);
        maybeClose();
    }
  protected:
    uv_handle_t* uvHandle() noexcept override {return reinterpret_cast<uv_handle_t*>(&timer_);}
    void abortPending() noexcept override {}
    void beginClose() noexcept override {stop();}
    void loopClosing() noexcept override {stop();}
  private:
    template<class T> bool reserve(WaitSlot<T>& slot,CancelHook& cancel,
                    co2::coroutine_handle<> waiter,co2::stop_token token,
                    CancelHook::Fn fn,Errc& error) {
        if(stopping_){error=reason_;return false;}
        if(token.stop_requested()){error=Errc::canceled;return false;}
        if(!slot.idle()){error=Errc::busy;return false;}
        slot.begin();slot.pend(waiter);opBegin();
        cancel.arm(loop(),std::move(token),fn,this);
        return true;
    }
    template<class T> void complete(WaitSlot<T>& slot,Result<T> result) noexcept {
        resumeLater(slot.complete(std::move(result)));
    }
    template<class T> void complete(WaitSlot<T>& slot,Errc error) noexcept {complete(slot,Result<T>{error});}
    template<class T> Result<T> take(WaitSlot<T>& slot,CancelHook& hook) noexcept {
        hook.disarm();auto result=slot.take();opEnd();return std::move(result.result);
    }
    void fulfillReceive() noexcept {
        if(!receive_.pending())return;
        if(receiveToken_.stop_requested()){complete(receive_,Errc::canceled);return;}
        auto message=core_->recvMessage();
        if(message||message.error()!=Errc::again)complete(receive_,std::move(message));
    }
    void fulfillRead() noexcept {
        if(!read_.pending())return;
        if(readToken_.stop_requested()){complete(read_,Errc::canceled);return;}
        unsigned budget=128;
        while(leftoverOffset_==leftover_.size()&&budget--!=0) {
            auto unit=core_->recvStreamUnit();
            if(!unit){if(unit.error()!=Errc::again)complete(read_,unit.error());return;}
            leftover_=std::move(*unit);leftoverOffset_=0;
        }
        if(leftoverOffset_==leftover_.size())return;
        auto n=(std::min)(readBuffer_.size,leftover_.size()-leftoverOffset_);
        std::memcpy(readBuffer_.data,leftover_.data()+leftoverOffset_,n);leftoverOffset_+=n;
        complete(read_,Result<std::size_t>{n});
    }
    void admitSender() noexcept {
        if(!send_.pending())return;
        if(sendToken_.stop_requested()){
            pendingMessage_.clear();complete(send_,Errc::canceled);return;
        }
        auto waiting=core_->waitingSegments();
        if(backpressured_&&waiting>options_.lowWater)return;
        backpressured_=false;
        auto const count=(std::max)(std::size_t{1},(pendingMessage_.size()+core_->mtu()-25)/(core_->mtu()-24));
        if(waiting>=options_.highWater || waiting+count>options_.hardLimit){backpressured_=true;return;}
        auto sent=core_->streamMode()?core_->write(buffer(pendingMessage_)):core_->sendMessage(buffer(pendingMessage_));
        pendingMessage_.clear();
        if(!sent){auto error=sent.error();complete(send_,error);stop(error);return;}
        complete(send_,Result<void>{});
        core_->flush();
    }
    void checkDrain() noexcept {
        if(drain_.pending()&&core_->waitingSegments()==0&&packetCount_==0)complete(drain_,Result<void>{});
    }
    void maybeClose() noexcept {if(stopping_&&packetCount_==0)closeInternally();}
    static bool output(void* self,ConstBuffer data) noexcept {
        auto& s=*static_cast<KcpSessionBlock*>(self);
        return !s.stopping_&&s.endpoint_->copyOutput(s,data);
    }
    static void onTimer(uv_timer_t* timer) noexcept {fromData<KcpSessionBlock>(timer->data).drive();}
    static void cancelReceive(void* p) {
        auto& s=*static_cast<KcpSessionBlock*>(p);
        if(s.receive_.pending())s.complete(s.receive_,Errc::canceled);
    }
    static void cancelRead(void* p) {
        auto& s=*static_cast<KcpSessionBlock*>(p);
        if(s.read_.pending())s.complete(s.read_,Errc::canceled);
    }
    static void cancelSend(void* p) {
        auto& s=*static_cast<KcpSessionBlock*>(p);
        if(s.send_.pending()){s.pendingMessage_.clear();s.complete(s.send_,Errc::canceled);}
    }
    static void cancelDrain(void* p) {
        auto& s=*static_cast<KcpSessionBlock*>(p);
        if(s.drain_.pending())s.complete(s.drain_,Errc::canceled);
    }
    uv_timer_t timer_{};
    HandleRef<KcpEndpointBlock> endpoint_;
    SocketAddress peer_;
    std::uint32_t conv_;
    KcpSessionOptions options_;
    std::unique_ptr<KcpCore> core_;
    WaitSlot<KcpMessage> receive_;
    WaitSlot<void> send_,drain_;
    WaitSlot<std::size_t> read_;
    CancelHook readCancel_;
    co2::stop_token readToken_;
    MutableBuffer readBuffer_;
    KcpMessage leftover_;
    std::size_t leftoverOffset_=0;
    CancelHook receiveCancel_,sendCancel_,drainCancel_;
    co2::stop_token receiveToken_,sendToken_;
    KcpMessage pendingMessage_;
    std::uint64_t lastUpdate_=0;
    std::size_t packetCount_=0;
    unsigned dueStreak_=0;
    Errc reason_=Errc::aborted;
    bool stopping_=false,backpressured_=false;
};
inline KcpSessionBlock* KcpEndpointBlock::find(SocketAddress const& peer,std::uint32_t conv) noexcept {
    for(auto* s:sessions_)if(s->conv()==conv&&kcpSamePeer(s->peer(),peer))return s;
    return nullptr;
}
inline void KcpEndpointBlock::add(KcpSessionBlock& s) {
    s.retainRegistration();sessions_.push_back(&s);
}
inline void KcpEndpointBlock::remove(KcpSessionBlock& s) noexcept {
    for(auto& p:packets_)if(p.owner==&s&&p.queued){p.owner=nullptr;p.queued=false;s.releasePacket();}
    auto found=std::find(sessions_.begin(),sessions_.end(),&s);
    if(found!=sessions_.end()){sessions_.erase(found);s.releaseRegistration();}
}
inline bool KcpEndpointBlock::copyOutput(KcpSessionBlock& s,ConstBuffer data) noexcept {
    if(stopping_||data.size>options_.mtu||s.packetCount()>=s.outputLimit())return false;
    for(auto& p:packets_)if(!p.owner){
        std::memcpy(p.data.data(),data.data,data.size);p.length=data.size;p.peer=s.peer();
        p.owner=&s;p.queued=true;s.retainPacket();return true;
    }
    return false;
}
inline void KcpEndpointBlock::route(Datagram const& packet) noexcept {
    if(stopping_||packet.truncated||packet.size<24)return;
    auto* s=find(packet.sender,KcpCore::decode32(receiveBuffer_.data()));
    if(s){ConstBuffer data{receiveBuffer_.data(),packet.size};s->drive(&data);}
    // Unknown peer/conv is never auto-created. Explicit bounded accept is added separately.
}
inline KcpEndpointBlock::Packet* KcpEndpointBlock::nextPacket() noexcept {
    if(stopping_||sessions_.empty())return nullptr;
    for(std::size_t n=0;n<sessions_.size();++n) {
        cursor_=(cursor_+1)%sessions_.size();auto* owner=sessions_[cursor_];
        for(auto& p:packets_)if(p.queued&&p.owner==owner){p.queued=false;return &p;}
    }
    return nullptr;
}
inline void KcpEndpointBlock::completePacket(Packet& packet,Result<void> result) noexcept {
    auto* owner=packet.owner;packet.owner=nullptr;packet.queued=false;
    if(!result)owner->stop(result.error());
    owner->releasePacket();
}
inline void KcpEndpointBlock::kickTx() noexcept {
    if(stopping_||txActive_)return;
    bool any=false;for(auto const& p:packets_)if(p.queued){any=true;break;}
    if(!any)return;
    try {
        txTask_=transmitPump(this);txActive_=true;
        co2compat::TaskAccess::start(txTask_,txCompletion_.handle(),{});
    }catch(std::bad_alloc const&){txActive_=false;stop(Errc::no_memory);}
}
inline void KcpEndpointBlock::finishPump(bool rx) noexcept {
    Errc failure=Errc::aborted;bool failed=false;
    try {if(rx)co2compat::TaskAccess::takeResult(rxTask_);else co2compat::TaskAccess::takeResult(txTask_);}
    catch(std::bad_alloc const&){failed=true;failure=Errc::no_memory;}
    catch(...){failed=true;failure=static_cast<Errc>(UV_EPROTO);}
    if(rx){rxTask_=co2::Task<>{};rxActive_=false;}
    else {
        txTask_=co2::Task<>{};txActive_=false;
        if(activePacket_){completePacket(*activePacket_,failure);activePacket_=nullptr;}
    }
    if(failed)stop(failure);
    if(!stopping_)kickTx();
    maybeClose();
}
inline void KcpEndpointBlock::stop(Errc error) noexcept {
    if(stopping_)return;
    stopping_=true;
    while(!sessions_.empty())sessions_.back()->stop(error);
    stop_.request_stop();
    if(!rxActive_){auto close=udp_.close();static_cast<void>(close);}
    maybeClose();
}

class KcpReceiveAwaiter {
  public:
    explicit KcpReceiveAwaiter(KcpSessionBlock& s) noexcept:session_{&s}{}
    bool await_ready() const noexcept{return false;}
    template<class P>bool await_suspend(co2::coroutine_handle<P> h){
        owned_=session_->beginReceive(h,co2compat::stopTokenOf(h),error_);return owned_;
    }
    Result<KcpMessage> await_resume() noexcept {
        if(!owned_)return error_;
        return session_->takeReceive();
    }
  private:
    HandleRef<KcpSessionBlock> session_;
    Errc error_=Errc::aborted;bool owned_=false;
};
class KcpSendAwaiter {
  public:
    KcpSendAwaiter(KcpSessionBlock& s,KcpMessage data) noexcept:session_{&s},data_{std::move(data)}{}
    KcpSendAwaiter(KcpSendAwaiter&&) noexcept=default;
    KcpSendAwaiter(KcpSendAwaiter const&)=delete;
    bool await_ready() const noexcept{return false;}
    template<class P>bool await_suspend(co2::coroutine_handle<P> h){
        owned_=session_->beginSend(h,co2compat::stopTokenOf(h),std::move(data_),error_);return owned_;
    }
    Result<void> await_resume() noexcept {if(!owned_)return error_;
        return session_->takeSend();}
  private:
    HandleRef<KcpSessionBlock> session_;
    KcpMessage data_;
    Errc error_=Errc::aborted;bool owned_=false;
};
class KcpDrainAwaiter {
  public:
    explicit KcpDrainAwaiter(KcpSessionBlock& s) noexcept:session_{&s}{}
    bool await_ready() const noexcept{return false;}
    template<class P>bool await_suspend(co2::coroutine_handle<P> h){
        owned_=session_->beginDrain(h,co2compat::stopTokenOf(h),error_);return owned_;
    }
    Result<void> await_resume() noexcept {if(!owned_)return error_;
        return session_->takeDrain();}
  private:
    HandleRef<KcpSessionBlock> session_;
    Errc error_=Errc::aborted;bool owned_=false;
};
class KcpReadAwaiter {
  public:
    KcpReadAwaiter(KcpSessionBlock& s,MutableBuffer b) noexcept:session_{&s},buffer_{b}{}
    bool await_ready() const noexcept{return false;}
    template<class P>bool await_suspend(co2::coroutine_handle<P> h){
        owned_=session_->beginRead(h,co2compat::stopTokenOf(h),buffer_,error_);return owned_;
    }
    Result<std::size_t> await_resume() noexcept {if(!owned_)return error_;
        return session_->takeRead();}
  private:
    HandleRef<KcpSessionBlock> session_;
    MutableBuffer buffer_;
    Errc error_=Errc::aborted;bool owned_=false;
};
class KcpWriteAwaiter {
  public:
    KcpWriteAwaiter(KcpSessionBlock& s,ConstBuffer b) noexcept:session_{&s},buffer_{b}{}
    bool await_ready() const noexcept{return false;}
    template<class P>bool await_suspend(co2::coroutine_handle<P> h){
        auto token=co2compat::stopTokenOf(h);
        if(token.stop_requested()){error_=Errc::canceled;return false;}
        if(buffer_.size&&!buffer_.data){error_=Errc::invalid_argument;return false;}
        size_=(std::min)(buffer_.size,session_->messageLimit());
        KcpMessage data;
        try {if(size_)data.assign(buffer_.data,buffer_.data+size_);}
        catch(std::bad_alloc const&){error_=Errc::no_memory;return false;}
        owned_=session_->beginSend(h,std::move(token),std::move(data),error_);return owned_;
    }
    Result<std::size_t> await_resume() noexcept {
        if(!owned_)return error_;
        auto result=session_->takeSend();if(!result)return result.error();
        return size_;
    }
  private:
    HandleRef<KcpSessionBlock> session_;
    ConstBuffer buffer_;
    std::size_t size_=0;
    Errc error_=Errc::aborted;bool owned_=false;
};
} // namespace detail

class KcpSession:public detail::Handle<detail::KcpSessionBlock> {
    friend class KcpEndpoint;
    explicit KcpSession(detail::KcpSessionBlock* block) noexcept:Handle{block}{}
  public:
    detail::KcpSendAwaiter sendMessage(KcpMessage message){return {block(),std::move(message)};}
    detail::KcpReceiveAwaiter recvMessage() noexcept{return detail::KcpReceiveAwaiter{block()};}
    detail::KcpDrainAwaiter drain() noexcept{return detail::KcpDrainAwaiter{block()};}
    void abort(Errc reason=Errc::aborted) noexcept{block().stop(reason);}
};
class KcpStream:public detail::Handle<detail::KcpSessionBlock> {
    friend class KcpEndpoint;
    explicit KcpStream(detail::KcpSessionBlock* block) noexcept:Handle{block}{}
  public:
    detail::KcpReadAwaiter readSome(MutableBuffer buffer) noexcept{return {block(),buffer};}
    detail::KcpWriteAwaiter write(ConstBuffer buffer) noexcept{return {block(),buffer};}
    detail::KcpDrainAwaiter drain() noexcept{return detail::KcpDrainAwaiter{block()};}
    void abort(Errc reason=Errc::aborted) noexcept{block().stop(reason);}
};
class KcpEndpoint:public detail::Handle<detail::KcpEndpointBlock> {
    explicit KcpEndpoint(detail::KcpEndpointBlock* block) noexcept:Handle{block}{}
  public:
    static Result<KcpEndpoint> create(Loop& loop,SocketAddress bind,KcpEndpointOptions options={}) {
        if(options.mtu<50||options.mtu>65507||options.packetLimit==0||options.packetLimit>65536||
            options.sessionLimit==0||options.sessionLimit>4096)return Errc::invalid_argument;
        auto udp=Udp::create(loop);if(!udp)return udp.error();
        auto bound=udp->bind(bind);if(!bound)return bound.error();
        std::unique_ptr<detail::KcpEndpointBlock> block;
        try{block.reset(new detail::KcpEndpointBlock{loop,std::move(*udp),options});}
        catch(std::bad_alloc const&){return Errc::no_memory;}
        int s=block->init();if(s<0)return static_cast<Errc>(s);
        auto* raw=block.release();
        try{raw->start();}catch(std::bad_alloc const&){raw->abandon();return Errc::no_memory;}
        return KcpEndpoint{raw};
    }
    Result<SocketAddress> localAddress() const noexcept{return block().udp().localAddress();}
    Result<KcpSession> openSession(SocketAddress peer,std::uint32_t conv,KcpSessionOptions options={}) {
        if(options.core.stream)return Errc::invalid_argument;
        auto result=openBlock(peer,conv,options);
        if(!result)return result.error();
        return KcpSession{*result};
    }
    Result<KcpStream> openStream(SocketAddress peer,std::uint32_t conv,KcpSessionOptions options={}) {
        options.core.stream=true;
        auto result=openBlock(peer,conv,options);
        if(!result)return result.error();
        return KcpStream{*result};
    }
  private:
    Result<detail::KcpSessionBlock*> openBlock(SocketAddress peer,std::uint32_t conv,KcpSessionOptions options) {
        auto& endpoint=block();
        if(endpoint.stopping())return Errc::aborted;
        if((!peer.isIpv4()&&!peer.isIpv6())||peer.port()==0||
            options.core.mtu>endpoint.options().mtu||options.lowWater>=options.highWater||
            options.highWater>options.hardLimit||options.outputLimit==0)return Errc::invalid_argument;
        if(endpoint.find(peer,conv))return Errc::address_in_use;
        if(!endpoint.hasCapacity())return Errc::busy;
        std::unique_ptr<detail::KcpSessionBlock> session;
        try {session.reset(new detail::KcpSessionBlock{loop(),endpoint,peer,conv,options});}
        catch(std::bad_alloc const&){return Errc::no_memory;}
        auto ready=session->prepare();if(!ready)return ready.error();
        int s=session->init();if(s<0)return static_cast<Errc>(s);
        auto* raw=session.release();endpoint.add(*raw);raw->drive();return raw;
    }
};
} // namespace uvco
