#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <uv.h>

#include <co2/co2.hpp>

#include "test.hpp"
#include "uvco/uvco.hpp"

#ifndef UVCO_TEST_TMP_DIR
#error "UVCO_TEST_TMP_DIR must name a writable directory inside the build tree"
#endif

// ---- 计数的全局 operator new：验证帧局部 awaiter 零分配 ----
// TSan 的静态运行时自带 operator new/delete，不能替换：那时跳过计数断言。

#if defined(__SANITIZE_THREAD__)
#define UVCO_TEST_COUNT_NEW 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UVCO_TEST_COUNT_NEW 0
#endif
#endif
#ifndef UVCO_TEST_COUNT_NEW
#define UVCO_TEST_COUNT_NEW 1
#endif

namespace {
std::atomic<std::size_t> gNews{0};
} // namespace

#if UVCO_TEST_COUNT_NEW
// Keep the matching malloc/free replacement pair opaque to GCC's optimized
// mismatched-new-delete heuristic. Do not weaken -Werror or sanitizer checks.
#if defined(__GNUC__)
#define UVCO_TEST_ALLOC_HOOK __attribute__((noinline))
#else
#define UVCO_TEST_ALLOC_HOOK
#endif
UVCO_TEST_ALLOC_HOOK void* operator new(std::size_t const size) {
    gNews.fetch_add(1, std::memory_order_relaxed);
    if (void* const p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
UVCO_TEST_ALLOC_HOOK void operator delete(void* const p) noexcept { std::free(p); }
UVCO_TEST_ALLOC_HOOK void operator delete(void* const p, std::size_t) noexcept { std::free(p); }
#undef UVCO_TEST_ALLOC_HOOK
#endif

namespace {

using namespace std::chrono_literals;
using uvco::Errc;
using uvco::Result;
namespace fs = uvco::fs;

// 每个测试进程一个独立目录（io_uring 开 / 关两个进程可能并行），全部在构建目录下。
std::string const& scratch() {
    static std::string const dir = [] {
        std::string d = std::string{UVCO_TEST_TMP_DIR} + "/fs-scratch-" + std::to_string(getpid());
        ::mkdir(d.c_str(), 0777);
        return d;
    }();
    return dir;
}

std::string path(char const* const name) { return scratch() + "/" + name; }

std::size_t openFdCount() {
    std::size_t n = 0;
    DIR* const d = ::opendir("/proc/self/fd");
    CHECK(d != nullptr);
    if (d == nullptr) return 0;
    while (::readdir(d) != nullptr) ++n;
    ::closedir(d);
    return n;
}

bool exists(std::string const& p) {
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0;
}

// ---- 协程 ----

auto roundTrip(uvco::Loop& loop, std::string p, std::string data)
    CO2_BEG(co2::Task<bool>, (loop, p, data), Result<void> w; Result<std::string> r;
            Result<fs::FileStat> st; Result<void> u; Result<fs::FileStat> gone;) {
    CO2_AWAIT_SET(w, fs::writeFile(loop, p, data));
    CHECK(w);
    CO2_AWAIT_SET(r, fs::readFile(loop, p));
    CHECK(r && *r == data);
    CO2_AWAIT_SET(st, fs::stat(loop, p));
    CHECK(st && st->st_size == data.size());
    CO2_AWAIT_SET(u, fs::unlink(loop, p));
    CHECK(u);
    CO2_AWAIT_SET(gone, fs::stat(loop, p));
    CHECK(not gone && gone.error() == Errc::no_such_file);
    CO2_RETURN(true);
}
CO2_END

auto fileOps(uvco::Loop& loop, std::string p)
    CO2_BEG(co2::Task<bool>, (loop, p), Result<fs::File> f; fs::File file; Result<std::size_t> n;
            std::string buf; Result<void> v; Result<fs::FileStat> st; Result<void> closed;) {
    CO2_AWAIT_SET(f, fs::open(loop, p, UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_TRUNC, 0600));
    CHECK(f && f->isOpen());
    file = std::move(*f);
    CO2_AWAIT_SET(n, file.write(uvco::ConstBuffer{"hello world", 11}));
    CHECK(n && *n == 11U);
    CO2_AWAIT_SET(n, file.write(uvco::ConstBuffer{"W", 1}, 6)); // 定位写
    CHECK(n && *n == 1U);
    buf.assign(32, '\0');
    CO2_AWAIT_SET(n, file.read(uvco::buffer(buf), 0)); // 定位读
    CHECK(n && *n == 11U && buf.compare(0, 11, "hello World") == 0);
    CO2_AWAIT_SET(n, file.read(uvco::buffer(buf))); // 当前位置在文件尾
    CHECK(n && *n == 0U);
    CO2_AWAIT_SET(v, file.truncate(5));
    CHECK(v);
    CO2_AWAIT_SET(v, file.sync());
    CHECK(v);
    CO2_AWAIT_SET(st, file.stat());
    CHECK(st && st->st_size == 5U);
    CO2_AWAIT_SET(closed, file.close());
    CHECK(closed);
    CHECK(not file.isOpen());
    // 关闭后的 File 仍记得 loop：操作报告 EBADF。
    CO2_AWAIT_SET(st, file.stat());
    CHECK(not st && st.error() == Errc::bad_descriptor);
    CO2_RETURN(true);
}
CO2_END

auto dirOps(uvco::Loop& loop, std::string base)
    CO2_BEG(co2::Task<bool>, (loop, base), Result<void> v; Result<std::vector<fs::DirEntry>> list;
            Result<std::string> s; Result<std::string> tmp; Result<fs::TempFile> tf;
            Result<fs::FileStat> st; Result<std::string> text; Result<fs::File> in;
            Result<fs::File> out; Result<std::size_t> sent;) {
    CO2_AWAIT_SET(v, fs::mkdir(loop, base + "/d"));
    CHECK(v);
    CO2_AWAIT_SET(v, fs::mkdir(loop, base + "/d"));
    CHECK(not v && v.error() == static_cast<Errc>(UV_EEXIST));
    CO2_AWAIT_SET(v, fs::writeFile(loop, base + "/d/a", std::string{"abc"}));
    CHECK(v);
    CO2_AWAIT_SET(v, fs::rename(loop, base + "/d/a", base + "/d/b"));
    CHECK(v);
    CO2_AWAIT_SET(v, fs::copyfile(loop, base + "/d/b", base + "/d/c"));
    CHECK(v);
    CO2_AWAIT_SET(v, fs::copyfile(loop, base + "/d/b", base + "/d/c", UV_FS_COPYFILE_EXCL));
    CHECK(not v && v.error() == static_cast<Errc>(UV_EEXIST));
    CO2_AWAIT_SET(list, fs::scandir(loop, base + "/d"));
    CHECK(list && list->size() == 2U);
    if (list && list->size() == 2U) {
        bool const sorted = (*list)[0].name < (*list)[1].name;
        CHECK((*list)[sorted ? 0 : 1].name == "b" && (*list)[sorted ? 1 : 0].name == "c");
        CHECK((*list)[0].type == UV_DIRENT_FILE);
    }
    CHECK(::symlink("c", (base + "/d/l").c_str()) == 0);
    CO2_AWAIT_SET(s, fs::readlink(loop, base + "/d/l"));
    CHECK(s && *s == "c");
    CO2_AWAIT_SET(s, fs::realpath(loop, base + "/d/l"));
    CHECK(s && s->size() > 2U && s->compare(s->size() - 2, 2, "/c") == 0);
    CO2_AWAIT_SET(st, fs::lstat(loop, base + "/d/l"));
    CHECK(st && S_ISLNK(st->st_mode));
    CO2_AWAIT_SET(st, fs::stat(loop, base + "/d/l"));
    CHECK(st && S_ISREG(st->st_mode) && st->st_size == 3U);
    // sendfile：c 的 "bc" -> e
    CO2_AWAIT_SET(in, fs::open(loop, base + "/d/c", UV_FS_O_RDONLY));
    CO2_AWAIT_SET(out, fs::open(loop, base + "/d/e", UV_FS_O_WRONLY | UV_FS_O_CREAT, 0600));
    CHECK(in && out);
    CO2_AWAIT_SET(sent, fs::sendfile(*out, *in, 1, 100));
    CHECK(sent && *sent == 2U);
    in = Result<fs::File>{Errc::canceled}; // 析构 File：同步关闭
    out = Result<fs::File>{Errc::canceled};
    CO2_AWAIT_SET(text, fs::readFile(loop, base + "/d/e"));
    CHECK(text && *text == "bc");
    CO2_AWAIT_SET(v, fs::rmdir(loop, base + "/d"));
    CHECK(not v && v.error() == static_cast<Errc>(UV_ENOTEMPTY));
    for (char const* name : {"/d/b", "/d/c", "/d/e", "/d/l"}) CHECK(::unlink((base + name).c_str()) == 0);
    CO2_AWAIT_SET(v, fs::rmdir(loop, base + "/d"));
    CHECK(v);
    // 临时目录 / 文件
    CO2_AWAIT_SET(tmp, fs::mkdtemp(loop, base + "/tmpdir-XXXXXX"));
    CHECK(tmp && tmp->find("XXXXXX") == std::string::npos && exists(*tmp));
    CO2_AWAIT_SET(v, fs::rmdir(loop, *tmp));
    CHECK(v);
    CO2_AWAIT_SET(tf, fs::mkstemp(loop, base + "/tmpfile-XXXXXX"));
    CHECK(tf && tf->file.isOpen() && exists(tf->path));
    CO2_AWAIT_SET(sent, tf->file.write(uvco::ConstBuffer{"xy", 2}));
    CHECK(sent && *sent == 2U);
    CO2_AWAIT_SET(v, fs::unlink(loop, tf->path));
    CHECK(v);
    CO2_RETURN(true);
}
CO2_END

// 帧局部 awaiter：等待本身不分配；右值 awaiter（uv_fs_t 超出内联槽）分配一次。
auto allocations(uvco::Loop& loop, std::string p)
    CO2_BEG(co2::Task<bool>, (loop, p), fs::StatOp op{loop, p}; Result<fs::FileStat> st;
            std::size_t before = 0; std::size_t frameLocal = 0; std::size_t rvalue = 0;) {
    CO2_AWAIT_SET(st, op); // 预热（线程池启动等）
    CHECK(st);
    before = gNews.load();
    CO2_AWAIT_SET(st, op);
    frameLocal = gNews.load() - before;
    CHECK(st);
    op.args().path = "/no/such/uvco/file";
    CO2_AWAIT_SET(st, op);
    CHECK(not st && st.error() == Errc::no_such_file);
    before = gNews.load();
    CO2_AWAIT_SET(st, fs::stat(loop, p));
    rvalue = gNews.load() - before;
    CHECK(st);
    if (UVCO_TEST_COUNT_NEW) {
        CHECK(frameLocal == 0U);
        CHECK(rvalue >= 1U); // path 复制 + awaiter 槽溢出
    }
    CO2_RETURN(true);
}
CO2_END

auto openResult(uvco::Loop& loop, std::string p, int flags)
    CO2_BEG(co2::Task<Result<fs::File>>, (loop, p, flags), Result<fs::File> r;) {
    CO2_AWAIT_SET(r, fs::open(loop, p, flags, 0600));
    CO2_RETURN(std::move(r));
}
CO2_END

auto statSetsFlag(uvco::Loop& loop, std::string p, bool& resumed)
    CO2_BEG(co2::Task<bool>, (loop, p, resumed), Result<fs::FileStat> st;) {
    CO2_AWAIT_SET(st, fs::stat(loop, p));
    resumed = true;
    CO2_RETURN(static_cast<bool>(st));
}
CO2_END

} // namespace

TEST_CASE("fs: writeFile / readFile / stat / unlink round trip (multi-chunk)") {
    uvco::Loop loop;
    std::string data;
    for (int i = 0; i < 300 * 1024; ++i) data.push_back(static_cast<char>('a' + i % 26));
    CHECK(uvco::run(loop, roundTrip(loop, path("big"), data)));
    CHECK(uvco::run(loop, roundTrip(loop, path("empty"), std::string{})));
}

TEST_CASE("fs: File open / positional and sequential read-write / truncate / sync / fstat / close") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, fileOps(loop, path("file"))));
    CHECK(::unlink(path("file").c_str()) == 0);
}

