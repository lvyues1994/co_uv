#pragma once
#include <exception>
#include <type_traits>
#include <utility>
#include "uvco/core/req_awaiter.hpp"

namespace uvco { namespace detail {
template<class F> auto invokeWork(F& fn,co2::stop_token token,int) -> decltype(fn(std::move(token))) {
    return fn(std::move(token));
}
template<class F> auto invokeWork(F& fn,co2::stop_token,long) -> decltype(fn()) {return fn();}
template<class F> using WorkValue=typename std::decay<decltype(invokeWork(std::declval<F&>(),co2::stop_token{},0))>::type;
template<class R> struct WorkResult {
    template<class F> static void run(F& fn,co2::stop_token token,co2compat::ResultStorage<R>& out) {
        out.emplace(invokeWork(fn,std::move(token),0));
    }
    static Result<R> take(co2compat::ResultStorage<R>& out) {return Result<R>{out.take()};}
};
template<> struct WorkResult<void> {
    template<class F> static void run(F& fn,co2::stop_token token,co2compat::ResultStorage<void>& out) {
        invokeWork(fn,std::move(token),0);out.emplace();
    }
    static Result<void> take(co2compat::ResultStorage<void>& out) {out.take();return {};}
};
template<class F> class Work final : public ReqAwaiter<Work<F>,uv_work_t,WorkValue<F>> {
    using R=WorkValue<F>;
    using Base=ReqAwaiter<Work<F>,uv_work_t,R>;
  public:
    Work(Loop& loop,F fn):Base{loop},fn_{std::move(fn)}{}
    Work(Work&&)=default;
    int start(uv_loop_t* loop) {
        exception_=nullptr;out_.reset();stopped_=false;source_=co2::stop_source{};
        return uv_queue_work(loop,&this->req_,&onWork,&onDone);
    }
    // Always try dequeue first. Only a running/finished worker needs the
    // cooperative notification. No user callback is invoked by this hook.
    void cancel() noexcept {
        if(uv_cancel(reinterpret_cast<uv_req_t*>(&this->req_))!=0)source_.request_stop();
    }
    Result<R> finish(int status) {
        if(exception_)std::rethrow_exception(exception_);
        if(status<0)return static_cast<Errc>(status);
        if(stopped_)return Errc::canceled;
        return WorkResult<R>::take(out_);
    }
  private:
    static void onWork(uv_work_t* req) noexcept {
        auto& self=*static_cast<Work*>(req->data);
        try{WorkResult<R>::run(self.fn_,self.source_.get_token(),self.out_);}
        catch(...){self.exception_=std::current_exception();}
    }
    static void onDone(uv_work_t* req,int status) noexcept {
        auto& self=*static_cast<Work*>(req->data);
        // libuv publishes all worker writes before after-work. This callback
        // entry is the cancellation/completion linearization point.
        self.stopped_=self.stopRequested();
        Base::complete(req,status); // may destroy self; no access afterwards
    }
    F fn_;
    co2::stop_source source_;
    co2compat::ResultStorage<R> out_;
    std::exception_ptr exception_;
    bool stopped_=false;
};
} // namespace detail
// Callable may accept a cooperative stop_token, or take no argument. The token
// is for polling; any callbacks registered on it must be noexcept. Worker/body
// and result-construction exceptions are rethrown only after after-work drains.
template<class F> UVCO_NODISCARD detail::Work<typename std::decay<F>::type> queueWork(Loop& loop,F&& fn) {
    return detail::Work<typename std::decay<F>::type>{loop,std::forward<F>(fn)};
}
} // namespace uvco
