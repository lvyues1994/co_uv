#pragma once

#include <climits>
#include <exception>
#include <new>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <co2/co2.hpp>
#include <uv.h>

#include "uvco/core/config.hpp"
#include "uvco/core/error.hpp"
#include "uvco/core/loop.hpp"
#include "uvco/core/req_awaiter.hpp"
#include "uvco/core/result.hpp"
#include "uvco/stream/buffer.hpp"

// fs（docs/architecture.md §4.8）：基于 detail::ReqAwaiter<…, uv_fs_t, …> 的文件系统操作。
//
// 每个操作都是一个 awaiter 类型 fs::XxxOp = fs::FsOp<detail::XxxSpec>：
//   - Spec 按值保存全部参数（路径是 std::string）：co2::Task 是惰性的，CO2_AWAIT 语句里的
//     临时对象在挂起前就已销毁，awaiter 不能借用调用者的参数；
//   - FsOp::start 用 Spec::submit 发起 uv_fs_xxx；libuv 回调里先把结果投影成值
//     （Spec::project），再 uv_fs_req_cleanup，最后恢复等待者——请求体在任何路径上都会被清理；
//   - 取消（stop_token）是尽力而为：uv_cancel 只能撤下还没被线程池取走的请求；已在执行的
//     请求（以及交给 io_uring 的请求，uv_cancel 返回 EBUSY）会照常完成；
//   - 迟到的取消：请求成功完成但取消已经请求过时，产生资源的操作（open / mkstemp / mkdtemp）
//     由 Spec::release 同步释放资源（关闭 fd、删除临时文件 / 目录），结果报 Errc::canceled，
//     不泄漏。不产生资源的操作照常报告真实结果（副作用已经发生，如 unlink）。
//
// 两种等待方式（§7 大请求存储）：
//   1. 右值：CO2_AWAIT_SET(r, uvco::fs::stat(loop, path));  uv_fs_t 约 440 字节，超出 co2 的
//      8 指针内联槽，awaiter 退化为一次堆分配；
//   2. 帧局部（零分配）：把 fs::StatOp op{loop, path}; 写进 CO2_BEG 的局部列表，以左值
//      CO2_AWAIT_SET(r, op); 等待。可重复等待；两次等待之间可经 op.args() 修改参数。
//
// File：只可移动的 fd 所有者。析构时若仍打开则**同步**关闭（uv_fs_close 无回调，阻塞调用
// 线程一次 close(2)）；需要异步关闭或想知道 close 的错误时 CO2_AWAIT(file.close())。
// File 的读写等操作按值捕获 fd：在操作在途时关闭 / 析构 / 移走 File 与对同一 fd 调用
// close(2) 一样是调用者的错误（fd 可能被重用）。