TEST_CASE("fs: mkdir / rename / copyfile / scandir / readlink / realpath / sendfile / temp") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, dirOps(loop, scratch())));
}

TEST_CASE("fs: missing file and readFile errors are Results") {
    uvco::Loop loop;
    auto const r = uvco::run(loop, fs::readFile(loop, path("missing")));
    CHECK(not r && r.error() == Errc::no_such_file);
    auto const d = uvco::run(loop, fs::readFile(loop, scratch())); // 读目录：EISDIR
    CHECK(not d && d.error() == static_cast<Errc>(UV_EISDIR));
}

TEST_CASE("fs: frame-local awaiter allocates nothing per await") {
    uvco::Loop loop;
    CHECK(uvco::run(loop, allocations(loop, UVCO_TEST_TMP_DIR)));
}

TEST_CASE("fs: File destructor closes synchronously; release() hands the fd over") {
    uvco::Loop loop;
    std::size_t const base = openFdCount();
    {
        auto r = uvco::run(loop, openResult(loop, path("dtor"), UV_FS_O_WRONLY | UV_FS_O_CREAT));
        CHECK(r && r->isOpen());
        CHECK(openFdCount() == base + 1);
    }
    CHECK(openFdCount() == base);
    auto r = uvco::run(loop, openResult(loop, path("dtor"), UV_FS_O_RDONLY));
    CHECK(r);
    uv_file const fd = r->release();
    CHECK(not r->isOpen());
    r = Result<fs::File>{Errc::canceled};
    CHECK(::fcntl(fd, F_GETFD) != -1); // 没有被关闭
    CHECK(::close(fd) == 0);
    CHECK(::unlink(path("dtor").c_str()) == 0);
}

