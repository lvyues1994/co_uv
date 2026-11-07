#pragma once

#include "uvco/core/co2_compat.hpp"

#include <cstddef>
#include <deque>
#include <limits>
#include <type_traits>
#include <utility>

#include <co2/coroutine_handle.hpp>
#include <co2/env.hpp>
#include <co2/stop_token.hpp>
#include "uvco/detail/libuv.hpp"

#include "uvco/core/config.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/result.hpp"
#include "uvco/sync/wait_queue.hpp"

// Channel<T>：loop 内的 FIFO 通道（docs/architecture.md §4.5）。
//
//   uvco::Channel<int> ch{loop};          // 无界
//   uvco::Channel<int> ch{loop, 16};      // 有界：缓冲满时 send 挂起
//   uvco::Channel<int> ch{loop, 0};       // 会合：send 挂起直到有接收者取走
//   CO2_AWAIT_SET(r, ch.send(42));        // Result<void>
//   CO2_AWAIT_SET(v, ch.recv());          // Result<int>
//   ch.close();
//
//   - 只能在 loop 线程上使用（契约）。发送者、接收者各自严格 FIFO。
//   - 有接收者等待时 send 直接把值交给队首接收者（不经过缓冲）；recv 取走缓冲后，
//     若有挂起的发送者，把队首发送者的值补进缓冲并唤醒它。被唤醒者延后到 loop 恢复。
//   - close()：之后 send → UV_EPIPE；recv 先取完缓冲，再返回 UV_EOF。挂起中的接收者
//     （此时缓冲必为空）以 UV_EOF 唤醒，挂起中的发送者以 UV_EPIPE 唤醒（值被丢弃）。
//     重复 close() 无效果。
//   - 取消：已请求停止 → UV_ECANCELED（即使可以立即完成；值不被发送 / 不被取走）；
//     挂起中 stop_token 被请求 → UV_ECANCELED 并离开队列。已被唤醒者结果不变。
//   - trySend / tryRecv：不挂起；无法立即完成时 UV_EAGAIN（trySend 失败时不移动实参）。
//   - 带着等待者销毁 Channel 是契约违规。