namespace uvco {
namespace fs {

class File;
struct TempFile;

using FileStat = uv_stat_t;

struct DirEntry {
    std::string name;
    uv_dirent_type_t type = UV_DIRENT_UNKNOWN;
};

namespace detail {

// ---- 同步清理（迟到取消的释放、File 析构）：没有人等待结果 ----

inline void closeSync(uv_file const fd) noexcept {
    uv_fs_t req;
    static_cast<void>(uv_fs_close(nullptr, &req, fd, nullptr));
    uv_fs_req_cleanup(&req);
}

inline void unlinkSync(char const* const path) noexcept {
    uv_fs_t req;
    static_cast<void>(uv_fs_unlink(nullptr, &req, path, nullptr));
    uv_fs_req_cleanup(&req);
}

inline void rmdirSync(char const* const path) noexcept {
    uv_fs_t req;
    static_cast<void>(uv_fs_rmdir(nullptr, &req, path, nullptr));
    uv_fs_req_cleanup(&req);
}

inline uv_buf_t toUvBuf(char* const data, std::size_t const size) noexcept {
    // 单次请求最多 UINT_MAX 字节：短读 / 短写本来就是合法结果。
    auto const len = size > UINT_MAX ? UINT_MAX : static_cast<unsigned>(size);
    return uv_buf_init(data, len);
}

template <class...> struct VoidT { using type = void; };

// Spec 若有 static void release(uv_fs_t&)，迟到取消时释放它产生的资源。
template <class Spec, class = void> struct Releases : std::false_type {};
template <class Spec>
struct Releases<Spec, typename VoidT<decltype(&Spec::release)>::type> : std::true_type {};

template <class T> struct Value {
    T value{};
    template <class Spec> void set(Spec& spec, uv_fs_t& req, Loop& loop) {
        value = spec.project(req, loop);
    }
    Result<T> take() { return Result<T>{std::move(value)}; }
};

template <> struct Value<void> {
    template <class Spec> void set(Spec&, uv_fs_t&, Loop&) noexcept {}
    Result<void> take() noexcept { return Result<void>{}; }
};

inline std::size_t countOf(uv_fs_t const& req) noexcept {
    return static_cast<std::size_t>(req.result);
}

// ---- 以 fd 为参数的操作 ----

struct ReadSpec {
    using value_type = std::size_t;
    uv_file fd = -1;
    MutableBuffer buf;
    std::int64_t offset = -1; // -1：当前位置
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        uv_buf_t const b = toUvBuf(buf.data, buf.size);
        return uv_fs_read(l, r, fd, &b, 1, offset, cb);
    }
    static std::size_t project(uv_fs_t& r, Loop&) noexcept { return countOf(r); }
};

struct WriteSpec {
    using value_type = std::size_t;
    uv_file fd = -1;
    ConstBuffer buf;
    std::int64_t offset = -1; // -1：当前位置
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        // libuv 只读取缓冲区，接口不带 const。
        uv_buf_t const b = toUvBuf(const_cast<char*>(buf.data), buf.size);
        return uv_fs_write(l, r, fd, &b, 1, offset, cb);
    }
    static std::size_t project(uv_fs_t& r, Loop&) noexcept { return countOf(r); }
};

struct FsyncSpec {
    using value_type = void;
    uv_file fd = -1;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_fsync(l, r, fd, cb);
    }
};

struct FtruncateSpec {
    using value_type = void;
    uv_file fd = -1;
    std::int64_t size = 0;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_ftruncate(l, r, fd, size, cb);
    }
};

struct FstatSpec {
    using value_type = FileStat;
    uv_file fd = -1;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_fstat(l, r, fd, cb);
    }
    static FileStat project(uv_fs_t& r, Loop&) noexcept { return r.statbuf; }
};

struct CloseSpec {
    using value_type = void;
    uv_file fd = -1;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_close(l, r, fd, cb);
    }
};

struct SendfileSpec {
    using value_type = std::size_t;
    uv_file out = -1;
    uv_file in = -1;
    std::int64_t inOffset = 0;
    std::size_t length = 0;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_sendfile(l, r, out, in, inOffset, length, cb);
    }
    static std::size_t project(uv_fs_t& r, Loop&) noexcept { return countOf(r); }
};

} // namespace detail

// ---- 通用 awaiter ----

template <class Spec>
class FsOp : public uvco::detail::ReqAwaiter<FsOp<Spec>, uv_fs_t, typename Spec::value_type> {
    using T = typename Spec::value_type;
    using Base = uvco::detail::ReqAwaiter<FsOp<Spec>, uv_fs_t, T>;
    friend Base;

  public:
    // 参数留空（之后经 args() 填写）：便于在 CO2_BEG 的局部列表里声明。
    explicit FsOp(Loop& loop) : Base{loop} {}

    template <class A0, class... A>
    FsOp(Loop& loop, A0&& a0, A&&... a)
        : Base{loop}, spec_{std::forward<A0>(a0), std::forward<A>(a)...} {}

    FsOp(FsOp&&) = default; // 只在发起前（ReqAwaiter 检查）

    // 两次等待之间修改参数；请求在途时调用是契约违规。
    Spec& args() noexcept {
        UVCO_CONTRACT_CHECK(not this->inFlight());
        return spec_;
    }