TEST_CASE("fs: stop requested before the request starts: canceled, nothing created") {
    uvco::Loop loop;
    co2::stop_source source;
    source.request_stop();
    auto const r = uvco::run(loop, openResult(loop, path("never"), UV_FS_O_WRONLY | UV_FS_O_CREAT),
                             source.get_token());
    CHECK(not r && r.error() == Errc::canceled);
    CHECK(not exists(path("never")));
}

bool ioUringDisabled() {
    char const* const v = std::getenv("UV_USE_IO_URING");
    return v != nullptr && std::string{v} == "0";
}

// 迟到的取消：open 阻塞在 FIFO 上（读端等写端），因此请求肯定已在线程池里执行，uv_cancel
// 无法撤下它。之后打开写端，open 成功——release 钩子必须关掉这个 fd 并报告取消。
// io_uring 的 openat 对 FIFO 不阻塞（内核先以非阻塞方式打开），open 会在取消之前就成功；
// 那时只检查不泄漏。确定性的 release 路径由 UV_USE_IO_URING=0 的那一遍覆盖。
TEST_CASE("fs: late cancel of a running open releases the fd (no leak in /proc/self/fd)") {
    std::string const fifo = path("fifo");
    static_cast<void>(::unlink(fifo.c_str()));
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    std::size_t const base = openFdCount();
    int released = 0;
    for (int round = 0; round < 3; ++round) {
        uvco::Loop loop;
        co2::stop_source source;
        bool writerOpened = false;
        std::atomic<bool> finished{false};
        bool finishedBeforeStop = false;
        std::thread helper{[&] {
            std::this_thread::sleep_for(100ms); // 让 open 进入阻塞
            finishedBeforeStop = finished.load();
            source.request_stop();              // cancel 钩子：uv_cancel 失败（请求在执行）
            std::this_thread::sleep_for(100ms); // 让钩子在 loop 线程上执行完
            // O_NONBLOCK 的写端只有在有读端（正在 open 或已打开）时才能成功（否则 ENXIO）。
            int const w = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
            writerOpened = w >= 0;
            if (w >= 0) ::close(w);
        }};
        auto r = uvco::run(loop, openResult(loop, fifo, UV_FS_O_RDONLY), source.get_token());
        finished = true;
        helper.join();
        CHECK(writerOpened);
        if (finishedBeforeStop) {
            CHECK(not ioUringDisabled()); // 只有 io_uring 才会不阻塞
            CHECK(r && r->isOpen());
        } else {
            CHECK(not r && r.error() == Errc::canceled);
            ++released;
        }
        r = Result<fs::File>{Errc::canceled};
    }
    if (ioUringDisabled()) CHECK(released == 3);
    std::fprintf(stderr, "late-cancel release path taken %d/3 times\n", released);
    CHECK(openFdCount() == base);
    CHECK(::unlink(fifo.c_str()) == 0);
}

