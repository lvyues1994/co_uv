#pragma once
#include <stdexcept>
#include <string>
#include "uvco/core/config.hpp"
#include "uvco/stream/buffer.hpp"
namespace uvco {
namespace detail {
// A writable string tail with rollback across suspension/exception. Uncommitted
// bytes remain invisible to the reader's bufferedBytes() observation.
class StringReadTail {
  public:
    StringReadTail() noexcept = default;
    StringReadTail(StringReadTail const &) = delete;
    StringReadTail &operator=(StringReadTail const &) = delete;
    ~StringReadTail() {
        rollback();
    }
    MutableBuffer prepare(std::string &data, std::size_t count, std::size_t *pending = nullptr) {
        UVCO_CONTRACT_CHECK(data_ == nullptr && count != 0);
        auto const old = data.size();
        if (count > data.max_size() - old)
            throw std::length_error("string read tail");
        data.resize(old + count);
        data_ = &data;
        old_ = old;
        count_ = count;
        pending_ = pending;
        if (pending_)
            *pending_ = count;
        return {&data[old], count};
    }
    void commit(std::size_t count) {
        UVCO_CONTRACT_CHECK(data_ != nullptr && count <= count_);
        data_->resize(old_ + count);
        clear();
    }
    void rollback() noexcept {
        if (data_) {
            data_->resize(old_);
            clear();
        }
    }

  private:
    void clear() noexcept {
        if (pending_)
            *pending_ = 0;
        pending_ = nullptr;
        data_ = nullptr;
    }
    std::string *data_ = nullptr;
    std::size_t old_ = 0, count_ = 0;
    std::size_t *pending_ = nullptr;
};
}
}
