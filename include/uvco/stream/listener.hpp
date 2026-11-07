#pragma once
#include "uvco/stream/tcp.hpp"
#include "uvco/stream/pipe.hpp"
namespace uvco {
// Thin role facade: deliberately no connected-stream read/write/connect API.
class TcpListener {
  public:
    TcpListener()=default;
    explicit TcpListener(Loop& loop):stream_{loop}{}
    TcpListener(TcpListener&&)=default;
    TcpListener& operator=(TcpListener&&)=default;
    TcpListener(TcpListener const&)=delete;
    TcpListener& operator=(TcpListener const&)=delete;
    Result<void> bind(SocketAddress const& address, unsigned flags=0) noexcept {return stream_.bind(address,flags);}
    Result<void> listen(int backlog=128) noexcept {return stream_.listen(backlog);}
    auto accept() noexcept {return stream_.accept();}
    auto close() noexcept {return stream_.close();}
    bool isOpen() const noexcept {return stream_.isOpen();}
    Loop& loop() const noexcept {return stream_.loop();}
    Result<SocketAddress> localAddress() const noexcept {return stream_.localAddress();}
  private:
    Tcp stream_;
};
// Thin role facade: deliberately no connected-stream read/write/connect API.
class PipeListener {
  public:
    PipeListener()=default;
    explicit PipeListener(Loop& loop):stream_{loop}{}
    PipeListener(PipeListener&&)=default;
    PipeListener& operator=(PipeListener&&)=default;
    PipeListener(PipeListener const&)=delete;
    PipeListener& operator=(PipeListener const&)=delete;
    Result<void> bind(std::string const& path) noexcept {return stream_.bind(path);}
    Result<void> listen(int backlog=128) noexcept {return stream_.listen(backlog);}
    auto accept() noexcept {return stream_.accept();}
    auto close() noexcept {return stream_.close();}
    bool isOpen() const noexcept {return stream_.isOpen();}
    Loop& loop() const noexcept {return stream_.loop();}
    Result<std::string> localName() const {return stream_.localName();}
  private:
    Pipe stream_;
};
} // namespace uvco