auto readResult(fs::File& file, std::string& buf)
    CO2_BEG(co2::Task<Result<std::size_t>>, (file, buf), Result<std::size_t> n;) {
    CO2_AWAIT_SET(n, file.read(uvco::buffer(buf)));
    CO2_RETURN(n);
}
CO2_END

// 取消是尽力而为：阻塞在空 FIFO 上的 read 已在执行（线程池），或在 io_uring 里（uv_cancel
// 返回 EBUSY），取消之后它照常完成并报告真实结果（read 不产生需要释放的资源）。
TEST_CASE("fs: cancel of a running read is best-effort: the read completes with data") {
    std::string const fifo = path("fifo-read");
    static_cast<void>(::unlink(fifo.c_str()));
    CHECK(::mkfifo(fifo.c_str(), 0600) == 0);
    std::size_t const base = openFdCount();
    {
        uvco::Loop loop;
        auto f = uvco::run(loop, openResult(loop, fifo, UV_FS_O_RDWR)); // FIFO 的 O_RDWR 不阻塞
        CHECK(f);
        fs::File file = std::move(*f);
        std::string buf(16, '\0');
        co2::stop_source source;
        std::thread helper{[&] {
            std::this_thread::sleep_for(100ms);
            source.request_stop();
            std::this_thread::sleep_for(100ms);
            int const w = ::open(fifo.c_str(), O_WRONLY | O_NONBLOCK);
            CHECK(w >= 0);
            CHECK(::write(w, "ping", 4) == 4);
            ::close(w);
        }};
        auto const n = uvco::run(loop, readResult(file, buf), source.get_token());
        helper.join();
        CHECK(n && *n == 4U && buf.compare(0, 4, "ping") == 0);
    }
    CHECK(openFdCount() == base);
    CHECK(::unlink(fifo.c_str()) == 0);
}

