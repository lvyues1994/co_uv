#pragma once
#include "uvco/core/req_awaiter.hpp"
namespace uvco {
// Two check visits cross at least one full iteration from every phase. An
// idle driver prevents a check-only loop from blocking in poll. Both embedded
// handles close before completion; no timer or extra heap state is required
// for a frame-local lvalue operation. scheduleOn retains its existing policy.
class YieldNextTurn final : public detail::ReqAwaiter<YieldNextTurn,uv_check_t,void>,private detail::LoopResident {
    using Base=detail::ReqAwaiter<YieldNextTurn,uv_check_t,void>;
  public:
    explicit YieldNextTurn(Loop& loop):Base{loop}{}
    YieldNextTurn(YieldNextTurn&& other) noexcept:Base{std::move(other)}{}
    int start(uv_loop_t* l) noexcept {
        UVCO_CONTRACT_CHECK(uv_check_init(l,&req_)==0);
        UVCO_CONTRACT_CHECK(uv_idle_init(l,&idle_)==0);
        req_.data=static_cast<Base*>(this);idle_.data=static_cast<Base*>(this);
        visits_=0;closing_=false;remaining_=2;status_=0;linkTo(loop());
        UVCO_CONTRACT_CHECK(uv_idle_start(&idle_,[](uv_idle_t*){})==0);
        UVCO_CONTRACT_CHECK(uv_check_start(&req_,&onCheck)==0);return 0;
    }
    void cancel() noexcept {closeWith(UV_ECANCELED);}
    Result<void> finish(int status) noexcept{return fromStatus(status);}
  private:
    void loopClosing() noexcept override {closeWith(UVCO_EABORTED);}
    void loopGone() noexcept override {UVCO_CONTRACT_FAIL("pending yield survived Loop teardown");}
    static YieldNextTurn& self(void* p) noexcept{return static_cast<YieldNextTurn&>(*static_cast<Base*>(p));}
    static void onCheck(uv_check_t* h) noexcept {auto& op=self(h->data);if(++op.visits_==2)op.closeWith(0);}
    void closeWith(int status) noexcept {
        if(closing_)return;
        closing_=true;status_=status;
        uv_close(reinterpret_cast<uv_handle_t*>(&req_),&onClosed);
        uv_close(reinterpret_cast<uv_handle_t*>(&idle_),&onClosed);
    }
    static void onClosed(uv_handle_t* h) noexcept {
        auto& op=self(h->data);if(--op.remaining_!=0)return;
        int const status=op.status_;op.unlinkFrom(op.loop());Base::complete(&op.req_,status);
    }
    uv_idle_t idle_{};unsigned visits_=0,remaining_=0;int status_=0;bool closing_=false;
};
UVCO_NODISCARD inline YieldNextTurn yieldNextTurn(Loop& loop){return YieldNextTurn{loop};}
} // namespace uvco