  private:
    int start(uv_loop_t* const loop) {
        cancelRequested_ = false;
        int const status = spec_.submit(loop, &this->req_, &FsOp::onDone);
        if (status < 0) uv_fs_req_cleanup(&this->req_);
        return status;
    }

    void cancel() noexcept {
        cancelRequested_ = true;
        Base::cancel(); // uv_cancel：失败（已在执行 / io_uring 的 EBUSY）则等它自然完成
    }

    Result<T> finish(int const status) {
        if (exception_) std::rethrow_exception(std::exchange(exception_, {}));
        if (status < 0) return static_cast<Errc>(status);
        return value_.take();
    }

    static void release(uv_fs_t& req, std::true_type) noexcept { Spec::release(req); }
    static void release(uv_fs_t&, std::false_type) noexcept {}

    static void onDone(uv_fs_t* const req) {
        auto* const self = static_cast<FsOp*>(static_cast<Base*>(req->data));
        int status = 0;
        if (req->result < 0) {
            status = static_cast<int>(req->result);
        } else if (self->cancelRequested_ && detail::Releases<Spec>::value) {
            // 取消来得太晚：请求已经成功，释放它产生的资源并报告取消。
            release(*req, detail::Releases<Spec>{});
            status = UV_ECANCELED;
        } else {
            try {
                self->value_.set(self->spec_, *req, self->loop());
            } catch (std::bad_alloc const&) {
                release(*req, detail::Releases<Spec>{});
                status = UV_ENOMEM;
            } catch (...) {
                release(*req, detail::Releases<Spec>{});
                self->exception_ = std::current_exception();
            }
        }
        uv_fs_req_cleanup(req);
        Base::complete(req, status);
    }

    Spec spec_{};
    detail::Value<T> value_;
    std::exception_ptr exception_;
    bool cancelRequested_ = false;
};

using ReadOp = FsOp<detail::ReadSpec>;
using WriteOp = FsOp<detail::WriteSpec>;
using FsyncOp = FsOp<detail::FsyncSpec>;
using FtruncateOp = FsOp<detail::FtruncateSpec>;
using FstatOp = FsOp<detail::FstatSpec>;
// Closing is resource cleanup, not a cancelable operation. Own the fd even
// before await_suspend, so dropping the awaiter cannot leak it.
class CloseOp : public uvco::detail::ReqAwaiter<CloseOp, uv_fs_t, void> {
    using Base = uvco::detail::ReqAwaiter<CloseOp, uv_fs_t, void>;
  public:
    static constexpr bool cancellable = false;
    static constexpr bool respectStop = false;
    explicit CloseOp(Loop& loop, uv_file fd = -1) : Base{loop} { spec_.fd=fd; }
    CloseOp(CloseOp&& other) noexcept : Base{std::move(other)}, spec_{other.spec_} {
        other.spec_.fd=-1;
    }
    ~CloseOp() { if(spec_.fd>=0) detail::closeSync(spec_.fd); }
    detail::CloseSpec& args() noexcept {
        UVCO_CONTRACT_CHECK(not this->inFlight()); return spec_;
    }
    int start(uv_loop_t* loop) {
        uv_file fd=std::exchange(spec_.fd,-1);
        int status=uv_fs_close(loop,&req_,fd,&onDone);
        if(status<0) { uv_fs_req_cleanup(&req_); if(fd>=0) detail::closeSync(fd); }
        return status;
    }
    Result<void> finish(int status) { return fromStatus(status); }
  private:
    static void onDone(uv_fs_t* r) {
        int status=r->result<0 ? static_cast<int>(r->result) : 0;
        uv_fs_req_cleanup(r); Base::complete(r,status);
    }
    detail::CloseSpec spec_;
};
using SendfileOp = FsOp<detail::SendfileSpec>;

// ---- File ----