TEST_CASE("fs: release hooks of mkstemp / mkdtemp close and remove what they made") {
    std::size_t const base = openFdCount();
    std::string const tpl = path("rel-XXXXXX");
    uv_fs_t req;
    CHECK(uv_fs_mkstemp(nullptr, &req, tpl.c_str(), nullptr) >= 0);
    std::string const made{req.path};
    uv_file const fd = static_cast<uv_file>(req.result);
    CHECK(exists(made) && openFdCount() == base + 1);
    fs::detail::MkstempSpec::release(req);
    uv_fs_req_cleanup(&req);
    CHECK(not exists(made));
    CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(openFdCount() == base);

    CHECK(uv_fs_mkdtemp(nullptr, &req, tpl.c_str(), nullptr) == 0);
    std::string const dir{req.path};
    CHECK(exists(dir));
    fs::detail::MkdtempSpec::release(req);
    uv_fs_req_cleanup(&req);
    CHECK(not exists(dir));
}

TEST_CASE("fs: ~Loop drives an in-flight request to completion (no abort for requests)") {
    bool resumed = false;
    std::unique_ptr<uvco::Loop> loop{new uvco::Loop};
    auto handle = co2::spawn(*loop, statSetsFlag(*loop, UVCO_TEST_TMP_DIR, resumed));
    static_cast<void>(uv_run(loop->raw(), UV_RUN_NOWAIT)); // 发起 stat
    loop.reset();
    CHECK(resumed);
    CHECK(handle.isReady());
    CHECK(handle.join());
}

