# uvco

基于 [co2](https://github.com/lvyues1994/coro)（C++14 无栈协程库）对 libuv 的协程封装。
设计见 [docs/architecture.md](docs/architecture.md)。

当前进度：v0.1 第 1 步 **core**（`Result<T>`、`Loop`、`run()`、`detail::ReqAwaiter`）、第 2 步 **timer**（`sleep()`、`Timer`、通用句柄生命周期）、第 3 步 **sync**（`Event`、`Mutex`、`Channel<T>`、`whenAny`、`timeout`）、第 4 步 **stream**（`Tcp`、`Pipe`：拉模式 read、write、shutdown、listen/accept、connect）、第 5 步 **udp**（`Udp`：拉模式 recv 返回 `Datagram`、send、connect）。
fs 按文档 §6 的顺序继续实现。

## 要求

- C++14 编译器（已测试 g++ 13、clang++ 18）
- CMake ≥ 3.14（使用 CMakePresets.json 需 ≥ 3.21）
- libuv ≥ 1.40：优先 `find_package(libuv)`，其次 pkg-config，最后 FetchContent（v1.48.0）
- co2：优先 `find_package(co2 CONFIG)`，否则 FetchContent（固定提交，见 `UVCO_CO2_GIT_TAG`）

## 构建与测试

```sh
cmake --preset debug-gcc   && cmake --build --preset debug-gcc   && ctest --preset debug-gcc
cmake --preset debug-clang && cmake --build --preset debug-clang && ctest --preset debug-clang
```

在其他项目中使用：

```cmake
add_subdirectory(uvco)          # 或 FetchContent
target_link_libraries(app PRIVATE uvco::uvco)
```

## 示例

```cpp
#include <co2/co2.hpp>
#include <uvco/uvco.hpp>

auto work(uvco::Loop& loop, co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (loop, pool)) {
    CO2_AWAIT(co2::scheduleOn(pool));  // 去线程池算点东西
    CO2_AWAIT(co2::scheduleOn(loop));  // 回到 loop 线程后才能碰 libuv
    CO2_RETURN(42);
}
CO2_END

int main() {
    uvco::Loop loop;                   // 用户持有 Loop，绑定当前线程
    co2::ThreadPool pool{2};
    return uvco::run(loop, work(loop, pool)) == 42 ? 0 : 1;
}
```

错误以 `uvco::Result<T>` 返回（错误码即 libuv 的负 errno，`message()` 走 `uv_strerror`，
可转换为 `std::error_code`，category 名为 `"uv"`）；用法错误是契约违规而非异常。