class File {
  public:
    File() noexcept = default;
    // 接管一个已打开的 fd。
    File(Loop& loop, uv_file const fd) noexcept : loop_{&loop}, fd_{fd} {}

    File(File const&) = delete;
    File& operator=(File const&) = delete;
    File(File&& other) noexcept : loop_{other.loop_}, fd_{other.release()} {}
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            reset();
            loop_ = other.loop_;
            fd_ = other.release();
        }
        return *this;
    }

    // 仍打开则同步关闭（见文件头注释）。
    ~File() { reset(); }

    bool isOpen() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return isOpen(); }
    uv_file fd() const noexcept { return fd_; }
    Loop& loop() const noexcept {
        UVCO_CONTRACT_CHECK(loop_ != nullptr);
        return *loop_;
    }

    // 放弃所有权，返回 fd（之后由调用者关闭）。
    uv_file release() noexcept {
        uv_file const fd = fd_;
        fd_ = -1;
        return fd;
    }

    // 同步关闭（若打开）。
    void reset() noexcept {
        if (fd_ >= 0) detail::closeSync(release());
    }

    // offset = -1：从当前位置读写（并移动文件位置）。
    ReadOp read(MutableBuffer const buf, std::int64_t const offset = -1) const {
        return ReadOp{loop(), fd_, buf, offset};
    }
    WriteOp write(ConstBuffer const buf, std::int64_t const offset = -1) const {
        return WriteOp{loop(), fd_, buf, offset};
    }
    FsyncOp sync() const { return FsyncOp{loop(), fd_}; }
    FtruncateOp truncate(std::int64_t const size) const { return FtruncateOp{loop(), fd_, size}; }
    FstatOp stat() const { return FstatOp{loop(), fd_}; }

    // 异步关闭：File 立即变空，返回的 awaiter 持有 fd。不等待它则 fd 不会被关闭——
    // 想要“即发即忘”请用 reset()（同步）或直接析构。
    CloseOp close() noexcept { return CloseOp{loop(), release()}; }

  private:
    Loop* loop_ = nullptr;
    uv_file fd_ = -1;
};

struct TempFile {
    File file;
    std::string path;
};

namespace detail {

// ---- 以路径为参数的操作 ----

struct OpenSpec {
    using value_type = File;
    std::string path;
    int flags = UV_FS_O_RDONLY;
    int mode = 0644;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_open(l, r, path.c_str(), flags, mode, cb);
    }
    static File project(uv_fs_t& r, Loop& loop) noexcept {
        return File{loop, static_cast<uv_file>(r.result)};
    }
    static void release(uv_fs_t& r) noexcept { closeSync(static_cast<uv_file>(r.result)); }
};

struct StatSpec {
    using value_type = FileStat;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_stat(l, r, path.c_str(), cb);
    }
    static FileStat project(uv_fs_t& r, Loop&) noexcept { return r.statbuf; }
};

struct LstatSpec {
    using value_type = FileStat;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_lstat(l, r, path.c_str(), cb);
    }
    static FileStat project(uv_fs_t& r, Loop&) noexcept { return r.statbuf; }
};

struct UnlinkSpec {
    using value_type = void;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_unlink(l, r, path.c_str(), cb);
    }
};

struct MkdirSpec {
    using value_type = void;
    std::string path;
    int mode = 0777;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_mkdir(l, r, path.c_str(), mode, cb);
    }
};

struct RmdirSpec {
    using value_type = void;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_rmdir(l, r, path.c_str(), cb);
    }
};

struct RenameSpec {
    using value_type = void;
    std::string path;
    std::string newPath;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_rename(l, r, path.c_str(), newPath.c_str(), cb);
    }
};

struct CopyfileSpec {
    using value_type = void;
    std::string path;
    std::string newPath;
    int flags = 0; // UV_FS_COPYFILE_EXCL / _FICLONE / _FICLONE_FORCE
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_copyfile(l, r, path.c_str(), newPath.c_str(), flags, cb);
    }
};

