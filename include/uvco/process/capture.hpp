#pragma once
#include <array>
#include <csignal>
#include <exception>
#include <string>
#include <tuple>
#include "uvco/process/process.hpp"
#include "uvco/sync/when_all_results.hpp"
namespace uvco {
enum class CaptureCancel { stopWaiting, killChild, killGroup };
struct CaptureOptions {
    std::string input;
    std::size_t stdoutLimit=1024U*1024U,stderrLimit=1024U*1024U;
    CaptureCancel cancel=CaptureCancel::stopWaiting;
    int killSignal=SIGTERM;
};
struct CaptureResult {ExitStatus status;std::string stdoutData,stderrData;};
namespace detail {
struct CaptureState {
    Loop& loop; CaptureOptions options; Process proc; Pipe in,out,err;
    CancelHook hook;bool interrupted=false;int killError=0;std::exception_ptr exception;
    CaptureState(Loop& l,CaptureOptions io):loop{l},options{std::move(io)},in{l},out{l},err{l}{}
    ~CaptureState(){hook.disarm();interrupt();}
    static void onCancel(void* p) noexcept {static_cast<CaptureState*>(p)->interrupt();}
    void interrupt() noexcept {
        if(interrupted)return;
        interrupted=true;
        if(proc.isOpen() && options.cancel!=CaptureCancel::stopWaiting){
            if(options.cancel==CaptureCancel::killGroup){
#ifndef _WIN32
                killError=uv_kill(-proc.pid(),options.killSignal);
#else
                killError=UV_ENOSYS;
#endif
            }else{auto r=proc.kill(options.killSignal);if(!r)killError=toInt(r.error());}
            if(killError==UV_ESRCH)killError=0;
        }
        if(in.isOpen())static_cast<void>(in.close());
        if(out.isOpen())static_cast<void>(out.close());
        if(err.isOpen())static_cast<void>(err.close());
    }
};
inline auto captureWait(CaptureState& s) CO2_BEG(co2::Task<Result<ExitStatus>>,(s),Result<ExitStatus> r;co2::stop_token token;) {
    CO2_AWAIT_SET(token,co2::getStopToken());s.hook.arm(s.loop,token,&CaptureState::onCancel,&s);
    CO2_AWAIT_SET(r,s.proc.wait());CO2_RETURN(r);
} CO2_END
inline auto captureInput(CaptureState& s) CO2_BEG(co2::Task<Result<void>>,(s),Result<void> r;co2::stop_token token;) {
    CO2_AWAIT_SET(token,co2::getStopToken());
    if(!token.stop_requested() && s.in.isOpen() && !s.options.input.empty())CO2_AWAIT_SET(r,s.in.write(buffer(s.options.input)));
    CO2_AWAIT(s.in.close());
    if(token.stop_requested())CO2_RETURN(Result<void>{Errc::canceled});
    CO2_RETURN(r);
} CO2_END
inline auto captureOutput(CaptureState& s,Pipe& pipe,std::size_t cap)
 CO2_BEG(co2::Task<Result<std::string>>,(s,pipe,cap),std::array<char,4096> bytes{};std::string data;Result<std::size_t> n;co2::stop_token token;) {
    CO2_AWAIT_SET(token,co2::getStopToken());
    while(pipe.isOpen() && !token.stop_requested()) {
        CO2_AWAIT_SET(n,pipe.read(buffer(bytes.data(),bytes.size())));
        if(!n){if(n.error()==Errc::eof)break;if(token.stop_requested())CO2_RETURN(Result<std::string>{Errc::canceled});CO2_RETURN(Result<std::string>{n.error()});}
        if(*n>cap-data.size())CO2_RETURN(Result<std::string>{static_cast<Errc>(UV_ENOBUFS)});
        data.append(bytes.data(),*n);
    }
    if(token.stop_requested())CO2_RETURN(Result<std::string>{Errc::canceled});
    CO2_RETURN(Result<std::string>{std::move(data)});
} CO2_END
// Convert join exceptions to stored state so the root can still await reaping
// before rethrowing, rather than unwinding past asynchronous cleanup.
template<class Op> struct CaptureJoin {
    CaptureState* state;Op op;
    bool await_ready() const noexcept{return op.await_ready();}
    template<class P> bool await_suspend(co2::coroutine_handle<P> h){return op.await_suspend(h);}
    auto await_resume() -> decltype(op.await_resume()) {
        try{return op.await_resume();}catch(...){state->exception=std::current_exception();return Errc::canceled;}
    }
};
template<class Op> CaptureJoin<Op> captureJoin(CaptureState& s,Op op){return {&s,std::move(op)};}
using CaptureValues=std::tuple<ExitStatus,std::string,std::string>;
} // namespace detail
inline auto communicate(Loop& loop,ProcessOptions options,CaptureOptions io={})
 CO2_BEG(co2::Task<Result<CaptureResult>>,(loop,options,io),detail::CaptureState state{loop,std::move(io)};
 Result<Process> spawned;Result<detail::CaptureValues> joined;co2::stop_token token;) {
    CO2_AWAIT_SET(token,co2::getStopToken());
    if(token.stop_requested())CO2_RETURN(Result<CaptureResult>{Errc::canceled});
    if(state.options.killSignal<=0)CO2_RETURN(Result<CaptureResult>{Errc::invalid_argument});
    if(state.options.cancel==CaptureCancel::killGroup){
#ifdef _WIN32
        CO2_RETURN(Result<CaptureResult>{static_cast<Errc>(UV_ENOSYS)});
#else
        options.detached=true; // a new session/group; never target our own group
#endif
    }
    options.stdio={ProcessStdio::createPipe(state.in,true,false),ProcessStdio::createPipe(state.out,false,true),ProcessStdio::createPipe(state.err,false,true)};
    spawned=spawn(loop,std::move(options));if(!spawned)CO2_RETURN(Result<CaptureResult>{spawned.error()});
    state.proc=std::move(*spawned);
    CO2_AWAIT_SET(joined,detail::captureJoin(state,whenAllResults(detail::captureWait(state),detail::captureInput(state),
        detail::captureOutput(state,state.out,state.options.stdoutLimit),detail::captureOutput(state,state.err,state.options.stderrLimit))));
    state.hook.disarm();
    if(!joined)state.interrupt();
    if(joined || (state.options.cancel!=CaptureCancel::stopWaiting && state.killError==0))CO2_AWAIT(state.proc.close());
    if(state.exception)std::rethrow_exception(state.exception);
    if(!joined){if(joined.error()==Errc::canceled && state.killError<0)CO2_RETURN(Result<CaptureResult>{static_cast<Errc>(state.killError)});CO2_RETURN(Result<CaptureResult>{joined.error()});}
    CO2_RETURN(Result<CaptureResult>{CaptureResult{std::get<0>(*joined),std::move(std::get<1>(*joined)),std::move(std::get<2>(*joined))}});
} CO2_END
inline auto capture(Loop& loop,ProcessOptions options,CaptureOptions io={}) -> co2::Task<Result<CaptureResult>> {
    return communicate(loop,std::move(options),std::move(io));
}
} // namespace uvco
