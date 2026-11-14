#pragma once
#include "uvco/detail/string_read_tail.hpp"
#include <string>
#include <co2/co2.hpp>
#include "uvco/core/result.hpp"
#include "uvco/stream/buffer.hpp"
namespace uvco {
// Optional pull-stream adapter. Reader and stream must outlive every operation;
// no concurrent reader operations or direct reads from the underlying stream.
// Cap/EOF/cancellation errors retain already buffered, unconsumed bytes.
template<class Stream> class BufferedReader {
    struct Lease {
        BufferedReader* owner=nullptr;
        void acquire(BufferedReader& r){UVCO_CONTRACT_CHECK(!r.busy_);r.busy_=true;owner=&r;}
        void release() noexcept {if(owner){owner->busy_=false;owner=nullptr;}}
        ~Lease(){release();}
        Result<std::string> finish(Result<std::string> r){release();return r;}
    };
  public:
    explicit BufferedReader(Stream& stream) noexcept:stream_{&stream}{}
    BufferedReader(BufferedReader const&)=delete;
    BufferedReader& operator=(BufferedReader const&)=delete;
    ~BufferedReader(){UVCO_CONTRACT_CHECK(!busy_);}
    auto readExactly(std::size_t size)
        CO2_BEG_MEMBER(co2::Task<Result<std::string>>,(size),Lease lease;Result<void> r;) {
        lease.acquire(*self);
        while(self->available()<size && !self->eof_){CO2_AWAIT_SET(r,self->fill());if(!r)CO2_RETURN(lease.finish(r.error()));}
        if(self->available()<size)CO2_RETURN(lease.finish(Errc::eof));
        CO2_RETURN(lease.finish(self->take(size,0)));
    } CO2_END
    // Delimiter excluded, CR is preserved; EOF returns the final unterminated
    // line, then eof. maxLength counts bytes excluding '\n'.
    auto readLine(std::size_t maxLength)
        CO2_BEG_MEMBER(co2::Task<Result<std::string>>,(maxLength),Lease lease;Result<void> r;std::size_t end=0;) {
        lease.acquire(*self);
        for(;;){
            end=self->data_.find('\n',self->offset_);
            if(end!=std::string::npos){
                if(end-self->offset_>maxLength)CO2_RETURN(lease.finish(static_cast<Errc>(UV_ENOBUFS)));
                CO2_RETURN(lease.finish(self->take(end-self->offset_,1)));
            }
            if(self->available()>maxLength)CO2_RETURN(lease.finish(static_cast<Errc>(UV_ENOBUFS)));
            if(self->eof_){if(self->available()==0)CO2_RETURN(lease.finish(Errc::eof));CO2_RETURN(lease.finish(self->take(self->available(),0)));}
            CO2_AWAIT_SET(r,self->fill());if(!r)CO2_RETURN(lease.finish(r.error()));
        }
    } CO2_END
    auto readToEnd(std::size_t maxBytes)
        CO2_BEG_MEMBER(co2::Task<Result<std::string>>,(maxBytes),Lease lease;Result<void> r;) {
        lease.acquire(*self);
        for(;;){
            if(self->available()>maxBytes)CO2_RETURN(lease.finish(static_cast<Errc>(UV_ENOBUFS)));
            if(self->eof_)CO2_RETURN(lease.finish(self->take(self->available(),0)));
            CO2_AWAIT_SET(r,self->fill());if(!r)CO2_RETURN(lease.finish(r.error()));
        }
    } CO2_END
    std::size_t bufferedBytes() const noexcept{return available();}
  private:
    std::size_t available() const noexcept{return data_.size()-offset_-pendingTail_;}
    Result<std::string> take(std::size_t n,std::size_t delimiter){
        std::string result{data_.data()+offset_,n};offset_+=n+delimiter;
        if(offset_==data_.size()){data_.clear();offset_=0;}
        return result;
    }
    auto fill() CO2_BEG_MEMBER(co2::Task<Result<void>>,(),detail::StringReadTail tail;MutableBuffer bytes;Result<std::size_t> n;) {
        if(self->offset_){self->data_.erase(0,self->offset_);self->offset_=0;}
        bytes=tail.prepare(self->data_,4096,&self->pendingTail_);
        CO2_AWAIT_SET(n,self->stream_->read(bytes));
        if(!n){tail.rollback();if(n.error()==Errc::eof){self->eof_=true;CO2_RETURN(Result<void>{});}CO2_RETURN(Result<void>{n.error()});}
        tail.commit(*n);if(*n==0)self->eof_=true;
        CO2_RETURN(Result<void>{});
    } CO2_END
    Stream* stream_;std::string data_;std::size_t offset_=0,pendingTail_=0;bool eof_=false,busy_=false;
};
} // namespace uvco