// 模板以 "XXXXXX" 结尾；结果路径在 req.path 里。
struct MkdtempSpec {
    using value_type = std::string;
    std::string tpl;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_mkdtemp(l, r, tpl.c_str(), cb);
    }
    static std::string project(uv_fs_t& r, Loop&) { return r.path; }
    static void release(uv_fs_t& r) noexcept { rmdirSync(r.path); }
};

struct MkstempSpec {
    using value_type = TempFile;
    std::string tpl;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_mkstemp(l, r, tpl.c_str(), cb);
    }
    static TempFile project(uv_fs_t& r, Loop& loop) {
        // Allocate before taking fd ownership: callback release owns it on throw.
        std::string path{r.path};
        return TempFile{File{loop, static_cast<uv_file>(r.result)}, std::move(path)};
    }
    static void release(uv_fs_t& r) noexcept {
        closeSync(static_cast<uv_file>(r.result));
        unlinkSync(r.path);
    }
};

struct ReadlinkSpec {
    using value_type = std::string;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_readlink(l, r, path.c_str(), cb);
    }
    static std::string project(uv_fs_t& r, Loop&) { return static_cast<char const*>(r.ptr); }
};

struct RealpathSpec {
    using value_type = std::string;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_realpath(l, r, path.c_str(), cb);
    }
    static std::string project(uv_fs_t& r, Loop&) { return static_cast<char const*>(r.ptr); }
};

// 不含 "." 与 ".."；顺序由文件系统决定。
struct ScandirSpec {
    using value_type = std::vector<DirEntry>;
    std::string path;
    int submit(uv_loop_t* const l, uv_fs_t* const r, uv_fs_cb const cb) const {
        return uv_fs_scandir(l, r, path.c_str(), 0, cb);
    }
    static std::vector<DirEntry> project(uv_fs_t& r, Loop&) {
        std::vector<DirEntry> out;
        uv_dirent_t entry;
        while (uv_fs_scandir_next(&r, &entry) == 0) out.push_back(DirEntry{entry.name, entry.type});
        return out;
    }
};

} // namespace detail

using OpenOp = FsOp<detail::OpenSpec>;
using StatOp = FsOp<detail::StatSpec>;
using LstatOp = FsOp<detail::LstatSpec>;
using UnlinkOp = FsOp<detail::UnlinkSpec>;
using MkdirOp = FsOp<detail::MkdirSpec>;
using RmdirOp = FsOp<detail::RmdirSpec>;
using RenameOp = FsOp<detail::RenameSpec>;
using CopyfileOp = FsOp<detail::CopyfileSpec>;
using MkdtempOp = FsOp<detail::MkdtempSpec>;
using MkstempOp = FsOp<detail::MkstempSpec>;
using ReadlinkOp = FsOp<detail::ReadlinkSpec>;
using RealpathOp = FsOp<detail::RealpathSpec>;
using ScandirOp = FsOp<detail::ScandirSpec>;

// ---- 便捷函数：返回右值 awaiter（一次堆分配；零分配请用帧局部 XxxOp） ----