namespace uvco {

template <class T> class Channel {
    static_assert(std::is_move_constructible<T>::value, "Channel<T> requires a movable T");

    struct SendNode : detail::WaitNode {
        explicit SendNode(T&& value_) : value{std::move(value_)} {}
        SendNode(SendNode&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
            : detail::WaitNode{std::move(other)}, value{std::move(other.value)} {}
        T value;
    };

    struct RecvNode : detail::WaitNode {
        RecvNode() noexcept : result{Errc::canceled} {}
        RecvNode(RecvNode&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
            : detail::WaitNode{std::move(other)}, result{std::move(other.result)} {}
        Result<T> result;
    };

  public:
    static constexpr std::size_t unbounded = (std::numeric_limits<std::size_t>::max)();

    explicit Channel(Loop& loop, std::size_t const capacity = unbounded) noexcept
        : senders_{loop}, receivers_{loop}, capacity_{capacity} {}

    Channel(Channel const&) = delete;
    Channel& operator=(Channel const&) = delete;

    Loop& loop() const noexcept { return senders_.loop(); }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return buffer_.size(); }
    bool isClosed() const noexcept { return closed_; }

    void close() noexcept {
        senders_.checkThread();
        if (closed_) return;
        closed_ = true;
        receivers_.wakeAll(UV_EOF);
        senders_.wakeAll(UV_EPIPE);
    }

    template <class U> Result<void> trySend(U&& value) {
        senders_.checkThread();
        if (closed_) return Errc::broken_pipe;
        if (not canSendNow()) return Errc::again;
        sendNow(std::forward<U>(value));
        return {};
    }

    Result<T> tryRecv() {
        receivers_.checkThread();
        if (not buffer_.empty() || not senders_.empty()) return recvNow();
        if (closed_) return Errc::eof;
        return Errc::again;
    }

    class SendAwaiter {
      public:
        SendAwaiter(Channel& channel, T&& value) : channel_{&channel}, node_{std::move(value)} {}

        bool await_ready() const noexcept { return false; }

        template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
            Channel& channel = *channel_;
            channel.senders_.checkThread();
            co2::stop_token token = ::uvco::detail::co2compat::stopTokenOf(waiting);
            if (token.stop_requested()) {
                node_.status = UV_ECANCELED;
                return false;
            }
            if (channel.closed_) {
                node_.status = UV_EPIPE;
                return false;
            }
            if (channel.canSendNow()) {
                channel.sendNow(std::move(node_.value));
                node_.status = 0;
                return false;
            }
            channel.senders_.enqueue(node_, waiting, std::move(token));
            return true;
        }

        Result<void> await_resume() noexcept {
            node_.finish();
            return fromStatus(node_.status);
        }

      private:
        Channel* channel_;
        SendNode node_;
    };

    class RecvAwaiter {
      public:
        explicit RecvAwaiter(Channel& channel) noexcept : channel_{&channel} {}

        bool await_ready() const noexcept { return false; }

        template <class Promise> bool await_suspend(co2::coroutine_handle<Promise> const waiting) {
            Channel& channel = *channel_;
            channel.receivers_.checkThread();
            co2::stop_token token = ::uvco::detail::co2compat::stopTokenOf(waiting);
            if (token.stop_requested()) {
                node_.result = Errc::canceled;
                return false;
            }
            if (not channel.buffer_.empty() || not channel.senders_.empty()) {
                node_.result = channel.recvNow();
                return false;
            }
            if (channel.closed_) {
                node_.result = Errc::eof;
                return false;
            }
            channel.receivers_.enqueue(node_, waiting, std::move(token));
            return true;
        }

        Result<T> await_resume() {
            node_.finish();
            // 取消 / close 唤醒只设置 status；值由 sendNow 直接写进 result。
            if (node_.status < 0) return Result<T>{static_cast<Errc>(node_.status)};
            return std::move(node_.result);
        }

      private:
        Channel* channel_;
        RecvNode node_;
    };

    UVCO_NODISCARD SendAwaiter send(T value) { return SendAwaiter{*this, std::move(value)}; }
    UVCO_NODISCARD RecvAwaiter recv() noexcept { return RecvAwaiter{*this}; }

  private:
    bool canSendNow() const noexcept {
        return not receivers_.empty() || buffer_.size() < capacity_;
    }

    // 前置：canSendNow()。
    template <class U> void sendNow(U&& value) {
        if (not receivers_.empty()) {
            auto& receiver = static_cast<RecvNode&>(receivers_.pop());
            receiver.result = Result<T>{T(std::forward<U>(value))};
            receivers_.wake(receiver, 0);
            return;
        }
        buffer_.emplace_back(std::forward<U>(value));
    }

    // 前置：缓冲非空或有挂起的发送者。
    Result<T> recvNow() {
        if (buffer_.empty()) {
            // 会合（或缓冲恰好被取空）：直接从队首发送者取值。
            auto& sender = static_cast<SendNode&>(senders_.pop());
            Result<T> result{std::move(sender.value)};
            senders_.wake(sender, 0);
            return result;
        }
        Result<T> result{std::move(buffer_.front())};
        buffer_.pop_front();
        if (not senders_.empty() && buffer_.size() < capacity_) {
            auto& sender = static_cast<SendNode&>(senders_.pop());
            buffer_.push_back(std::move(sender.value));
            senders_.wake(sender, 0);
        }
        return result;
    }

    detail::WaitQueue senders_;
    detail::WaitQueue receivers_;
    std::deque<T> buffer_;
    std::size_t capacity_;
    bool closed_ = false;
};

template <class T> constexpr std::size_t Channel<T>::unbounded;

} // namespace uvco
