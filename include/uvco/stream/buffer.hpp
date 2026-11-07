#pragma once

#include <cstddef>
#include <string>
#include <vector>

// 读写缓冲区描述符（docs/architecture.md §4.6）：只是 (指针, 长度)，不拥有内存。
//
// 生命周期：数据必须在对应的 read()/write() 完成（await 恢复）之前保持有效。
// 注意 co2 是无栈协程：CO2_AWAIT 语句里的临时对象在挂起前就已销毁，因此
// buffer(std::string{...}) 这类临时对象的缓冲区会悬空——右值重载被删除了。

namespace uvco {

struct MutableBuffer {
    char* data = nullptr;
    std::size_t size = 0;
};

struct ConstBuffer {
    char const* data = nullptr;
    std::size_t size = 0;

    ConstBuffer() noexcept = default;
    ConstBuffer(char const* const d, std::size_t const n) noexcept : data{d}, size{n} {}
    ConstBuffer(MutableBuffer const b) noexcept : data{b.data}, size{b.size} {} // NOLINT
};

inline MutableBuffer buffer(void* const data, std::size_t const size) noexcept {
    return MutableBuffer{static_cast<char*>(data), size};
}
inline ConstBuffer buffer(void const* const data, std::size_t const size) noexcept {
    return ConstBuffer{static_cast<char const*>(data), size};
}

inline MutableBuffer buffer(std::string& s) noexcept { return MutableBuffer{&s[0], s.size()}; }
inline ConstBuffer buffer(std::string const& s) noexcept { return ConstBuffer{s.data(), s.size()}; }
ConstBuffer buffer(std::string&&) = delete;

inline MutableBuffer buffer(std::vector<char>& v) noexcept {
    return MutableBuffer{v.data(), v.size()};
}
inline ConstBuffer buffer(std::vector<char> const& v) noexcept {
    return ConstBuffer{v.data(), v.size()};
}
ConstBuffer buffer(std::vector<char>&&) = delete;

template <std::size_t N> MutableBuffer buffer(char (&array)[N]) noexcept {
    return MutableBuffer{array, N};
}

} // namespace uvco