// flags：UV_FS_O_RDONLY / UV_FS_O_WRONLY | UV_FS_O_CREAT | … ；mode 只在创建时使用。
inline OpenOp open(Loop& loop, std::string path, int const flags, int const mode = 0644) {
    return OpenOp{loop, std::move(path), flags, mode};
}
inline StatOp stat(Loop& loop, std::string path) { return StatOp{loop, std::move(path)}; }
inline LstatOp lstat(Loop& loop, std::string path) { return LstatOp{loop, std::move(path)}; }
inline UnlinkOp unlink(Loop& loop, std::string path) { return UnlinkOp{loop, std::move(path)}; }
inline MkdirOp mkdir(Loop& loop, std::string path, int const mode = 0777) {
    return MkdirOp{loop, std::move(path), mode};
}
inline RmdirOp rmdir(Loop& loop, std::string path) { return RmdirOp{loop, std::move(path)}; }
inline RenameOp rename(Loop& loop, std::string from, std::string to) {
    return RenameOp{loop, std::move(from), std::move(to)};
}
inline CopyfileOp copyfile(Loop& loop, std::string from, std::string to, int const flags = 0) {
    return CopyfileOp{loop, std::move(from), std::move(to), flags};
}
inline MkdtempOp mkdtemp(Loop& loop, std::string tpl) { return MkdtempOp{loop, std::move(tpl)}; }
inline MkstempOp mkstemp(Loop& loop, std::string tpl) { return MkstempOp{loop, std::move(tpl)}; }
inline ReadlinkOp readlink(Loop& loop, std::string path) {
    return ReadlinkOp{loop, std::move(path)};
}
inline RealpathOp realpath(Loop& loop, std::string path) {
    return RealpathOp{loop, std::move(path)};
}
inline ScandirOp scandir(Loop& loop, std::string path) { return ScandirOp{loop, std::move(path)}; }
// 从 in 的 inOffset 处复制至多 length 字节到 out 的当前位置；返回实际字节数。
inline SendfileOp sendfile(File const& out, File const& in, std::int64_t const inOffset,
                           std::size_t const length) {
    return SendfileOp{out.loop(), out.fd(), in.fd(), inOffset, length};
}

// ---- readFile / writeFile：帧局部 awaiter，除 Task 帧与数据本身外零分配 ----

using StringResult = Result<std::string>;

// 读出整个文件。
inline auto readFile(Loop& loop, std::string path)
    CO2_BEG(co2::Task<StringResult>, (loop, path), OpenOp openOp{loop}; Result<File> opened;
            File file; ReadOp readOp{loop}; Result<std::size_t> n; std::string data;
            std::size_t used = 0; CloseOp closeOp{loop}; Result<void> closed;) {
    openOp.args().path = std::move(path);
    openOp.args().flags = UV_FS_O_RDONLY;
    CO2_AWAIT_SET(opened, openOp);
    if (not opened) CO2_RETURN(StringResult{opened.error()});
    file = std::move(*opened);
    for (;;) {
        if (data.size() - used < 16 * 1024) data.resize(used + 64 * 1024);
        readOp.args() = detail::ReadSpec{file.fd(), MutableBuffer{&data[used], data.size() - used}, -1};
        CO2_AWAIT_SET(n, readOp);
        if (not n) CO2_RETURN(StringResult{n.error()}); // file 析构时同步关闭
        if (*n == 0) break;
        used += *n;
    }
    data.resize(used);
    closeOp.args().fd = file.release();
    CO2_AWAIT_SET(closed, closeOp);
    if (not closed) CO2_RETURN(StringResult{closed.error()});
    CO2_RETURN(StringResult{std::move(data)});
}
CO2_END

// 写入整个文件（默认创建或截断）。data 按值保存在帧里。
inline auto writeFile(Loop& loop, std::string path, std::string data,
                      int flags = UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC, int mode = 0644)
    CO2_BEG(co2::Task<Result<void>>, (loop, path, data, flags, mode), OpenOp openOp{loop};
            Result<File> opened; File file; WriteOp writeOp{loop}; Result<std::size_t> n;
            std::size_t done = 0; CloseOp closeOp{loop}; Result<void> closed;) {
    openOp.args() = detail::OpenSpec{std::move(path), flags, mode};
    CO2_AWAIT_SET(opened, openOp);
    if (not opened) CO2_RETURN(Result<void>{opened.error()});
    file = std::move(*opened);
    while (done < data.size()) {
        writeOp.args() = detail::WriteSpec{file.fd(), ConstBuffer{data.data() + done, data.size() - done}, -1};
        CO2_AWAIT_SET(n, writeOp);
        if (not n) CO2_RETURN(Result<void>{n.error()});
        done += *n;
    }
    closeOp.args().fd = file.release();
    CO2_AWAIT_SET(closed, closeOp);
    CO2_RETURN(closed);
}
CO2_END

} // namespace fs
} // namespace uvco