auto closeStopped(fs::CloseOp& op)
 CO2_BEG(co2::Task<Result<void>>, (op), Result<void> r;) {
 CO2_AWAIT_SET(r,op); CO2_RETURN(r);
} CO2_END
TEST_CASE("fs regression: close retains ownership and ignores stop") {
 uvco::Loop loop; int fd=::open("/dev/null",O_RDONLY); CHECK(fd>=0);
 fs::File file{loop,fd}; auto op=file.close(); co2::stop_source stop;stop.request_stop();
 auto r=uvco::run(loop,closeStopped(op),stop.get_token()); CHECK(r);
 bool leaked=::fcntl(fd,F_GETFD)!=-1;
 if(leaked) ::close(fd);
 CHECK(not leaked);
}
TEST_CASE("fs regression: unawaited close owns fd") {
 uvco::Loop loop; int fd=::open("/dev/null",O_RDONLY); CHECK(fd>=0);
 {fs::File file{loop,fd};auto op=file.close();}
 bool leaked=::fcntl(fd,F_GETFD)!=-1;if(leaked)::close(fd); CHECK(not leaked);
}
namespace {
struct ThrowProject {
 using value_type=std::string;
 std::string path;
 int submit(uv_loop_t* l,uv_fs_t* r,uv_fs_cb cb)const{return uv_fs_stat(l,r,path.c_str(),cb);}
 static std::string project(uv_fs_t&,uvco::Loop&){throw std::bad_alloc{};}
};
using ThrowOp=fs::FsOp<ThrowProject>;
auto projectFail(ThrowOp& op)
 CO2_BEG(co2::Task<Result<std::string>>, (op),Result<std::string> r;) {
 CO2_AWAIT_SET(r,op);CO2_RETURN(r);
} CO2_END
}
TEST_CASE("fs regression: bad_alloc in callback completes as no_memory and cleans request") {
 uvco::Loop loop;ThrowOp op{loop,std::string{UVCO_TEST_TMP_DIR}};
 auto r=uvco::run(loop,projectFail(op));CHECK(not r && r.error()==Errc::no_memory);
}
namespace {
co2::stop_source* stopAtCallback=nullptr;
uv_fs_cb nextCallback=nullptr;
std::string callbackPath;
struct StopAtDoneSpec : fs::detail::MkstempSpec {
 StopAtDoneSpec(std::string p){tpl=std::move(p);}
 int submit(uv_loop_t* l,uv_fs_t* r,uv_fs_cb cb)const {
  nextCallback=cb;
  return uv_fs_mkstemp(l,r,tpl.c_str(),[](uv_fs_t* req){
   callbackPath=req->path;stopAtCallback->request_stop();nextCallback(req);
  });
 }
};
using StopAtDoneOp=fs::FsOp<StopAtDoneSpec>;
auto stopAtDone(StopAtDoneOp& op)
 CO2_BEG(co2::Task<Result<fs::TempFile>>, (op),Result<fs::TempFile> r;) {
 CO2_AWAIT_SET(r,op);CO2_RETURN(std::move(r));
} CO2_END
}
TEST_CASE("fs regression: stop already requested before completion releases without dispatch") {
 uvco::Loop loop;co2::stop_source stop;stopAtCallback=&stop;
 auto base=openFdCount();StopAtDoneOp op{loop,path("late-XXXXXX")};
 auto r=uvco::run(loop,stopAtDone(op),stop.get_token());
 bool canceled=not r && r.error()==Errc::canceled;
 if(r){r->file.reset();::unlink(r->path.c_str());}
 CHECK(canceled);CHECK(not exists(callbackPath));CHECK(openFdCount()==base);
}
namespace {
std::string failedTempPath;
struct ThrowTempProject : fs::detail::MkstempSpec {
 explicit ThrowTempProject(std::string p){tpl=std::move(p);}
 static fs::TempFile project(uv_fs_t& r,uvco::Loop&){failedTempPath=r.path;throw std::bad_alloc{};}
};
using ThrowTempOp=fs::FsOp<ThrowTempProject>;
auto tempProjectFail(ThrowTempOp& op)
 CO2_BEG(co2::Task<Result<fs::TempFile>>, (op),Result<fs::TempFile> r;) {
 CO2_AWAIT_SET(r,op);CO2_RETURN(std::move(r));
} CO2_END
struct OtherProject : ThrowProject {
 explicit OtherProject(std::string p){path=std::move(p);}
 static std::string project(uv_fs_t&,uvco::Loop&){throw 42;}
};
using OtherOp=fs::FsOp<OtherProject>;
auto otherProjectFail(OtherOp& op)
 CO2_BEG(co2::Task<Result<std::string>>, (op),Result<std::string> r;) {
 CO2_AWAIT_SET(r,op);CO2_RETURN(r);
} CO2_END
}
TEST_CASE("fs: throwing resource projection releases fd and temp path") {
 uvco::Loop loop;auto count=openFdCount();ThrowTempOp op{loop,path("throw-XXXXXX")};
 auto r=uvco::run(loop,tempProjectFail(op));CHECK(not r && r.error()==Errc::no_memory);
 CHECK(not exists(failedTempPath));CHECK(openFdCount()==count);
}
TEST_CASE("fs: non-allocation callback exception rethrows at await_resume") {
 uvco::Loop loop;OtherOp op{loop,std::string{UVCO_TEST_TMP_DIR}};
 bool caught=false;try {static_cast<void>(uvco::run(loop,otherProjectFail(op)));}catch(int n){caught=n==42;}
 CHECK(caught);
}
int main() {
    int const rc = ::uvco_test::runAll();
    static_cast<void>(::rmdir(scratch().c_str()));
    return rc;
}
