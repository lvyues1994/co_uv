#pragma once
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include "uvco/core/req_awaiter.hpp"
#include "uvco/stream/address.hpp"

namespace uvco {
struct NameInfo { std::string host; std::string service; };
namespace detail {
// Read-only requests linearize cancellation once at callback entry. Cancellation
// visible there discards data, even if uv_cancel failed; later stop loses to completion.
class GetAddrInfo final : public ReqAwaiter<GetAddrInfo, uv_getaddrinfo_t, std::vector<SocketAddress>> {
    using Base = ReqAwaiter<GetAddrInfo, uv_getaddrinfo_t, std::vector<SocketAddress>>;
  public:
    GetAddrInfo(Loop& loop, std::string host, std::string service, addrinfo const& hints)
        : Base{loop}, host_{std::move(host)}, service_{std::move(service)} {
        hints_.ai_flags=hints.ai_flags; hints_.ai_family=hints.ai_family;
        hints_.ai_socktype=hints.ai_socktype; hints_.ai_protocol=hints.ai_protocol;
    }
    GetAddrInfo(GetAddrInfo&&) = default;
    int start(uv_loop_t* loop) {
        exception_=nullptr;
        if(host_.find('\0')!=std::string::npos || service_.find('\0')!=std::string::npos) return UV_EINVAL;
        return uv_getaddrinfo(loop,&req_,&onDone,host_.empty()?nullptr:host_.c_str(),
                              service_.empty()?nullptr:service_.c_str(),&hints_);
    }
    Result<std::vector<SocketAddress>> finish(int status) {
        if(status<0) return static_cast<Errc>(status);
        if(exception_) std::rethrow_exception(exception_);
        return std::move(result_);
    }
  private:
    struct FreeAddresses { addrinfo* value; ~FreeAddresses(){if(value)uv_freeaddrinfo(value);} };
    static void onDone(uv_getaddrinfo_t* req, int status, addrinfo* addresses) noexcept {
        auto& self=*static_cast<GetAddrInfo*>(req->data);
        bool const stopped=self.stopRequested();
        {
            FreeAddresses guard{addresses};
            if(stopped) status=UV_ECANCELED;
            else if(status==0) {
                try {
                    std::vector<SocketAddress> out;
                    for(auto* p=addresses;p;p=p->ai_next) {
                        if(p->ai_addr && (p->ai_family==AF_INET || p->ai_family==AF_INET6))
                            out.push_back(SocketAddress::fromRaw(p->ai_addr));
                    }
                    self.result_=Result<std::vector<SocketAddress>>{std::move(out)};
                } catch(std::bad_alloc const&) {status=UV_ENOMEM;}
                catch(...) {self.exception_=std::current_exception();}
            }
        } // free before resumption, which can destroy the operation
        Base::complete(req,status);
    }
    std::string host_,service_;
    addrinfo hints_{};
    Result<std::vector<SocketAddress>> result_{Errc::canceled};
    std::exception_ptr exception_;
};
class GetNameInfo final : public ReqAwaiter<GetNameInfo, uv_getnameinfo_t, NameInfo> {
    using Base=ReqAwaiter<GetNameInfo,uv_getnameinfo_t,NameInfo>;
  public:
    GetNameInfo(Loop& loop,SocketAddress address,int flags)
        :Base{loop},address_{address},flags_{flags}{}
    GetNameInfo(GetNameInfo&&)=default;
    int start(uv_loop_t* loop) {
        exception_=nullptr;
        return uv_getnameinfo(loop,&req_,&onDone,address_.raw(),flags_);
    }
    Result<NameInfo> finish(int status) {
        if(status<0)return static_cast<Errc>(status);
        if(exception_)std::rethrow_exception(exception_);
        return std::move(result_);
    }
  private:
    static void onDone(uv_getnameinfo_t* req,int status,char const* host,char const* service) noexcept {
        auto& self=*static_cast<GetNameInfo*>(req->data);
        if(self.stopRequested())status=UV_ECANCELED;
        else if(status==0) {
            try {self.result_=Result<NameInfo>{NameInfo{host,service}};}
            catch(std::bad_alloc const&){status=UV_ENOMEM;}
            catch(...){self.exception_=std::current_exception();}
        }
        Base::complete(req,status);
    }
    SocketAddress address_; int flags_;
    Result<NameInfo> result_{Errc::canceled};
    std::exception_ptr exception_;
};
} // namespace detail
UVCO_NODISCARD inline detail::GetAddrInfo getaddrinfo(Loop& loop,std::string host,std::string service={},addrinfo const& hints={}) {
    return detail::GetAddrInfo{loop,std::move(host),std::move(service),hints};
}
UVCO_NODISCARD inline detail::GetNameInfo getnameinfo(Loop& loop,SocketAddress address,int flags=0) {
    return detail::GetNameInfo{loop,address,flags};
}
} // namespace uvco
